// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	Movement.cpp: Character movement implementation

=============================================================================*/

#include "VRCharacterMovementComponent.h"
#include "GameFramework/PhysicsVolume.h"
#include "GameFramework/GameNetworkManager.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameState.h"
#include "Components/PrimitiveComponent.h"
#include "Animation/AnimMontage.h"
#include "DrawDebugHelpers.h"
//#include "PhysicsEngine/DestructibleActor.h"
#include "VRCharacter.h"
#include "VRExpansionFunctionLibrary.h"

// @todo this is here only due to circular dependency to AIModule. To be removed
#include "Navigation/PathFollowingComponent.h"
#include "AI/Navigation/AvoidanceManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/BrushComponent.h"
//#include "Components/DestructibleComponent.h"

#include "Engine/DemoNetDriver.h"
#include "Engine/NetworkObjectList.h"

//#include "PerfCountersHelpers.h"

DEFINE_LOG_CATEGORY(LogVRCharacterMovement);

/**
 * Character stats
 */
DECLARE_CYCLE_STAT(TEXT("Char StepUp"), STAT_CharStepUp, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char FindFloor"), STAT_CharFindFloor, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char ReplicateMoveToServer"), STAT_CharacterMovementReplicateMoveToServer, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char CallServerMove"), STAT_CharacterMovementCallServerMove, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char CombineNetMove"), STAT_CharacterMovementCombineNetMove, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char PhysWalking"), STAT_CharPhysWalking, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char PhysFalling"), STAT_CharPhysFalling, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char PhysNavWalking"), STAT_CharPhysNavWalking, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char NavProjectPoint"), STAT_CharNavProjectPoint, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char NavProjectLocation"), STAT_CharNavProjectLocation, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char AdjustFloorHeight"), STAT_CharAdjustFloorHeight, STATGROUP_Character);
DECLARE_CYCLE_STAT(TEXT("Char ProcessLanded"), STAT_CharProcessLanded, STATGROUP_Character);

// MAGIC NUMBERS
const float MAX_STEP_SIDE_Z = 0.08f;	// maximum z value for the normal on the vertical side of steps
const float SWIMBOBSPEED = -80.f;
const float VERTICAL_SLOPE_NORMAL_Z = 0.001f; // Slope is vertical if Abs(Normal.Z) <= this threshold. Accounts for precision problems that sometimes angle normals slightly off horizontal for vertical surface.

// Defines for build configs
#if DO_CHECK && !UE_BUILD_SHIPPING // Disable even if checks in shipping are enabled.
#define devCode( Code )		checkCode( Code )
#else
#define devCode(...)
#endif

// Statics
namespace CharacterMovementComponentStatics
{
	static const FName CrouchTraceName = FName(TEXT("CrouchTrace"));
	static const FName ImmersionDepthName = FName(TEXT("MovementComp_Character_ImmersionDepth"));

	static float fRotationCorrectionThreshold = 0.02f;
	FAutoConsoleVariableRef CVarRotationCorrectionThreshold(
		TEXT("vre.RotationCorrectionThreshold"),
		fRotationCorrectionThreshold,
		TEXT("Error threshold value before correcting a clients rotation.\n")
		TEXT("Rotation is replicated at 2 decimal precision, so values less than 0.01 won't matter."),
		ECVF_Default);

}

void UVRCharacterMovementComponent::Crouch(bool bClientSimulation)
{
	if (!HasValidData())
	{
		return;
	}

	if (!bClientSimulation && !CanCrouchInCurrentState())
	{
		return;
	}

	// See if collision is already at desired size.
	if (CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() == CrouchedHalfHeight)
	{
		if (!bClientSimulation)
		{
			CharacterOwner->bIsCrouched = true;
		}
		CharacterOwner->OnStartCrouch(0.f, 0.f);
		return;
	}

	if (bClientSimulation && CharacterOwner->Role == ROLE_SimulatedProxy)
	{
		// restore collision size before crouching
		ACharacter* DefaultCharacter = CharacterOwner->GetClass()->GetDefaultObject<ACharacter>();
		if (VRRootCapsule)
			VRRootCapsule->SetCapsuleSizeVR(DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight());
		else
			CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight());

		bShrinkProxyCapsule = true;
	}

	// Change collision size to crouching dimensions
	const float ComponentScale = CharacterOwner->GetCapsuleComponent()->GetShapeScale();
	const float OldUnscaledHalfHeight = CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float OldUnscaledRadius = CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleRadius();
	// Height is not allowed to be smaller than radius.
	const float ClampedCrouchedHalfHeight = FMath::Max3(0.f, OldUnscaledRadius, CrouchedHalfHeight);

	if (VRRootCapsule)
		VRRootCapsule->SetCapsuleSizeVR(OldUnscaledRadius, ClampedCrouchedHalfHeight);
	else
		CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(OldUnscaledRadius, ClampedCrouchedHalfHeight);

	float HalfHeightAdjust = (OldUnscaledHalfHeight - ClampedCrouchedHalfHeight);
	float ScaledHalfHeightAdjust = HalfHeightAdjust * ComponentScale;

	if (!bClientSimulation)
	{
		// Crouching to a larger height? (this is rare)
		if (ClampedCrouchedHalfHeight > OldUnscaledHalfHeight)
		{
			FCollisionQueryParams CapsuleParams(CharacterMovementComponentStatics::CrouchTraceName, false, CharacterOwner);
			FCollisionResponseParams ResponseParam;
			InitCollisionParams(CapsuleParams, ResponseParam);

			FVector capLocation;
			if (VRRootCapsule)
			{
				capLocation = VRRootCapsule->OffsetComponentToWorld.GetLocation();
			}
			else
				capLocation = UpdatedComponent->GetComponentLocation();

			const bool bEncroached = GetWorld()->OverlapBlockingTestByChannel(capLocation - FVector(0.f, 0.f, ScaledHalfHeightAdjust), FQuat::Identity,
				UpdatedComponent->GetCollisionObjectType(), GetPawnCapsuleCollisionShape(SHRINK_None), CapsuleParams, ResponseParam);

			// If encroached, cancel
			if (bEncroached)
			{
				if (VRRootCapsule)
					VRRootCapsule->SetCapsuleSizeVR(OldUnscaledRadius, OldUnscaledHalfHeight);
				else
					CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(OldUnscaledRadius, OldUnscaledHalfHeight);
				return;
			}
		}

		// Skipping this move down as the VR character's base root doesn't behave the same
		if (bCrouchMaintainsBaseLocation)
		{
			// Intentionally not using MoveUpdatedComponent, where a horizontal plane constraint would prevent the base of the capsule from staying at the same spot.
			//UpdatedComponent->MoveComponent(FVector(0.f, 0.f, -ScaledHalfHeightAdjust), UpdatedComponent->GetComponentQuat(), true, nullptr, EMoveComponentFlags::MOVECOMP_NoFlags, ETeleportType::TeleportPhysics);
		}

		CharacterOwner->bIsCrouched = true;
	}

	bForceNextFloorCheck = true;

	// OnStartCrouch takes the change from the Default size, not the current one (though they are usually the same).
	const float MeshAdjust = ScaledHalfHeightAdjust;
	ACharacter* DefaultCharacter = CharacterOwner->GetClass()->GetDefaultObject<ACharacter>();
	HalfHeightAdjust = (DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - ClampedCrouchedHalfHeight);
	ScaledHalfHeightAdjust = HalfHeightAdjust * ComponentScale;

	AdjustProxyCapsuleSize();
	CharacterOwner->OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

	// Don't smooth this change in mesh position
	if (bClientSimulation && CharacterOwner->Role == ROLE_SimulatedProxy)
	{
		FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
		if (ClientData && ClientData->MeshTranslationOffset.Z != 0.f)
		{
			ClientData->MeshTranslationOffset -= FVector(0.f, 0.f, MeshAdjust);
			ClientData->OriginalMeshTranslationOffset = ClientData->MeshTranslationOffset;
		}
	}
}

void UVRCharacterMovementComponent::UnCrouch(bool bClientSimulation)
{
	if (!HasValidData())
	{
		return;
	}

	ACharacter* DefaultCharacter = CharacterOwner->GetClass()->GetDefaultObject<ACharacter>();

	// See if collision is already at desired size.
	if (CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() == DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight())
	{
		if (!bClientSimulation)
		{
			CharacterOwner->bIsCrouched = false;
		}
		CharacterOwner->OnEndCrouch(0.f, 0.f);
		return;
	}

	const float CurrentCrouchedHalfHeight = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

	const float ComponentScale = CharacterOwner->GetCapsuleComponent()->GetShapeScale();
	const float OldUnscaledHalfHeight = CharacterOwner->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float HalfHeightAdjust = DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - OldUnscaledHalfHeight;
	const float ScaledHalfHeightAdjust = HalfHeightAdjust * ComponentScale;

	/*const*/ FVector PawnLocation = UpdatedComponent->GetComponentLocation();

	if (VRRootCapsule)
	{
		PawnLocation = VRRootCapsule->OffsetComponentToWorld.GetLocation();
	}

	// Grow to uncrouched size.
	check(CharacterOwner->GetCapsuleComponent());

	if (!bClientSimulation)
	{
		// Try to stay in place and see if the larger capsule fits. We use a slightly taller capsule to avoid penetration.
		const UWorld* MyWorld = GetWorld();
		const float SweepInflation = KINDA_SMALL_NUMBER * 10.f;
		FCollisionQueryParams CapsuleParams(CharacterMovementComponentStatics::CrouchTraceName, false, CharacterOwner);
		FCollisionResponseParams ResponseParam;
		InitCollisionParams(CapsuleParams, ResponseParam);

		// Compensate for the difference between current capsule size and standing size
		const FCollisionShape StandingCapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_HeightCustom, -SweepInflation - ScaledHalfHeightAdjust); // Shrink by negative amount, so actually grow it.
		const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();
		bool bEncroached = true;

		if (!bCrouchMaintainsBaseLocation)
		{
			// Expand in place
			bEncroached = MyWorld->OverlapBlockingTestByChannel(PawnLocation, FQuat::Identity, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);

			if (bEncroached)
			{
				// Try adjusting capsule position to see if we can avoid encroachment.
				if (ScaledHalfHeightAdjust > 0.f)
				{
					// Shrink to a short capsule, sweep down to base to find where that would hit something, and then try to stand up from there.
					float PawnRadius, PawnHalfHeight;
					CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);
					const float ShrinkHalfHeight = PawnHalfHeight - PawnRadius;
					const float TraceDist = PawnHalfHeight - ShrinkHalfHeight;
					const FVector Down = FVector(0.f, 0.f, -TraceDist);

					FHitResult Hit(1.f);
					const FCollisionShape ShortCapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_HeightCustom, ShrinkHalfHeight);
					const bool bBlockingHit = MyWorld->SweepSingleByChannel(Hit, PawnLocation, PawnLocation + Down, FQuat::Identity, CollisionChannel, ShortCapsuleShape, CapsuleParams);
					if (Hit.bStartPenetrating)
					{
						bEncroached = true;
					}
					else
					{
						// Compute where the base of the sweep ended up, and see if we can stand there
						const float DistanceToBase = (Hit.Time * TraceDist) + ShortCapsuleShape.Capsule.HalfHeight;
						const FVector NewLoc = FVector(PawnLocation.X, PawnLocation.Y, PawnLocation.Z - DistanceToBase + StandingCapsuleShape.Capsule.HalfHeight + SweepInflation + MIN_FLOOR_DIST / 2.f);
						bEncroached = MyWorld->OverlapBlockingTestByChannel(NewLoc, FQuat::Identity, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
						if (!bEncroached)
						{
							// Intentionally not using MoveUpdatedComponent, where a horizontal plane constraint would prevent the base of the capsule from staying at the same spot.
							UpdatedComponent->MoveComponent(NewLoc - PawnLocation, UpdatedComponent->GetComponentQuat(), false, nullptr, EMoveComponentFlags::MOVECOMP_NoFlags, ETeleportType::TeleportPhysics);
						}
					}
				}
			}
		}
		else
		{
			// Expand while keeping base location the same.
			FVector StandingLocation = PawnLocation + FVector(0.f, 0.f, StandingCapsuleShape.GetCapsuleHalfHeight() - CurrentCrouchedHalfHeight);
			bEncroached = MyWorld->OverlapBlockingTestByChannel(StandingLocation, FQuat::Identity, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);

			if (bEncroached)
			{
				if (IsMovingOnGround())
				{
					// Something might be just barely overhead, try moving down closer to the floor to avoid it.
					const float MinFloorDist = KINDA_SMALL_NUMBER * 10.f;
					if (CurrentFloor.bBlockingHit && CurrentFloor.FloorDist > MinFloorDist)
					{
						StandingLocation.Z -= CurrentFloor.FloorDist - MinFloorDist;
						bEncroached = MyWorld->OverlapBlockingTestByChannel(StandingLocation, FQuat::Identity, CollisionChannel, StandingCapsuleShape, CapsuleParams, ResponseParam);
					}
				}
			}

			// Canceling this move because our VR capsule isn't actor based like it expects it to be
			if (!bEncroached)
			{
				// Commit the change in location.
				//UpdatedComponent->MoveComponent(StandingLocation - PawnLocation, UpdatedComponent->GetComponentQuat(), false, nullptr, EMoveComponentFlags::MOVECOMP_NoFlags, ETeleportType::TeleportPhysics);
				bForceNextFloorCheck = true;
			}
		}

		// If still encroached then abort.
		if (bEncroached)
		{
			return;
		}

		CharacterOwner->bIsCrouched = false;
	}
	else
	{
		bShrinkProxyCapsule = true;
	}

	// Now call SetCapsuleSize() to cause touch/untouch events and actually grow the capsule
	if (VRRootCapsule)
		VRRootCapsule->SetCapsuleSizeVR(DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), true);
	else
		CharacterOwner->GetCapsuleComponent()->SetCapsuleSize(DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleRadius(), DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), true);

	const float MeshAdjust = ScaledHalfHeightAdjust;
	AdjustProxyCapsuleSize();
	CharacterOwner->OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);

	// Don't smooth this change in mesh position
	if (bClientSimulation && CharacterOwner->Role == ROLE_SimulatedProxy)
	{
		FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
		if (ClientData && ClientData->MeshTranslationOffset.Z != 0.f)
		{
			ClientData->MeshTranslationOffset += FVector(0.f, 0.f, MeshAdjust);
			ClientData->OriginalMeshTranslationOffset = ClientData->MeshTranslationOffset;
		}
	}
}

FNetworkPredictionData_Client* UVRCharacterMovementComponent::GetPredictionData_Client() const
{
	// Should only be called on client or listen server (for remote clients) in network games
	check(CharacterOwner != NULL);
	checkSlow(CharacterOwner->Role < ROLE_Authority || (CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy && GetNetMode() == NM_ListenServer));
	checkSlow(GetNetMode() == NM_Client || GetNetMode() == NM_ListenServer);

	if (!ClientPredictionData)
	{
		UVRCharacterMovementComponent* MutableThis = const_cast<UVRCharacterMovementComponent*>(this);
		MutableThis->ClientPredictionData = new FNetworkPredictionData_Client_VRCharacter(*this);
	}

	return ClientPredictionData;
}

FNetworkPredictionData_Server* UVRCharacterMovementComponent::GetPredictionData_Server() const
{
	// Should only be called on server in network games
	check(CharacterOwner != NULL);
	check(CharacterOwner->Role == ROLE_Authority);
	checkSlow(GetNetMode() < NM_Client);

	if (!ServerPredictionData)
	{
		UVRCharacterMovementComponent* MutableThis = const_cast<UVRCharacterMovementComponent*>(this);
		MutableThis->ServerPredictionData = new FNetworkPredictionData_Server_VRCharacter(*this);
	}

	return ServerPredictionData;
}

void FSavedMove_VRCharacter::SetInitialPosition(ACharacter* C)
{

	// See if we can get the VR capsule location
	if (AVRCharacter * VRC = Cast<AVRCharacter>(C))
	{
		UVRCharacterMovementComponent * CharMove = Cast<UVRCharacterMovementComponent>(VRC->GetCharacterMovement());
		if (VRC->VRRootReference)
		{
			VRCapsuleLocation = VRC->VRRootReference->curCameraLoc;
			VRCapsuleRotation = UVRExpansionFunctionLibrary::GetHMDPureYaw_I(VRC->VRRootReference->curCameraRot);
			LFDiff = VRC->VRRootReference->DifferenceFromLastFrame;
		}
		else
		{
			VRCapsuleLocation = FVector::ZeroVector;
			VRCapsuleRotation = FRotator::ZeroRotator;
			LFDiff = FVector::ZeroVector;
		}
	}

	FSavedMove_VRBaseCharacter::SetInitialPosition(C);
}

void FSavedMove_VRCharacter::PrepMoveFor(ACharacter* Character)
{
	UVRCharacterMovementComponent * CharMove = Cast<UVRCharacterMovementComponent>(Character->GetCharacterMovement());

	// Set capsule location prior to testing movement
	// I am overriding the replicated value here when movement is made on purpose
	if (CharMove && CharMove->VRRootCapsule)
	{
		CharMove->VRRootCapsule->curCameraLoc = this->VRCapsuleLocation;
		CharMove->VRRootCapsule->curCameraRot = this->VRCapsuleRotation;//FRotator(0.0f, FRotator::DecompressAxisFromByte(CapsuleYaw), 0.0f);
		CharMove->VRRootCapsule->DifferenceFromLastFrame = FVector(LFDiff.X, LFDiff.Y, 0.0f);
		CharMove->AdditionalVRInputVector = CharMove->VRRootCapsule->DifferenceFromLastFrame;

		if (AVRBaseCharacter * BaseChar = Cast<AVRBaseCharacter>(CharMove->GetCharacterOwner()))
		{
			if (BaseChar->VRReplicateCapsuleHeight && this->LFDiff.Z > 0.0f && !FMath::IsNearlyEqual(this->LFDiff.Z, CharMove->VRRootCapsule->GetUnscaledCapsuleHalfHeight()))
			{
				BaseChar->SetCharacterHalfHeightVR(LFDiff.Z, false);
				//CharMove->VRRootCapsule->SetCapsuleHalfHeight(this->LFDiff.Z, false);
			}
		}

		CharMove->VRRootCapsule->GenerateOffsetToWorld(false, false);
	}

	FSavedMove_VRBaseCharacter::PrepMoveFor(Character);
}

bool UVRCharacterMovementComponent::ServerMoveVR_Validate(float TimeStamp, FVector_NetQuantize10 InAccel, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 MoveFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	return true;
}

bool UVRCharacterMovementComponent::ServerMoveVRExLight_Validate(float TimeStamp, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 MoveFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	return true;
}

bool UVRCharacterMovementComponent::ServerMoveVRDual_Validate(float TimeStamp0, FVector_NetQuantize10 InAccel0, uint8 PendingFlags, uint32 View0, FVector_NetQuantize100 OldCapsuleLoc, FVRConditionalMoveRep OldConditionalReps, FVector_NetQuantize100 OldLFDiff, uint16 OldCapsuleYaw, float TimeStamp, FVector_NetQuantize10 InAccel, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 NewFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	return true;
}

bool UVRCharacterMovementComponent::ServerMoveVRDualExLight_Validate(float TimeStamp0, uint8 PendingFlags, uint32 View0, FVector_NetQuantize100 OldCapsuleLoc, FVRConditionalMoveRep OldConditionalReps, FVector_NetQuantize100 OldLFDiff, uint16 OldCapsuleYaw, float TimeStamp, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 NewFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	return true;
}

bool UVRCharacterMovementComponent::ServerMoveVRDualHybridRootMotion_Validate(float TimeStamp0, FVector_NetQuantize10 InAccel0, uint8 PendingFlags,  uint32 View0, FVector_NetQuantize100 OldCapsuleLoc, FVRConditionalMoveRep OldConditionalReps, FVector_NetQuantize100 OldLFDiff, uint16 OldCapsuleYaw, float TimeStamp, FVector_NetQuantize10 InAccel, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 NewFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	return true;
}

void UVRCharacterMovementComponent::ServerMoveVRDualHybridRootMotion_Implementation(
	float TimeStamp0,
	FVector_NetQuantize10 InAccel0,
	uint8 PendingFlags,
	uint32 View0,
	FVector_NetQuantize100 OldCapsuleLoc,
	FVRConditionalMoveRep OldConditionalReps,
	FVector_NetQuantize100 OldLFDiff,
	uint16 OldCapsuleYaw,
	float TimeStamp,
	FVector_NetQuantize10 InAccel,
	FVector_NetQuantize100 ClientLoc,
	FVector_NetQuantize100 CapsuleLoc,
	FVRConditionalMoveRep ConditionalReps,
	FVector_NetQuantize100 LFDiff,
	uint16 CapsuleYaw,
	uint8 NewFlags,
	FVRConditionalMoveRep2 MoveReps,
	uint8 ClientMovementMode)
{

	// Keep new moves base and bone, but use the View of the old move
	FVRConditionalMoveRep2 MoveRepsOld;
	MoveRepsOld.ClientBaseBoneName = MoveReps.ClientBaseBoneName;
	MoveRepsOld.ClientMovementBase = MoveReps.ClientMovementBase;
	MoveRepsOld.UnpackAndSetINTRotations(View0);

	// Scope these, they nest with Outer references so it should work fine, this keeps the update rotation and move autonomous from double updating the char
	FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);
	// First move received didn't use root motion, process it as such.
	CharacterOwner->bServerMoveIgnoreRootMotion = CharacterOwner->IsPlayingNetworkedRootMotionMontage();
	ServerMoveVR_Implementation(TimeStamp0, InAccel0, FVector(1.f, 2.f, 3.f), OldCapsuleLoc, OldConditionalReps, OldLFDiff, OldCapsuleYaw, PendingFlags, MoveRepsOld, ClientMovementMode);
	CharacterOwner->bServerMoveIgnoreRootMotion = false;

	ServerMoveVR_Implementation(TimeStamp, InAccel, ClientLoc, CapsuleLoc, ConditionalReps, LFDiff, CapsuleYaw, NewFlags, MoveReps, ClientMovementMode);
}

void UVRCharacterMovementComponent::ServerMoveVRDual_Implementation(
	float TimeStamp0,
	FVector_NetQuantize10 InAccel0,
	uint8 PendingFlags,
	uint32 View0,
	FVector_NetQuantize100 OldCapsuleLoc, 
	FVRConditionalMoveRep OldConditionalReps,
	FVector_NetQuantize100 OldLFDiff,
	uint16 OldCapsuleYaw,
	float TimeStamp,
	FVector_NetQuantize10 InAccel,
	FVector_NetQuantize100 ClientLoc,
	FVector_NetQuantize100 CapsuleLoc,
	FVRConditionalMoveRep ConditionalReps,
	FVector_NetQuantize100 LFDiff,
	uint16 CapsuleYaw,
	uint8 NewFlags,
	FVRConditionalMoveRep2 MoveReps,
	uint8 ClientMovementMode)
{

	// Keep new moves base and bone, but use the View of the old move
	FVRConditionalMoveRep2 MoveRepsOld;
	MoveRepsOld.ClientBaseBoneName = MoveReps.ClientBaseBoneName;
	MoveRepsOld.ClientMovementBase = MoveReps.ClientMovementBase;
	MoveRepsOld.UnpackAndSetINTRotations(View0);

	// Scope these, they nest with Outer references so it should work fine, this keeps the update rotation and move autonomous from double updating the char
	FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);
	ServerMoveVR_Implementation(TimeStamp0, InAccel0, FVector(1.f, 2.f, 3.f), OldCapsuleLoc, OldConditionalReps, OldLFDiff, OldCapsuleYaw, PendingFlags, MoveRepsOld, ClientMovementMode);
	ServerMoveVR_Implementation(TimeStamp, InAccel, ClientLoc, CapsuleLoc, ConditionalReps, LFDiff, CapsuleYaw, NewFlags, MoveReps, ClientMovementMode);
}

void UVRCharacterMovementComponent::ServerMoveVRDualExLight_Implementation(
	float TimeStamp0,
	uint8 PendingFlags,
	uint32 View0,
	FVector_NetQuantize100 OldCapsuleLoc,
	FVRConditionalMoveRep OldConditionalReps,
	FVector_NetQuantize100 OldLFDiff,
	uint16 OldCapsuleYaw,
	float TimeStamp,
	FVector_NetQuantize100 ClientLoc,
	FVector_NetQuantize100 CapsuleLoc,
	FVRConditionalMoveRep ConditionalReps,
	FVector_NetQuantize100 LFDiff,
	uint16 CapsuleYaw,
	uint8 NewFlags,
	FVRConditionalMoveRep2 MoveReps,
	uint8 ClientMovementMode)
{

	// Keep new moves base and bone, but use the View of the old move
	FVRConditionalMoveRep2 MoveRepsOld;
	MoveRepsOld.ClientBaseBoneName = MoveReps.ClientBaseBoneName;
	MoveRepsOld.ClientMovementBase = MoveReps.ClientMovementBase;
	MoveRepsOld.UnpackAndSetINTRotations(View0);

	// Scope these, they nest with Outer references so it should work fine, this keeps the update rotation and move autonomous from double updating the char
	FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);
	ServerMoveVR_Implementation(TimeStamp0, FVector::ZeroVector, FVector(1.f, 2.f, 3.f), OldCapsuleLoc, OldConditionalReps, OldLFDiff, OldCapsuleYaw, PendingFlags,  MoveRepsOld, ClientMovementMode);
	ServerMoveVR_Implementation(TimeStamp, FVector::ZeroVector, ClientLoc, CapsuleLoc, ConditionalReps, LFDiff, CapsuleYaw, NewFlags, MoveReps, ClientMovementMode);
}

void UVRCharacterMovementComponent::ServerMoveVRExLight_Implementation(
	float TimeStamp,
	FVector_NetQuantize100 ClientLoc,
	FVector_NetQuantize100 CapsuleLoc,
	FVRConditionalMoveRep ConditionalReps,
	FVector_NetQuantize100 LFDiff,
	uint16 CapsuleYaw,
	uint8 MoveFlags,
	FVRConditionalMoveRep2 MoveReps,
	uint8 ClientMovementMode)
{
	ServerMoveVR_Implementation(TimeStamp, FVector::ZeroVector, ClientLoc, CapsuleLoc, ConditionalReps, LFDiff, CapsuleYaw, MoveFlags, MoveReps, ClientMovementMode);
}

void UVRCharacterMovementComponent::ServerMoveVR_Implementation(
	float TimeStamp,
	FVector_NetQuantize10 InAccel,
	FVector_NetQuantize100 ClientLoc,
	FVector_NetQuantize100 CapsuleLoc,
	FVRConditionalMoveRep ConditionalReps,
	FVector_NetQuantize100 LFDiff,
	uint16 CapsuleYaw,
	uint8 MoveFlags,
	FVRConditionalMoveRep2 MoveReps,
	uint8 ClientMovementMode)
{
	if (!HasValidData() || !IsComponentTickEnabled())
	{
		return;
	}

	FNetworkPredictionData_Server_Character* ServerData = GetPredictionData_Server_Character();
	check(ServerData);

	if (!VerifyClientTimeStamp(TimeStamp, *ServerData))
	{
		return;
	}

	// Scope these, they nest with Outer references so it should work fine, this keeps the update rotation and move autonomous from double updating the char
	FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);

	bool bServerReadyForClient = true;
	APlayerController* PC = Cast<APlayerController>(CharacterOwner->GetController());
	if (PC)
	{
		bServerReadyForClient = PC->NotifyServerReceivedClientData(CharacterOwner, TimeStamp);
		if (!bServerReadyForClient)
		{
			InAccel = FVector::ZeroVector;
		}
	}

	// View components
	//const uint16 ViewPitch = (View & 65535);
	//const uint16 ViewYaw = ClientYaw;//(View >> 16);

	const FVector Accel = InAccel;
	// Save move parameters.
	const float DeltaTime = ServerData->GetServerMoveDeltaTime(TimeStamp, CharacterOwner->GetActorTimeDilation());

	const UWorld* MyWorld = GetWorld();
	ServerData->CurrentClientTimeStamp = TimeStamp;
	ServerData->ServerTimeStamp = MyWorld->GetTimeSeconds();
	ServerData->ServerTimeStampLastServerMove = ServerData->ServerTimeStamp;
	FRotator ViewRot;
	ViewRot.Pitch = FRotator::DecompressAxisFromShort(MoveReps.ClientPitch);
	ViewRot.Yaw = FRotator::DecompressAxisFromShort(MoveReps.ClientYaw);
	ViewRot.Roll = FRotator::DecompressAxisFromByte(MoveReps.ClientRoll);

	if (PC && bUseClientControlRotation)
	{
		PC->SetControlRotation(ViewRot);
	}

	if (!bServerReadyForClient)
	{
		return;
	}

	// Perform actual movement
	if ((MyWorld->GetWorldSettings()->Pauser == NULL) && (DeltaTime > 0.f))
	{
		if (PC)
		{
			PC->UpdateRotation(DeltaTime);
		}

		if (!ConditionalReps.RequestedVelocity.IsZero())
		{
			RequestedVelocity = ConditionalReps.RequestedVelocity;
			bHasRequestedVelocity = true;
		}

		CustomVRInputVector = ConditionalReps.CustomVRInputVector;
		MoveActionArray = ConditionalReps.MoveActionArray;

		// Set capsule location prior to testing movement
		// I am overriding the replicated value here when movement is made on purpose
		if (VRRootCapsule)
		{
			VRRootCapsule->curCameraLoc = CapsuleLoc;
			VRRootCapsule->curCameraRot = FRotator(0.0f, FRotator::DecompressAxisFromShort(CapsuleYaw), 0.0f);
			VRRootCapsule->DifferenceFromLastFrame = FVector(LFDiff.X, LFDiff.Y, 0.0f);
			AdditionalVRInputVector = VRRootCapsule->DifferenceFromLastFrame;
		
			if (AVRBaseCharacter * BaseChar = Cast<AVRBaseCharacter>(CharacterOwner))
			{
				if (BaseChar->VRReplicateCapsuleHeight && LFDiff.Z > 0.0f && !FMath::IsNearlyEqual(LFDiff.Z, VRRootCapsule->GetUnscaledCapsuleHalfHeight()))
				{
					BaseChar->SetCharacterHalfHeightVR(LFDiff.Z, false);
				//	BaseChar->ReplicatedCapsuleHeight.CapsuleHeight = LFDiff.Z;
					//VRRootCapsule->SetCapsuleHalfHeight(LFDiff.Z, false);
				}
			}

			VRRootCapsule->GenerateOffsetToWorld(false, false);

			// #TODO: Should I actually implement the mesh translation from "Crouch"? Generally people are going to be
			//	IKing any mesh from the HMD instead.
			/*
				// Don't smooth this change in mesh position
				if (bClientSimulation && CharacterOwner->Role == ROLE_SimulatedProxy)
				{
					FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
					if (ClientData && ClientData->MeshTranslationOffset.Z != 0.f)
					{
						ClientData->MeshTranslationOffset += FVector(0.f, 0.f, MeshAdjust);
						ClientData->OriginalMeshTranslationOffset = ClientData->MeshTranslationOffset;
					}
				}
			*/
		}

		MoveAutonomous(TimeStamp, DeltaTime, MoveFlags, Accel);
		bHasRequestedVelocity = false;
	}

	UE_LOG(LogVRCharacterMovement, Verbose, TEXT("ServerMove Time %f Acceleration %s Position %s DeltaTime %f"),
		TimeStamp, *Accel.ToString(), *UpdatedComponent->GetComponentLocation().ToString(), DeltaTime);

	ServerMoveHandleClientErrorVR(TimeStamp, DeltaTime, Accel, ClientLoc, ViewRot.Yaw, MoveReps.ClientMovementBase, MoveReps.ClientBaseBoneName, ClientMovementMode);
}


void UVRCharacterMovementComponent::CallServerMove
(
	const class FSavedMove_Character* NewCMove,
	const class FSavedMove_Character* OldCMove
	)
{

	// This is technically "safe", I know for sure that I am using my own FSavedMove
	// I would have like to not override any of this, but I need a lot more specific information about the pawn
	// So just setting flags in the FSaved Move doesn't cut it
	// I could see a problem if someone overrides this override though
	const FSavedMove_VRCharacter * NewMove = (const FSavedMove_VRCharacter *)NewCMove;
	const FSavedMove_VRCharacter * OldMove = (const FSavedMove_VRCharacter *)OldCMove;

	check(NewMove != nullptr);
	//uint32 ClientYawPitchINT = PackYawAndPitchTo32(NewMove->SavedControlRotation.Yaw, NewMove->SavedControlRotation.Pitch);
	//uint8 ClientRollBYTE = FRotator::CompressAxisToByte(NewMove->SavedControlRotation.Roll);
	const uint16 CapsuleYawShort = FRotator::CompressAxisToShort(NewMove->VRCapsuleRotation.Yaw);
	const uint16 ClientYawShort = FRotator::CompressAxisToShort(NewMove->SavedControlRotation.Yaw);

	// Determine if we send absolute or relative location
	UPrimitiveComponent* ClientMovementBase = NewMove->EndBase.Get();
	const FName ClientBaseBone = NewMove->EndBoneName;
	const FVector SendLocation = MovementBaseUtility::UseRelativeLocation(ClientMovementBase) ? NewMove->SavedRelativeLocation : NewMove->SavedLocation;

	// send old move if it exists
	if (OldMove)
	{
		ServerMoveOld(OldMove->TimeStamp, OldMove->Acceleration, OldMove->GetCompressedFlags());
	}

	// Pass these in here, don't pass in to old move, it will receive the new move values in dual operations
	// Will automatically not replicate them if movement base is nullptr (1 bit cost to check this)
	FVRConditionalMoveRep2 NewMoveConds;
	NewMoveConds.ClientMovementBase = ClientMovementBase;
	NewMoveConds.ClientBaseBoneName = ClientBaseBone;

	if (CharacterOwner && (CharacterOwner->bUseControllerRotationRoll || CharacterOwner->bUseControllerRotationPitch))
	{
		NewMoveConds.ClientPitch = FRotator::CompressAxisToShort(NewMove->SavedControlRotation.Pitch);
		NewMoveConds.ClientRoll = FRotator::CompressAxisToByte(NewMove->SavedControlRotation.Roll);
	}

	NewMoveConds.ClientYaw = FRotator::CompressAxisToShort(NewMove->SavedControlRotation.Yaw);

	FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
	if (const FSavedMove_Character* const PendingMove = ClientData->PendingMove.Get())
	{
		// This should send same as the uint16 because it uses a packedINT send by default for shorts
		//uint32 OldClientYawPitchINT = PackYawAndPitchTo32(ClientData->PendingMove->SavedControlRotation.Yaw, ClientData->PendingMove->SavedControlRotation.Pitch);

		uint32 cPitch = 0;
		if (CharacterOwner && (CharacterOwner->bUseControllerRotationPitch))
			cPitch = FRotator::CompressAxisToShort(ClientData->PendingMove->SavedControlRotation.Pitch);
		
		uint32 cYaw = FRotator::CompressAxisToShort(ClientData->PendingMove->SavedControlRotation.Yaw);

		// Switch the order of pitch and yaw to place Yaw in smallest value, this will cut down on rep cost since normally pitch is zero'd out in VR
		uint32 OldClientYawPitchINT = (cPitch << 16) | (cYaw);

		FSavedMove_VRCharacter* oldMove = (FSavedMove_VRCharacter*)ClientData->PendingMove.Get();
		const uint16 OldCapsuleYawShort = FRotator::CompressAxisToShort(oldMove->VRCapsuleRotation.Yaw);
		//const uint16 OldClientYawShort = FRotator::CompressAxisToShort(ClientData->PendingMove->SavedControlRotation.Yaw);


		// If we delayed a move without root motion, and our new move has root motion, send these through a special function, so the server knows how to process them.
		if ((PendingMove->RootMotionMontage == NULL) && (NewMove->RootMotionMontage != NULL))
		{
		// send two moves simultaneously
			ServerMoveVRDualHybridRootMotion
			(
				PendingMove->TimeStamp,
				PendingMove->Acceleration,
				PendingMove->GetCompressedFlags(),
				OldClientYawPitchINT,
				oldMove->VRCapsuleLocation,
				oldMove->ConditionalValues,
				oldMove->LFDiff,
				OldCapsuleYawShort,
				NewMove->TimeStamp,
				NewMove->Acceleration,
				SendLocation,
				NewMove->VRCapsuleLocation,
				NewMove->ConditionalValues,
				NewMove->LFDiff,
				CapsuleYawShort,
				NewMove->GetCompressedFlags(),
				NewMoveConds,
				NewMove->EndPackedMovementMode
			);
		}
		else // Not Hybrid root motion rpc
		{
			// send two moves simultaneously
			if (oldMove->Acceleration.IsZero() && NewMove->Acceleration.IsZero())
			{
				ServerMoveVRDualExLight
				(
					PendingMove->TimeStamp,
					PendingMove->GetCompressedFlags(),
					OldClientYawPitchINT,
					oldMove->VRCapsuleLocation,
					oldMove->ConditionalValues,
					oldMove->LFDiff,
					OldCapsuleYawShort,
					NewMove->TimeStamp,
					SendLocation,
					NewMove->VRCapsuleLocation,
					NewMove->ConditionalValues,
					NewMove->LFDiff,
					CapsuleYawShort,
					NewMove->GetCompressedFlags(),
					NewMoveConds,
					NewMove->EndPackedMovementMode
				);
			}
			else
			{
				ServerMoveVRDual
				(
					PendingMove->TimeStamp,
					PendingMove->Acceleration,
					PendingMove->GetCompressedFlags(),
					OldClientYawPitchINT,
					oldMove->VRCapsuleLocation,
					oldMove->ConditionalValues,
					oldMove->LFDiff,
					OldCapsuleYawShort,
					NewMove->TimeStamp,
					NewMove->Acceleration,
					SendLocation,
					NewMove->VRCapsuleLocation,
					NewMove->ConditionalValues,
					NewMove->LFDiff,
					CapsuleYawShort,
					NewMove->GetCompressedFlags(),
					NewMoveConds,
					NewMove->EndPackedMovementMode
				);
			}
		}
	}
	else
	{

		if (NewMove->Acceleration.IsZero())
		{
			ServerMoveVRExLight
			(
				NewMove->TimeStamp,
				SendLocation,
				NewMove->VRCapsuleLocation,
				NewMove->ConditionalValues,
				NewMove->LFDiff,
				CapsuleYawShort,
				NewMove->GetCompressedFlags(),
				NewMoveConds,
				NewMove->EndPackedMovementMode
			);
		}
		else
		{
			ServerMoveVR
			(
				NewMove->TimeStamp,
				NewMove->Acceleration,
				SendLocation,
				NewMove->VRCapsuleLocation,
				NewMove->ConditionalValues,
				NewMove->LFDiff,
				CapsuleYawShort,
				NewMove->GetCompressedFlags(),
				NewMoveConds,
				NewMove->EndPackedMovementMode
			);
		}
	}


	APlayerController* PC = Cast<APlayerController>(CharacterOwner->GetController());
	APlayerCameraManager* PlayerCameraManager = (PC ? PC->PlayerCameraManager : NULL);
	if (PlayerCameraManager != NULL && PlayerCameraManager->bUseClientSideCameraUpdates)
	{
		PlayerCameraManager->bShouldSendClientSideCameraUpdate = true;
	}
}


void UVRCharacterMovementComponent::ServerMoveVR(float TimeStamp, FVector_NetQuantize10 InAccel, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 CompressedMoveFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	((AVRCharacter*)CharacterOwner)->ServerMoveVR(TimeStamp, InAccel, ClientLoc, CapsuleLoc, ConditionalReps, LFDiff, CapsuleYaw, CompressedMoveFlags, MoveReps, ClientMovementMode);
}

void UVRCharacterMovementComponent::ServerMoveVRExLight(float TimeStamp, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 CompressedMoveFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	((AVRCharacter*)CharacterOwner)->ServerMoveVRExLight(TimeStamp, ClientLoc, CapsuleLoc, ConditionalReps, LFDiff, CapsuleYaw, CompressedMoveFlags, MoveReps, ClientMovementMode);
}

void UVRCharacterMovementComponent::ServerMoveVRDual(float TimeStamp0, FVector_NetQuantize10 InAccel0, uint8 PendingFlags, uint32 View0, FVector_NetQuantize100 OldCapsuleLoc, FVRConditionalMoveRep OldConditionalReps, FVector_NetQuantize100 OldLFDiff, uint16 OldCapsuleYaw, float TimeStamp, FVector_NetQuantize10 InAccel, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 NewFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	((AVRCharacter*)CharacterOwner)->ServerMoveVRDual(TimeStamp0, InAccel0, PendingFlags, View0, OldCapsuleLoc, OldConditionalReps, OldLFDiff, OldCapsuleYaw, TimeStamp, InAccel, ClientLoc, CapsuleLoc, ConditionalReps, LFDiff, CapsuleYaw, NewFlags, MoveReps, ClientMovementMode);
}

void UVRCharacterMovementComponent::ServerMoveVRDualExLight(float TimeStamp0, uint8 PendingFlags, uint32 View0, FVector_NetQuantize100 OldCapsuleLoc, FVRConditionalMoveRep OldConditionalReps, FVector_NetQuantize100 OldLFDiff, uint16 OldCapsuleYaw, float TimeStamp, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 NewFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	((AVRCharacter*)CharacterOwner)->ServerMoveVRDualExLight(TimeStamp0, PendingFlags, View0, OldCapsuleLoc, OldConditionalReps, OldLFDiff, OldCapsuleYaw, TimeStamp, ClientLoc, CapsuleLoc, ConditionalReps, LFDiff, CapsuleYaw, NewFlags, MoveReps, ClientMovementMode);
}

void UVRCharacterMovementComponent::ServerMoveVRDualHybridRootMotion(float TimeStamp0, FVector_NetQuantize10 InAccel0, uint8 PendingFlags, uint32 View0, FVector_NetQuantize100 OldCapsuleLoc, FVRConditionalMoveRep OldConditionalReps, FVector_NetQuantize100 OldLFDiff, uint16 OldCapsuleYaw, float TimeStamp, FVector_NetQuantize10 InAccel, FVector_NetQuantize100 ClientLoc, FVector_NetQuantize100 CapsuleLoc, FVRConditionalMoveRep ConditionalReps, FVector_NetQuantize100 LFDiff, uint16 CapsuleYaw, uint8 NewFlags, FVRConditionalMoveRep2 MoveReps, uint8 ClientMovementMode)
{
	((AVRCharacter*)CharacterOwner)->ServerMoveVRDualHybridRootMotion(TimeStamp0, InAccel0, PendingFlags, View0, OldCapsuleLoc, OldConditionalReps, OldLFDiff, OldCapsuleYaw, TimeStamp, InAccel, ClientLoc, CapsuleLoc, ConditionalReps, LFDiff, CapsuleYaw, NewFlags, MoveReps, ClientMovementMode);
}

bool UVRCharacterMovementComponent::ShouldCheckForValidLandingSpot(float DeltaTime, const FVector& Delta, const FHitResult& Hit) const
{
	// See if we hit an edge of a surface on the lower portion of the capsule.
	// In this case the normal will not equal the impact normal, and a downward sweep may find a walkable surface on top of the edge.
	if (Hit.Normal.Z > KINDA_SMALL_NUMBER && !Hit.Normal.Equals(Hit.ImpactNormal))
	{
		FVector PawnLocation = UpdatedComponent->GetComponentLocation();
		if (VRRootCapsule)
			PawnLocation = VRRootCapsule->OffsetComponentToWorld.GetLocation();

		if (IsWithinEdgeTolerance(PawnLocation, Hit.ImpactPoint, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius()))
		{
			return true;
		}
	}

	return false;
}

void UVRCharacterMovementComponent::PhysWalking(float deltaTime, int32 Iterations)
{
	SCOPE_CYCLE_COUNTER(STAT_CharPhysWalking);

	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	if (!CharacterOwner || (!CharacterOwner->Controller && !bRunPhysicsWithNoController && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && (CharacterOwner->Role != ROLE_SimulatedProxy)))
	{
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		return;
	}

	if (!UpdatedComponent->IsQueryCollisionEnabled())
	{
		SetMovementMode(MOVE_Walking);
		return;
	}

	devCode(ensureMsgf(!Velocity.ContainsNaN(), TEXT("PhysWalking: Velocity contains NaN before Iteration (%s)\n%s"), *GetPathNameSafe(this), *Velocity.ToString()));

	bJustTeleported = false;
	bool bCheckedFall = false;
	bool bTriedLedgeMove = false;
	float remainingTime = deltaTime;

	// Rewind the players position by the new capsule location
	RewindVRRelativeMovement();

	// Perform the move
	while ((remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations) && CharacterOwner && (CharacterOwner->Controller || bRunPhysicsWithNoController || HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity() || (CharacterOwner->Role == ROLE_SimulatedProxy)))
	{
		Iterations++;
		bJustTeleported = false;
		const float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;

		// Save current values
		UPrimitiveComponent * const OldBase = GetMovementBase();
		const FVector PreviousBaseLocation = (OldBase != NULL) ? OldBase->GetComponentLocation() : FVector::ZeroVector;
		const FVector OldLocation = UpdatedComponent->GetComponentLocation();

		// Used for ledge check
		FVector OldCapsuleLocation = VRRootCapsule ? VRRootCapsule->OffsetComponentToWorld.GetLocation() : OldLocation;

		const FFindFloorResult OldFloor = CurrentFloor;

		//RestorePreAdditiveRootMotionVelocity();
		//RestorePreAdditiveVRMotionVelocity();

		// Ensure velocity is horizontal.
		MaintainHorizontalGroundVelocity();
		const FVector OldVelocity = Velocity;
		Acceleration.Z = 0.f;

		// Apply acceleration
		if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
		{
			CalcVelocity(timeTick, GroundFriction, false, GetMaxBrakingDeceleration());
			devCode(ensureMsgf(!Velocity.ContainsNaN(), TEXT("PhysWalking: Velocity contains NaN after CalcVelocity (%s)\n%s"), *GetPathNameSafe(this), *Velocity.ToString()));
		}

		//ApplyRootMotionToVelocity(timeTick);
		ApplyVRMotionToVelocity(timeTick);

		devCode(ensureMsgf(!Velocity.ContainsNaN(), TEXT("PhysWalking: Velocity contains NaN after Root Motion application (%s)\n%s"), *GetPathNameSafe(this), *Velocity.ToString()));

		if (IsFalling())
		{
			// Root motion could have put us into Falling.
			// No movement has taken place this movement tick so we pass on full time/past iteration count
			StartNewPhysics(remainingTime + timeTick, Iterations - 1);
			return;
		}

		// Compute move parameters
		const FVector MoveVelocity = Velocity;

		const FVector Delta = timeTick * MoveVelocity;

		const bool bZeroDelta = Delta.IsNearlyZero();
		FStepDownResult StepDownResult;

		if (bZeroDelta)
		{
			remainingTime = 0.f;
			// TODO: Bugged currently
			/*if (VRRootCapsule && VRRootCapsule->bUseWalkingCollisionOverride)
			{

				FHitResult HitRes;
				FCollisionQueryParams Params("RelativeMovementSweep", false, GetOwner());
				FCollisionResponseParams ResponseParam;

				VRRootCapsule->InitSweepCollisionParams(Params, ResponseParam);
				Params.bFindInitialOverlaps = true;
				bool bWasBlockingHit = false;

				bWasBlockingHit = GetWorld()->SweepSingleByChannel(HitRes, VRRootCapsule->OffsetComponentToWorld.GetLocation(), VRRootCapsule->OffsetComponentToWorld.GetLocation() + VRRootCapsule->DifferenceFromLastFrame, FQuat(0.0f, 0.0f, 0.0f, 1.0f), VRRootCapsule->GetCollisionObjectType(), VRRootCapsule->GetCollisionShape(), Params, ResponseParam);
				
				const FVector GravDir(0.f, 0.f, -1.f);
				if (CanStepUp(HitRes) || (CharacterOwner->GetMovementBase() != NULL && CharacterOwner->GetMovementBase()->GetOwner() == HitRes.GetActor()))
					StepUp(GravDir,VRRootCapsule->DifferenceFromLastFrame.GetSafeNormal2D(), HitRes, &StepDownResult);
			}*/
		}
		else
		{
			// try to move forward
			MoveAlongFloor(MoveVelocity, timeTick, &StepDownResult);

			if (IsFalling())
			{
				// pawn decided to jump up
				const float DesiredDist = Delta.Size();
				if (DesiredDist > KINDA_SMALL_NUMBER)
				{
					const float ActualDist = (UpdatedComponent->GetComponentLocation() - OldLocation).Size2D();
					remainingTime += timeTick * (1.f - FMath::Min(1.f, ActualDist / DesiredDist));
				}
				RestorePreAdditiveVRMotionVelocity();
				StartNewPhysics(remainingTime, Iterations);
				return;
			}
			else if (IsSwimming()) //just entered water
			{
				RestorePreAdditiveVRMotionVelocity();
				StartSwimmingVR(OldCapsuleLocation, OldVelocity, timeTick, remainingTime, Iterations);
				return;
			}
		}

		// Update floor.
		// StepUp might have already done it for us.
		if (StepDownResult.bComputedFloor)
		{
			CurrentFloor = StepDownResult.FloorResult;
		}
		else
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, bZeroDelta, NULL);
		}

		// check for ledges here
		const bool bCheckLedges = !CanWalkOffLedges();
		if (bCheckLedges && !CurrentFloor.IsWalkableFloor())
		{
			// calculate possible alternate movement
			const FVector GravDir = FVector(0.f, 0.f, -1.f);
			const FVector NewDelta = bTriedLedgeMove ? FVector::ZeroVector : GetLedgeMove(OldCapsuleLocation, Delta, GravDir);
			if (!NewDelta.IsZero())
			{
				// first revert this move
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, false);

				// avoid repeated ledge moves if the first one fails
				bTriedLedgeMove = true;

				// Try new movement direction
				Velocity = NewDelta / timeTick;
				remainingTime += timeTick;
				RestorePreAdditiveVRMotionVelocity();
				continue;
			}
			else
			{
				// see if it is OK to jump
				// @todo collision : only thing that can be problem is that oldbase has world collision on
				bool bMustJump = bZeroDelta || (OldBase == NULL || (!OldBase->IsQueryCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if ((bMustJump || !bCheckedFall) && CheckFall(OldFloor, CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump))
				{
					RestorePreAdditiveVRMotionVelocity();
					return;
				}
				bCheckedFall = true;

				// revert this move
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, true);
				remainingTime = 0.f;
				RestorePreAdditiveVRMotionVelocity();
				break;
			}
		}
		else
		{
			// Validate the floor check
			if (CurrentFloor.IsWalkableFloor())
			{
				if (ShouldCatchAir(OldFloor, CurrentFloor))
				{
					RestorePreAdditiveVRMotionVelocity();
					CharacterOwner->OnWalkingOffLedge(OldFloor.HitResult.ImpactNormal, OldFloor.HitResult.Normal, OldLocation, timeTick);
					if (IsMovingOnGround())
					{
						// If still walking, then fall. If not, assume the user set a different mode they want to keep.
						StartFalling(Iterations, remainingTime, timeTick, Delta, OldLocation);
					}
					return;
				}

				AdjustFloorHeight();
				SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
			}
			else if (CurrentFloor.HitResult.bStartPenetrating && remainingTime <= 0.f)
			{
				// The floor check failed because it started in penetration
				// We do not want to try to move downward because the downward sweep failed, rather we'd like to try to pop out of the floor.
				FHitResult Hit(CurrentFloor.HitResult);
				Hit.TraceEnd = Hit.TraceStart + FVector(0.f, 0.f, MAX_FLOOR_DIST);
				const FVector RequestedAdjustment = GetPenetrationAdjustment(Hit);
				ResolvePenetration(RequestedAdjustment, Hit, UpdatedComponent->GetComponentQuat());
				bForceNextFloorCheck = true;
			}

			// check if just entered water
			if (IsSwimming())
			{
				RestorePreAdditiveVRMotionVelocity();
				StartSwimmingVR(OldCapsuleLocation, Velocity, timeTick, remainingTime, Iterations);
				return;
			}

			// See if we need to start falling.
			if (!CurrentFloor.IsWalkableFloor() && !CurrentFloor.HitResult.bStartPenetrating)
			{
				const bool bMustJump = bJustTeleported || bZeroDelta || (OldBase == NULL || (!OldBase->IsQueryCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if ((bMustJump || !bCheckedFall) && CheckFall(OldFloor, CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump))
				{
					RestorePreAdditiveVRMotionVelocity();
					return;
				}
				bCheckedFall = true;
			}
		}


		// Allow overlap events and such to change physics state and velocity
		if (IsMovingOnGround())
		{
			// Make velocity reflect actual move
			if (!bJustTeleported && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && timeTick >= MIN_TICK_TIME)
			{
				// TODO-RootMotionSource: Allow this to happen during partial override Velocity, but only set allowed axes?
				Velocity =((UpdatedComponent->GetComponentLocation() - OldLocation ) / timeTick);
				RestorePreAdditiveVRMotionVelocity();
			}
		}

		// If we didn't move at all this iteration then abort (since future iterations will also be stuck).
		if (UpdatedComponent->GetComponentLocation() == OldLocation)
		{
			RestorePreAdditiveVRMotionVelocity();
			remainingTime = 0.f;
			break;
		}
	}

	if (IsMovingOnGround())
	{
		MaintainHorizontalGroundVelocity();
	}
}

void UVRCharacterMovementComponent::CapsuleTouched(UPrimitiveComponent* OverlappedComp, AActor* Other, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (!bEnablePhysicsInteraction)
	{
		return;
	}

	if (OtherComp != NULL && OtherComp->IsAnySimulatingPhysics())
	{
		/*const*/FVector OtherLoc = OtherComp->GetComponentLocation();
		if (UVRRootComponent * rCap = Cast<UVRRootComponent>(OtherComp))
		{
			OtherLoc = rCap->OffsetComponentToWorld.GetLocation();
		}
		
		const FVector Loc = VRRootCapsule->OffsetComponentToWorld.GetLocation();//UpdatedComponent->GetComponentLocation();
		FVector ImpulseDir = FVector(OtherLoc.X - Loc.X, OtherLoc.Y - Loc.Y, 0.25f).GetSafeNormal();
		ImpulseDir = (ImpulseDir + Velocity.GetSafeNormal2D()) * 0.5f;
		ImpulseDir.Normalize();

		FName BoneName = NAME_None;
		if (OtherBodyIndex != INDEX_NONE)
		{
			BoneName = ((USkinnedMeshComponent*)OtherComp)->GetBoneName(OtherBodyIndex);
		}

		float TouchForceFactorModified = TouchForceFactor;

		if (bTouchForceScaledToMass)
		{
			FBodyInstance* BI = OtherComp->GetBodyInstance(BoneName);
			TouchForceFactorModified *= BI ? BI->GetBodyMass() : 1.0f;
		}

		float ImpulseStrength = FMath::Clamp(Velocity.Size2D() * TouchForceFactorModified,
			MinTouchForce > 0.0f ? MinTouchForce : -FLT_MAX,
			MaxTouchForce > 0.0f ? MaxTouchForce : FLT_MAX);

		FVector Impulse = ImpulseDir * ImpulseStrength;

		OtherComp->AddImpulse(Impulse, BoneName);
	}
}


void UVRCharacterMovementComponent::ReplicateMoveToServer(float DeltaTime, const FVector& NewAcceleration)
{
	SCOPE_CYCLE_COUNTER(STAT_CharacterMovementReplicateMoveToServer);
	check(CharacterOwner != NULL);

	// Can only start sending moves if our controllers are synced up over the network, otherwise we flood the reliable buffer.
	APlayerController* PC = Cast<APlayerController>(CharacterOwner->GetController());
	if (PC && PC->AcknowledgedPawn != CharacterOwner)
	{
		return;
	}

	// Bail out if our character's controller doesn't have a Player. This may be the case when the local player
	// has switched to another controller, such as a debug camera controller.
	if (PC && PC->Player == nullptr)
	{
		return;
	}

	FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
	if (!ClientData)
	{
		return;
	}

	// Update our delta time for physics simulation.
	DeltaTime = ClientData->UpdateTimeStampAndDeltaTime(DeltaTime, *CharacterOwner, *this);

	// Find the oldest (unacknowledged) important move (OldMove).
	// Don't include the last move because it may be combined with the next new move.
	// A saved move is interesting if it differs significantly from the last acknowledged move
	FSavedMovePtr OldMove = NULL;
	if (ClientData->LastAckedMove.IsValid())
	{
		for (int32 i = 0; i < ClientData->SavedMoves.Num() - 1; i++)
		{
			const FSavedMovePtr& CurrentMove = ClientData->SavedMoves[i];
			if (CurrentMove->IsImportantMove(ClientData->LastAckedMove))
			{
				OldMove = CurrentMove;
				break;
			}
		}
	}

	// Get a SavedMove object to store the movement in.
	FSavedMovePtr NewMovePtr = ClientData->CreateSavedMove();
	FSavedMove_Character* const NewMove = NewMovePtr.Get();
	if (NewMove == nullptr)
	{
		return;
	}

	NewMove->SetMoveFor(CharacterOwner, DeltaTime, NewAcceleration, *ClientData);
	// Causing really bad crash when using vr offset location, rather remove for now than have it merge move improperly.

	// see if the two moves could be combined
	// do not combine moves which have different TimeStamps (before and after reset).
	if (const FSavedMove_Character* PendingMove = ClientData->PendingMove.Get())
	{
		if (bAllowMovementMerging && !PendingMove->bOldTimeStampBeforeReset && PendingMove->CanCombineWith(NewMovePtr, CharacterOwner, ClientData->MaxMoveDeltaTime * CharacterOwner->GetActorTimeDilation()))
		{
			//SCOPE_CYCLE_COUNTER(STAT_CharacterMovementCombineNetMove);

			// Only combine and move back to the start location if we don't move back in to a spot that would make us collide with something new.
			/*const */FVector OldStartLocation = PendingMove->GetRevertedLocation();

			// Modifying the location to account for capsule loc
			FVector OverlapLocation = OldStartLocation;
			if (VRRootCapsule)
				OverlapLocation += VRRootCapsule->OffsetComponentToWorld.GetLocation() - VRRootCapsule->GetComponentLocation();

			if (!OverlapTest(/*OldStartLocation*/OverlapLocation, PendingMove->StartRotation.Quaternion(), UpdatedComponent->GetCollisionObjectType(), GetPawnCapsuleCollisionShape(SHRINK_None), CharacterOwner))
			{
				// Avoid updating Mesh bones to physics during the teleport back, since PerformMovement() will update it right away anyway below.
				// Note: this must be before the FScopedMovementUpdate below, since that scope is what actually moves the character and mesh.
				AVRBaseCharacter * BaseCharacter = Cast<AVRBaseCharacter>(CharacterOwner);
				FScopedMeshBoneUpdateOverrideVR ScopedNoMeshBoneUpdate(BaseCharacter != nullptr ? BaseCharacter->GetIKMesh() : CharacterOwner->GetMesh(), EKinematicBonesUpdateToPhysics::SkipAllBones);

				// Accumulate multiple transform updates until scope ends.
				FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, EScopedUpdate::DeferredUpdates);
				UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("CombineMove: add delta %f + %f and revert from %f %f to %f %f"), DeltaTime, ClientData->PendingMove->DeltaTime, UpdatedComponent->GetComponentLocation().X, UpdatedComponent->GetComponentLocation().Y, /*OldStartLocation.X*/OverlapLocation.X, /*OldStartLocation.Y*/OverlapLocation.Y);

				NewMove->CombineWith(PendingMove, CharacterOwner, PC, OldStartLocation);

				/************************/
				if (PC)
				{
					// We reverted position to that at the start of the pending move (above), however some code paths expect rotation to be set correctly
					// before character movement occurs (via FaceRotation), so try that now. The bOrientRotationToMovement path happens later as part of PerformMovement() and PhysicsRotation().
					CharacterOwner->FaceRotation(PC->GetControlRotation(), NewMove->DeltaTime);
				}

				SaveBaseLocation();
				NewMove->SetInitialPosition(CharacterOwner);

				// Remove pending move from move list. It would have to be the last move on the list.
				if (ClientData->SavedMoves.Num() > 0 && ClientData->SavedMoves.Last() == ClientData->PendingMove)
				{
					const bool bAllowShrinking = false;
					ClientData->SavedMoves.Pop(bAllowShrinking);
				}
				ClientData->FreeMove(ClientData->PendingMove);
				ClientData->PendingMove = nullptr;
				PendingMove = nullptr; // Avoid dangling reference, it's deleted above.
			}
			else
			{
				UE_LOG(LogVRCharacterMovement, Log, TEXT("Not combining move [would collide at start location]"));
			}
		}
		/*else
		{
			UE_LOG(LogVRCharacterMovement, Log, TEXT("Not combining move [not allowed by CanCombineWith()]"));
		}*/
	}

	// Acceleration should match what we send to the server, plus any other restrictions the server also enforces (see MoveAutonomous).
	Acceleration = NewMove->Acceleration.GetClampedToMaxSize(GetMaxAcceleration());
	AnalogInputModifier = ComputeAnalogInputModifier(); // recompute since acceleration may have changed.

	// Perform the move locally
	CharacterOwner->ClientRootMotionParams.Clear();
	CharacterOwner->SavedRootMotion.Clear();
	PerformMovement(NewMove->DeltaTime);

	NewMove->PostUpdate(CharacterOwner, FSavedMove_Character::PostUpdate_Record);

	// Add NewMove to the list
	if (CharacterOwner->bReplicateMovement)
	{
		check(NewMove == NewMovePtr.Get());
		ClientData->SavedMoves.Push(NewMovePtr);
		const UWorld* MyWorld = GetWorld();

		static const auto CVarNetEnableMoveCombining = IConsoleManager::Get().FindConsoleVariable(TEXT("p.NetEnableMoveCombining"));
		const bool bCanDelayMove = (CVarNetEnableMoveCombining->GetInt() != 0) && CanDelaySendingMove(NewMovePtr);

		if (bCanDelayMove && ClientData->PendingMove.IsValid() == false)
		{
			// Decide whether to hold off on move	
			// Decide whether to hold off on move
			const float NetMoveDelta = FMath::Clamp(GetClientNetSendDeltaTime(PC, ClientData, NewMovePtr), 1.f / 120.f, 1.f / 5.f);
			
			if ((MyWorld->TimeSeconds - ClientData->ClientUpdateTime) * MyWorld->GetWorldSettings()->GetEffectiveTimeDilation() < NetMoveDelta)
			{
				// Delay sending this move.
				ClientData->PendingMove = NewMovePtr;
				return;
			}
		}

		// Remove 4.16
		//ClientData->ClientUpdateTime = GetWorld()->TimeSeconds;
		
		// Uncomment 4.16
		ClientData->ClientUpdateTime = MyWorld->TimeSeconds;

		UE_LOG(LogVRCharacterMovement, Verbose, TEXT("Client ReplicateMove Time %f Acceleration %s Position %s DeltaTime %f"),
			NewMove->TimeStamp, *NewMove->Acceleration.ToString(), *UpdatedComponent->GetComponentLocation().ToString(), DeltaTime);

		// Send move to server if this character is replicating movement
		bool bSendServerMove = true;
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		static const auto CVarNetForceClientServerMoveLossPercent = IConsoleManager::Get().FindConsoleVariable(TEXT("p.NetForceClientServerMoveLossPercent"));
		bSendServerMove = (CVarNetForceClientServerMoveLossPercent->GetFloat() == 0.f) || (FMath::SRand() >= CVarNetForceClientServerMoveLossPercent->GetFloat());
#endif
		if (bSendServerMove)
		{
			SCOPE_CYCLE_COUNTER(STAT_CharacterMovementCallServerMove);
			CallServerMove(NewMove, OldMove.Get());
		}
	}

	ClientData->PendingMove = NULL;
}

/*
*
*
*  END TEST AREA
*
*
*/

UVRCharacterMovementComponent::UVRCharacterMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PostPhysicsTickFunction.bCanEverTick = true;
	PostPhysicsTickFunction.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	VRRootCapsule = NULL;
	//VRCameraCollider = NULL;
	// 0.1f is low slide and still impacts surfaces well
	// This variable is a bit of a hack, it reduces the movement of the pawn in the direction of relative movement
	WallRepulsionMultiplier = 0.01f;
	bUseClientControlRotation = false;
	bAllowMovementMerging = false;
	bRequestedMoveUseAcceleration = false;
}


void UVRCharacterMovementComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	if (!HasValidData())
	{
		return;
	}

	if (CharacterOwner && CharacterOwner->IsLocallyControlled())
	{
		// Root capsule is now throwing out the difference itself, I use the difference for multiplayer sends
		if (VRRootCapsule)
		{
			AdditionalVRInputVector = VRRootCapsule->DifferenceFromLastFrame;
		}
		else
		{
			AdditionalVRInputVector = FVector::ZeroVector;
		}
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

// No support for crouching code yet
bool UVRCharacterMovementComponent::CanCrouch()
{
	return false;
}


void UVRCharacterMovementComponent::ApplyRepulsionForce(float DeltaSeconds)
{
	if (UpdatedPrimitive && RepulsionForce > 0.0f && CharacterOwner != nullptr)
	{
		const TArray<FOverlapInfo>& Overlaps = UpdatedPrimitive->GetOverlapInfos();
		if (Overlaps.Num() > 0)
		{
			FCollisionQueryParams QueryParams;
			QueryParams.bReturnFaceIndex = false;
			QueryParams.bReturnPhysicalMaterial = false;

			float CapsuleRadius = 0.f;
			float CapsuleHalfHeight = 0.f;
			CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(CapsuleRadius, CapsuleHalfHeight);
			const float RepulsionForceRadius = CapsuleRadius * 1.2f;
			const float StopBodyDistance = 2.5f;
			FVector MyLocation;
			
			if (VRRootCapsule)
				MyLocation = VRRootCapsule->OffsetComponentToWorld.GetLocation();
			else
				MyLocation = UpdatedPrimitive->GetComponentLocation();

			for (int32 i = 0; i < Overlaps.Num(); i++)
			{
				const FOverlapInfo& Overlap = Overlaps[i];

				UPrimitiveComponent* OverlapComp = Overlap.OverlapInfo.Component.Get();
				if (!OverlapComp || OverlapComp->Mobility < EComponentMobility::Movable)
				{
					continue;
				}

				// Use the body instead of the component for cases where we have multi-body overlaps enabled
				FBodyInstance* OverlapBody = nullptr;
				const int32 OverlapBodyIndex = Overlap.GetBodyIndex();
				const USkeletalMeshComponent* SkelMeshForBody = (OverlapBodyIndex != INDEX_NONE) ? Cast<USkeletalMeshComponent>(OverlapComp) : nullptr;
				if (SkelMeshForBody != nullptr)
				{
					OverlapBody = SkelMeshForBody->Bodies.IsValidIndex(OverlapBodyIndex) ? SkelMeshForBody->Bodies[OverlapBodyIndex] : nullptr;
				}
				else
				{
					OverlapBody = OverlapComp->GetBodyInstance();
				}

				if (!OverlapBody)
				{
					UE_LOG(LogVRCharacterMovement, Warning, TEXT("%s could not find overlap body for body index %d"), *GetName(), OverlapBodyIndex);
					continue;
				}

				if (!OverlapBody->IsInstanceSimulatingPhysics())
				{
					continue;
				}

				FTransform BodyTransform = OverlapBody->GetUnrealWorldTransform();

				FVector BodyVelocity = OverlapBody->GetUnrealWorldVelocity();
				FVector BodyLocation = BodyTransform.GetLocation();

				// Trace to get the hit location on the capsule
				FHitResult Hit;
				bool bHasHit = UpdatedPrimitive->LineTraceComponent(Hit, BodyLocation,
					FVector(MyLocation.X, MyLocation.Y, BodyLocation.Z),
					QueryParams);

				FVector HitLoc = Hit.ImpactPoint;
				bool bIsPenetrating = Hit.bStartPenetrating || Hit.PenetrationDepth > StopBodyDistance;

				// If we didn't hit the capsule, we're inside the capsule
				if (!bHasHit)
				{
					HitLoc = BodyLocation;
					bIsPenetrating = true;
				}

				const float DistanceNow = (HitLoc - BodyLocation).SizeSquared2D();
				const float DistanceLater = (HitLoc - (BodyLocation + BodyVelocity * DeltaSeconds)).SizeSquared2D();

				if (bHasHit && DistanceNow < StopBodyDistance && !bIsPenetrating)
				{
					OverlapBody->SetLinearVelocity(FVector(0.0f, 0.0f, 0.0f), false);
				}
				else if (DistanceLater <= DistanceNow || bIsPenetrating)
				{
					FVector ForceCenter = MyLocation;

					if (bHasHit)
					{
						ForceCenter.Z = HitLoc.Z;
					}
					else
					{
						ForceCenter.Z = FMath::Clamp(BodyLocation.Z, MyLocation.Z - CapsuleHalfHeight, MyLocation.Z + CapsuleHalfHeight);
					}

					OverlapBody->AddRadialForceToBody(ForceCenter, RepulsionForceRadius, RepulsionForce * Mass, ERadialImpulseFalloff::RIF_Constant);
				}
			}
		}
	}
}


void UVRCharacterMovementComponent::SetUpdatedComponent(USceneComponent* NewUpdatedComponent)
{
	Super::SetUpdatedComponent(NewUpdatedComponent);

	if (UpdatedComponent)
	{	
		// Fill the VRRootCapsule if we can
		VRRootCapsule = Cast<UVRRootComponent>(UpdatedComponent);

		// Fill in the camera collider if we can
		/*if (AVRCharacter * vrOwner = Cast<AVRCharacter>(this->GetOwner()))
		{
			VRCameraCollider = vrOwner->VRCameraCollider;
		}*/

		// Stop the tick forcing
		UpdatedComponent->PrimaryComponentTick.RemovePrerequisite(this, PrimaryComponentTick);

		// Start forcing the root to tick before this, the actor tick will still tick after the movement component
		// We want the root component to tick first because it is setting its offset location based off of tick
		this->PrimaryComponentTick.AddPrerequisite(UpdatedComponent, UpdatedComponent->PrimaryComponentTick);
	}
}

FORCEINLINE_DEBUGGABLE bool UVRCharacterMovementComponent::SafeMoveUpdatedComponent(const FVector& Delta, const FRotator& NewRotation, bool bSweep, FHitResult& OutHit, ETeleportType Teleport)
{
	return SafeMoveUpdatedComponent(Delta, NewRotation.Quaternion(), bSweep, OutHit, Teleport);
}

bool UVRCharacterMovementComponent::SafeMoveUpdatedComponent(const FVector& Delta, const FQuat& NewRotation, bool bSweep, FHitResult& OutHit, ETeleportType Teleport)
{
	if (UpdatedComponent == NULL)
	{
		OutHit.Reset(1.f);
		return false;
	}

	bool bMoveResult = MoveUpdatedComponent(Delta, NewRotation, bSweep, &OutHit, Teleport);

	// Handle initial penetrations
	if (OutHit.bStartPenetrating && UpdatedComponent)
	{
		const FVector RequestedAdjustment = GetPenetrationAdjustment(OutHit);
		if (ResolvePenetration(RequestedAdjustment, OutHit, NewRotation))
		{
			FHitResult TempHit;
			// Retry original move
			bMoveResult = MoveUpdatedComponent(Delta, NewRotation, bSweep, &TempHit, Teleport);

			// Remove start penetrating if this is a clean move, otherwise use the last moves hit as the result so step up actually attempts to work.
			if (TempHit.bStartPenetrating)
				OutHit = TempHit;
			else
				OutHit.bStartPenetrating = TempHit.bStartPenetrating;
		}
	}

	return bMoveResult;
}

void UVRCharacterMovementComponent::MoveAlongFloor(const FVector& InVelocity, float DeltaSeconds, FStepDownResult* OutStepDownResult)
{
	if (!CurrentFloor.IsWalkableFloor())
	{
		return;
	}

	// Move along the current floor
	const FVector Delta = FVector(InVelocity.X, InVelocity.Y, 0.f) * DeltaSeconds;
	FHitResult Hit(1.f);
	FVector RampVector = ComputeGroundMovementDelta(Delta, CurrentFloor.HitResult, CurrentFloor.bLineTrace);
	SafeMoveUpdatedComponent(RampVector, UpdatedComponent->GetComponentQuat(), true, Hit);
	float LastMoveTimeSlice = DeltaSeconds;

	if (Hit.bStartPenetrating)
	{
		// Allow this hit to be used as an impact we can deflect off, otherwise we do nothing the rest of the update and appear to hitch.
		HandleImpact(Hit);
		SlideAlongSurface(Delta, 1.f, Hit.Normal, Hit, true);

		if (Hit.bStartPenetrating)
		{
			OnCharacterStuckInGeometry(&Hit);
		}
	}
	else if (Hit.IsValidBlockingHit())
	{
		// We impacted something (most likely another ramp, but possibly a barrier).
		float PercentTimeApplied = Hit.Time;
		if ((Hit.Time > 0.f) && (Hit.Normal.Z > KINDA_SMALL_NUMBER) && IsWalkable(Hit))
		{
			// Another walkable ramp.
			const float InitialPercentRemaining = 1.f - PercentTimeApplied;
			RampVector = ComputeGroundMovementDelta(Delta * InitialPercentRemaining, Hit, false);
			LastMoveTimeSlice = InitialPercentRemaining * LastMoveTimeSlice;
			SafeMoveUpdatedComponent(RampVector, UpdatedComponent->GetComponentQuat(), true, Hit);

			const float SecondHitPercent = Hit.Time * InitialPercentRemaining;
			PercentTimeApplied = FMath::Clamp(PercentTimeApplied + SecondHitPercent, 0.f, 1.f);
		}

		if (Hit.IsValidBlockingHit())
		{
			if (CanStepUp(Hit) || (CharacterOwner->GetMovementBase() != NULL && CharacterOwner->GetMovementBase()->GetOwner() == Hit.GetActor()))
			{
				// hit a barrier, try to step up
				const FVector GravDir(0.f, 0.f, -1.f);

				// I add in the HMD difference from last frame to the step up check to enforce it stepping up
				if (!StepUp(GravDir, (Delta * (1.f - PercentTimeApplied)) /*+ AdditionalVRInputVector.GetSafeNormal2D()*/, Hit, OutStepDownResult))
				{
					UE_LOG(LogVRCharacterMovement, Verbose, TEXT("- StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					HandleImpact(Hit, LastMoveTimeSlice, RampVector);
					SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, true);

				}
				else
				{
					// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
					UE_LOG(LogVRCharacterMovement, Verbose, TEXT("+ StepUp (ImpactNormal %s, Normal %s"), *Hit.ImpactNormal.ToString(), *Hit.Normal.ToString());
					bJustTeleported |= !bMaintainHorizontalGroundVelocity;
				}
			}
			else if (Hit.Component.IsValid() && !Hit.Component.Get()->CanCharacterStepUp(CharacterOwner))
			{
				HandleImpact(Hit, LastMoveTimeSlice, RampVector);
				SlideAlongSurface(Delta, 1.f - PercentTimeApplied, Hit.Normal, Hit, true);

			}
		}
	}
}

bool UVRCharacterMovementComponent::StepUp(const FVector& GravDir, const FVector& Delta, const FHitResult &InHit, FStepDownResult* OutStepDownResult)
{
	SCOPE_CYCLE_COUNTER(STAT_CharStepUp);

	if (!CanStepUp(InHit) || MaxStepHeight <= 0.f)
	{
		return false;
	}

	FVector OldLocation;

	if (VRRootCapsule)
		OldLocation = VRRootCapsule->OffsetComponentToWorld.GetLocation();
	else
		OldLocation = UpdatedComponent->GetComponentLocation();

	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	// Don't bother stepping up if top of capsule is hitting something.
	const float InitialImpactZ = InHit.ImpactPoint.Z;
	if (InitialImpactZ > OldLocation.Z + (PawnHalfHeight - PawnRadius))
	{
		return false;
	}

	if (GravDir.IsZero())
	{
		return false;
	}

	// Gravity should be a normalized direction
	ensure(GravDir.IsNormalized());

	float StepTravelUpHeight = MaxStepHeight;
	float StepTravelDownHeight = StepTravelUpHeight;
	const float StepSideZ = -1.f * FVector::DotProduct(InHit.ImpactNormal, GravDir);//const float StepSideZ = -1.f * (InHit.ImpactNormal | GravDir);
	float PawnInitialFloorBaseZ = OldLocation.Z -PawnHalfHeight;
	float PawnFloorPointZ = PawnInitialFloorBaseZ;

	if (IsMovingOnGround() && CurrentFloor.IsWalkableFloor())
	{
		// Since we float a variable amount off the floor, we need to enforce max step height off the actual point of impact with the floor.
		const float FloorDist = FMath::Max(0.f, CurrentFloor.GetDistanceToFloor());
		PawnInitialFloorBaseZ -= FloorDist;
		StepTravelUpHeight = FMath::Max(StepTravelUpHeight - FloorDist, 0.f);
		StepTravelDownHeight = (MaxStepHeight + MAX_FLOOR_DIST*2.f);

		const bool bHitVerticalFace = !IsWithinEdgeTolerance(InHit.Location, InHit.ImpactPoint, PawnRadius);
		if (!CurrentFloor.bLineTrace && !bHitVerticalFace)
		{
			PawnFloorPointZ = CurrentFloor.HitResult.ImpactPoint.Z;
		}
		else
		{
			// Base floor point is the base of the capsule moved down by how far we are hovering over the surface we are hitting.
			PawnFloorPointZ -= CurrentFloor.FloorDist;
		}
	}

	// Don't step up if the impact is below us, accounting for distance from floor.
	if (InitialImpactZ <= PawnInitialFloorBaseZ)
	{
		return false;
	}

	// Scope our movement updates, and do not apply them until all intermediate moves are completed.
	/*FScopedMovementUpdate*/ FVRCharacterScopedMovementUpdate ScopedStepUpMovement(UpdatedComponent, EScopedUpdate::DeferredUpdates);

	// step up - treat as vertical wall
	FHitResult SweepUpHit(1.f);
	const FQuat PawnRotation = UpdatedComponent->GetComponentQuat();
	MoveUpdatedComponent(-GravDir * StepTravelUpHeight, PawnRotation, true, &SweepUpHit);

	if (SweepUpHit.bStartPenetrating)
	{
		// Undo movement
		ScopedStepUpMovement.RevertMove();
		return false;
	}


	// step fwd
	FHitResult Hit(1.f);
	MoveUpdatedComponent(Delta, PawnRotation, true, &Hit);

	// Check result of forward movement
	if (Hit.bBlockingHit)
	{
		if (Hit.bStartPenetrating)
		{
			// Undo movement
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// If we hit something above us and also something ahead of us, we should notify about the upward hit as well.
		// The forward hit will be handled later (in the bSteppedOver case below).
		// In the case of hitting something above but not forward, we are not blocked from moving so we don't need the notification.
		if (SweepUpHit.bBlockingHit && Hit.bBlockingHit)
		{
			HandleImpact(SweepUpHit);
		}

		// pawn ran into a wall
		HandleImpact(Hit);
		if (IsFalling())
		{
			return true;
		}

		// adjust and try again
		const float ForwardHitTime = Hit.Time;
		const float ForwardSlideAmount = SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);

		if (IsFalling())
		{
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// If both the forward hit and the deflection got us nowhere, there is no point in this step up.
		if (ForwardHitTime == 0.f && ForwardSlideAmount == 0.f)
		{
			ScopedStepUpMovement.RevertMove();
			return false;
		}
	}

	// Step down
	MoveUpdatedComponent(GravDir * StepTravelDownHeight, UpdatedComponent->GetComponentQuat(), true, &Hit);

	// If step down was initially penetrating abort the step up
	if (Hit.bStartPenetrating)
	{
		ScopedStepUpMovement.RevertMove();
		return false;
	}

	FStepDownResult StepDownResult;
	if (Hit.IsValidBlockingHit())
	{
		// See if this step sequence would have allowed us to travel higher than our max step height allows.
		const float DeltaZ = Hit.ImpactPoint.Z - PawnFloorPointZ;
		if (DeltaZ > MaxStepHeight)
		{
			//UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (too high Height %.3f) up from floor base %f"), DeltaZ, PawnInitialFloorBaseZ);
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// Reject unwalkable surface normals here.
		if (!IsWalkable(Hit))
		{
			// Reject if normal opposes movement direction
			const bool bNormalTowardsMe = (Delta | Hit.ImpactNormal) < 0.f;
			if (bNormalTowardsMe)
			{
				//UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (unwalkable normal %s opposed to movement)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
				return false;
			}

			// Also reject if we would end up being higher than our starting location by stepping down.
			// It's fine to step down onto an unwalkable normal below us, we will just slide off. Rejecting those moves would prevent us from being able to walk off the edge.
			if (Hit.Location.Z > OldLocation.Z)
			{
				//UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (unwalkable normal %s above old position)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
				return false;
			}
		}

		// Reject moves where the downward sweep hit something very close to the edge of the capsule. This maintains consistency with FindFloor as well.
		if (!IsWithinEdgeTolerance(Hit.Location, Hit.ImpactPoint, PawnRadius))
		{
			//UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (outside edge tolerance)"));
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// Don't step up onto invalid surfaces if traveling higher.
		if (DeltaZ > 0.f && !CanStepUp(Hit))
		{
			//UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (up onto surface with !CanStepUp())"));
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// See if we can validate the floor as a result of this step down. In almost all cases this should succeed, and we can avoid computing the floor outside this method.
		if (OutStepDownResult != NULL)
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), StepDownResult.FloorResult, false, &Hit);

			// Reject unwalkable normals if we end up higher than our initial height.
			// It's fine to walk down onto an unwalkable surface, don't reject those moves.
			if (Hit.Location.Z > OldLocation.Z)
			{
				// We should reject the floor result if we are trying to step up an actual step where we are not able to perch (this is rare).
				// In those cases we should instead abort the step up and try to slide along the stair.
				if (!StepDownResult.FloorResult.bBlockingHit && StepSideZ < MAX_STEP_SIDE_Z)
				{
					ScopedStepUpMovement.RevertMove();
					return false;
				}
			}

			StepDownResult.bComputedFloor = true;
		}
	}

	// Copy step down result.
	if (OutStepDownResult != NULL)
	{
		*OutStepDownResult = StepDownResult;
	}

	// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
	bJustTeleported |= !bMaintainHorizontalGroundVelocity;

	return true;
}

bool UVRCharacterMovementComponent::IsWithinEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const
{
	const float DistFromCenterSq = (TestImpactPoint - CapsuleLocation).SizeSquared2D();
	const float ReducedRadiusSq = FMath::Square(FMath::Max(VREdgeRejectDistance + KINDA_SMALL_NUMBER, CapsuleRadius - VREdgeRejectDistance));
	return DistFromCenterSq < ReducedRadiusSq;
}

bool UVRCharacterMovementComponent::IsWithinClimbingEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const
{
	const float DistFromCenterSq = (TestImpactPoint - CapsuleLocation).SizeSquared2D();
	const float ReducedRadiusSq = FMath::Square(FMath::Max(VRClimbingEdgeRejectDistance + KINDA_SMALL_NUMBER, CapsuleRadius - VRClimbingEdgeRejectDistance));
	return DistFromCenterSq < ReducedRadiusSq;
}

bool UVRCharacterMovementComponent::VRClimbStepUp(const FVector& GravDir, const FVector& Delta, const FHitResult &InHit, FStepDownResult* OutStepDownResult)
{
	SCOPE_CYCLE_COUNTER(STAT_CharStepUp);

	if (!CanStepUp(InHit) || MaxStepHeight <= 0.f)
	{
		return false;
	}

	FVector OldLocation;

	if (VRRootCapsule)
		OldLocation = VRRootCapsule->OffsetComponentToWorld.GetLocation();
	else
		OldLocation = UpdatedComponent->GetComponentLocation();

	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	// Don't bother stepping up if top of capsule is hitting something.
	const float InitialImpactZ = InHit.ImpactPoint.Z;
	if (InitialImpactZ > OldLocation.Z + (PawnHalfHeight - PawnRadius))
	{
		return false;
	}

	// Don't step up if the impact is below us
	if (InitialImpactZ <= OldLocation.Z - PawnHalfHeight)
	{
		return false;
	}

	if (GravDir.IsZero())
	{
		return false;
	}

	// Gravity should be a normalized direction
	ensure(GravDir.IsNormalized());

	float StepTravelUpHeight = MaxStepHeight;
	float StepTravelDownHeight = StepTravelUpHeight;
	const float StepSideZ = -1.f * (InHit.ImpactNormal | GravDir);
	float PawnInitialFloorBaseZ = OldLocation.Z - PawnHalfHeight;
	float PawnFloorPointZ = PawnInitialFloorBaseZ;

	// Scope our movement updates, and do not apply them until all intermediate moves are completed.
	FVRCharacterScopedMovementUpdate ScopedStepUpMovement(UpdatedComponent, EScopedUpdate::DeferredUpdates);

	// step up - treat as vertical wall
	FHitResult SweepUpHit(1.f);
	const FQuat PawnRotation = UpdatedComponent->GetComponentQuat();
	MoveUpdatedComponent(-GravDir * StepTravelUpHeight, PawnRotation, true, &SweepUpHit);

	if (SweepUpHit.bStartPenetrating)
	{
		// Undo movement
		ScopedStepUpMovement.RevertMove();
		return false;
	}

	// step fwd
	FHitResult Hit(1.f);


	// Adding in the directional difference of the last HMD movement to promote stepping up
	// Probably entirely wrong as Delta is divided by movement ticks but I want the effect to be stronger anyway
	// This won't effect control based movement unless stepping forward at the same time, but gives RW movement
	// the extra boost to get up over a lip
	// #TODO test this more, currently appears to be needed for walking, but is harmful for other modes
//	if (VRRootCapsule)
//		MoveUpdatedComponent(Delta + VRRootCapsule->DifferenceFromLastFrame.GetSafeNormal2D(), PawnRotation, true, &Hit);
//	else
		MoveUpdatedComponent(Delta, PawnRotation, true, &Hit);

	//MoveUpdatedComponent(Delta, PawnRotation, true, &Hit);

	// Check result of forward movement
	if (Hit.bBlockingHit)
	{
		if (Hit.bStartPenetrating)
		{
			// Undo movement
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// If we hit something above us and also something ahead of us, we should notify about the upward hit as well.
		// The forward hit will be handled later (in the bSteppedOver case below).
		// In the case of hitting something above but not forward, we are not blocked from moving so we don't need the notification.
		if (SweepUpHit.bBlockingHit && Hit.bBlockingHit)
		{

			HandleImpact(SweepUpHit);
		}

		// pawn ran into a wall
		HandleImpact(Hit);
		if (IsFalling())
		{
			return true;
		}

		//Don't adjust for VR, it doesn't work correctly
		ScopedStepUpMovement.RevertMove();
		return false;

		// adjust and try again
		//const float ForwardHitTime = Hit.Time;
		//const float ForwardSlideAmount = SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);

		//if (IsFalling())
		//{
		//	ScopedStepUpMovement.RevertMove();
		//	return false;
		//}

		// If both the forward hit and the deflection got us nowhere, there is no point in this step up.
		//if (ForwardHitTime == 0.f && ForwardSlideAmount == 0.f)
		//{
		//	ScopedStepUpMovement.RevertMove();
		//	return false;
		//}
	}

	// Step down
	MoveUpdatedComponent(GravDir * StepTravelDownHeight, UpdatedComponent->GetComponentQuat(), true, &Hit);

	// If step down was initially penetrating abort the step up
	if (Hit.bStartPenetrating)
	{
		ScopedStepUpMovement.RevertMove();
		return false;
	}

	FStepDownResult StepDownResult;
	if (Hit.IsValidBlockingHit())
	{
		// See if this step sequence would have allowed us to travel higher than our max step height allows.
		const float DeltaZ = Hit.ImpactPoint.Z - PawnFloorPointZ;
		if (DeltaZ > MaxStepHeight)
		{
			UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (too high Height %.3f) up from floor base %f"), DeltaZ, PawnInitialFloorBaseZ);
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// Reject unwalkable surface normals here.
		if (!IsWalkable(Hit))
		{
			// Reject if normal opposes movement direction
			const bool bNormalTowardsMe = (Delta | Hit.ImpactNormal) < 0.f;
			if (bNormalTowardsMe)
			{
				//UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (unwalkable normal %s opposed to movement)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
				return false;
			}

			// Also reject if we would end up being higher than our starting location by stepping down.
			// It's fine to step down onto an unwalkable normal below us, we will just slide off. Rejecting those moves would prevent us from being able to walk off the edge.
			if (Hit.Location.Z > OldLocation.Z)
			{
				UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (unwalkable normal %s above old position)"), *Hit.ImpactNormal.ToString());
				ScopedStepUpMovement.RevertMove();
				return false;
			}
		}

		// Reject moves where the downward sweep hit something very close to the edge of the capsule. This maintains consistency with FindFloor as well.
		if (!IsWithinClimbingEdgeTolerance(Hit.Location, Hit.ImpactPoint, PawnRadius))
		{
			UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (outside edge tolerance)"));
			ScopedStepUpMovement.RevertMove();
			return false;
		}

		// Don't step up onto invalid surfaces if traveling higher.
		if (DeltaZ > 0.f && !CanStepUp(Hit))
		{
			UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("- Reject StepUp (up onto surface with !CanStepUp())"));
			ScopedStepUpMovement.RevertMove();
			return false;		
		}

		// See if we can validate the floor as a result of this step down. In almost all cases this should succeed, and we can avoid computing the floor outside this method.
		if (OutStepDownResult != NULL)
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), StepDownResult.FloorResult, false, &Hit);

			// Reject unwalkable normals if we end up higher than our initial height.
			// It's fine to walk down onto an unwalkable surface, don't reject those moves.
			if (Hit.Location.Z > OldLocation.Z)
			{
				// We should reject the floor result if we are trying to step up an actual step where we are not able to perch (this is rare).
				// In those cases we should instead abort the step up and try to slide along the stair.
				if (!StepDownResult.FloorResult.bBlockingHit && StepSideZ < MAX_STEP_SIDE_Z)
				{
					ScopedStepUpMovement.RevertMove();
					return false;
				}
			}

			StepDownResult.bComputedFloor = true;
		}
	}

	// Copy step down result.
	if (OutStepDownResult != NULL)
	{
		*OutStepDownResult = StepDownResult;
	}

	// Don't recalculate velocity based on this height adjustment, if considering vertical adjustments.
	bJustTeleported |= !bMaintainHorizontalGroundVelocity;
	return true;
}


void UVRCharacterMovementComponent::UpdateBasedMovement(float DeltaSeconds)
{
	if (!HasValidData())
	{
		return;
	}

	const UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
	if (!MovementBaseUtility::UseRelativeLocation(MovementBase))
	{
		return;
	}

	if (!IsValid(MovementBase) || !IsValid(MovementBase->GetOwner()))
	{
		SetBase(NULL);
		return;
	}

	// Ignore collision with bases during these movements.
	TGuardValue<EMoveComponentFlags> ScopedFlagRestore(MoveComponentFlags, MoveComponentFlags | MOVECOMP_IgnoreBases);

	FQuat DeltaQuat = FQuat::Identity;
	FVector DeltaPosition = FVector::ZeroVector;

	FQuat NewBaseQuat;
	FVector NewBaseLocation;
	if (!MovementBaseUtility::GetMovementBaseTransform(MovementBase, CharacterOwner->GetBasedMovement().BoneName, NewBaseLocation, NewBaseQuat))
	{
		return;
	}

	// Find change in rotation
	const bool bRotationChanged = !OldBaseQuat.Equals(NewBaseQuat, 1e-8f);
	if (bRotationChanged)
	{
		DeltaQuat = NewBaseQuat * OldBaseQuat.Inverse();
	}

	// only if base moved
	if (bRotationChanged || (OldBaseLocation != NewBaseLocation))
	{
		// Calculate new transform matrix of base actor (ignoring scale).
		const FQuatRotationTranslationMatrix OldLocalToWorld(OldBaseQuat, OldBaseLocation);
		const FQuatRotationTranslationMatrix NewLocalToWorld(NewBaseQuat, NewBaseLocation);

		if (CharacterOwner->IsMatineeControlled())
		{
			FRotationTranslationMatrix HardRelMatrix(CharacterOwner->GetBasedMovement().Rotation, CharacterOwner->GetBasedMovement().Location);
			const FMatrix NewWorldTM = HardRelMatrix * NewLocalToWorld;
			const FQuat NewWorldRot = bIgnoreBaseRotation ? UpdatedComponent->GetComponentQuat() : NewWorldTM.ToQuat();
			MoveUpdatedComponent(NewWorldTM.GetOrigin() - UpdatedComponent->GetComponentLocation(), NewWorldRot, true);
		}
		else
		{
			FQuat FinalQuat = UpdatedComponent->GetComponentQuat();

			if (bRotationChanged && !bIgnoreBaseRotation)
			{
				// Apply change in rotation and pipe through FaceRotation to maintain axis restrictions
				const FQuat PawnOldQuat = UpdatedComponent->GetComponentQuat();
				const FQuat TargetQuat = DeltaQuat * FinalQuat;
				FRotator TargetRotator(TargetQuat);
				CharacterOwner->FaceRotation(TargetRotator, 0.f);
				FinalQuat = UpdatedComponent->GetComponentQuat();

				if (PawnOldQuat.Equals(FinalQuat, 1e-6f))
				{
					// Nothing changed. This means we probably are using another rotation mechanism (bOrientToMovement etc). We should still follow the base object.
					// @todo: This assumes only Yaw is used, currently a valid assumption. This is the only reason FaceRotation() is used above really, aside from being a virtual hook.
					if (bOrientRotationToMovement || (bUseControllerDesiredRotation && CharacterOwner->Controller))
					{
						TargetRotator.Pitch = 0.f;
						TargetRotator.Roll = 0.f;
						MoveUpdatedComponent(FVector::ZeroVector, TargetRotator, false);
						FinalQuat = UpdatedComponent->GetComponentQuat();
					}
				}

				// Pipe through ControlRotation, to affect camera.
				if (CharacterOwner->Controller)
				{
					const FQuat PawnDeltaRotation = FinalQuat * PawnOldQuat.Inverse();
					FRotator FinalRotation = FinalQuat.Rotator();
					UpdateBasedRotation(FinalRotation, PawnDeltaRotation.Rotator());
					FinalQuat = UpdatedComponent->GetComponentQuat();
				}
			}

			// We need to offset the base of the character here, not its origin, so offset by half height
			float HalfHeight, Radius;
			CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(Radius, HalfHeight);

			FVector const BaseOffset(0.0f, 0.0f, 0.0f);//(0.0f, 0.0f, HalfHeight);
			FVector const LocalBasePos = OldLocalToWorld.InverseTransformPosition(UpdatedComponent->GetComponentLocation() - BaseOffset);
			FVector const NewWorldPos = ConstrainLocationToPlane(NewLocalToWorld.TransformPosition(LocalBasePos) + BaseOffset);
			DeltaPosition = ConstrainDirectionToPlane(NewWorldPos - UpdatedComponent->GetComponentLocation());

			// move attached actor
			if (bFastAttachedMove)
			{
				// we're trusting no other obstacle can prevent the move here
				UpdatedComponent->SetWorldLocationAndRotation(NewWorldPos, FinalQuat, false);
			}
			else
			{
				// hack - transforms between local and world space introducing slight error FIXMESTEVE - discuss with engine team: just skip the transforms if no rotation?
				FVector BaseMoveDelta = NewBaseLocation - OldBaseLocation;
				if (!bRotationChanged && (BaseMoveDelta.X == 0.f) && (BaseMoveDelta.Y == 0.f))
				{
					DeltaPosition.X = 0.f;
					DeltaPosition.Y = 0.f;
				}

				FHitResult MoveOnBaseHit(1.f);
				const FVector OldLocation = UpdatedComponent->GetComponentLocation();
				MoveUpdatedComponent(DeltaPosition, FinalQuat, true, &MoveOnBaseHit);
				if ((UpdatedComponent->GetComponentLocation() - (OldLocation + DeltaPosition)).IsNearlyZero() == false)
				{
					OnUnableToFollowBaseMove(DeltaPosition, OldLocation, MoveOnBaseHit);
				}
			}
		}

		if (MovementBase->IsSimulatingPhysics() && CharacterOwner->GetMesh())
		{
			CharacterOwner->GetMesh()->ApplyDeltaToAllPhysicsTransforms(DeltaPosition, DeltaQuat);
		}
	}
}

FVector UVRCharacterMovementComponent::GetImpartedMovementBaseVelocity() const
{
	FVector Result = FVector::ZeroVector;

	if (CharacterOwner)
	{
		UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
		if (MovementBaseUtility::IsDynamicBase(MovementBase))
		{
			FVector BaseVelocity = MovementBaseUtility::GetMovementBaseVelocity(MovementBase, CharacterOwner->GetBasedMovement().BoneName);

			if (bImpartBaseAngularVelocity)
			{
				// Base position should be the bottom of the actor since I offset the capsule now
				const FVector CharacterBasePosition = (UpdatedComponent->GetComponentLocation()/* - FVector(0.f, 0.f, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight())*/);
				const FVector BaseTangentialVel = MovementBaseUtility::GetMovementBaseTangentialVelocity(MovementBase, CharacterOwner->GetBasedMovement().BoneName, CharacterBasePosition);
				BaseVelocity += BaseTangentialVel;
			}

			if (bImpartBaseVelocityX)
			{
				Result.X = BaseVelocity.X;
			}
			if (bImpartBaseVelocityY)
			{
				Result.Y = BaseVelocity.Y;
			}
			if (bImpartBaseVelocityZ)
			{
				Result.Z = BaseVelocity.Z;
			}
		}
	}

	return Result;
}



void UVRCharacterMovementComponent::FindFloor(const FVector& CapsuleLocation, FFindFloorResult& OutFloorResult, bool bZeroDelta, const FHitResult* DownwardSweepResult) const
{
	SCOPE_CYCLE_COUNTER(STAT_CharFindFloor);
	//UE_LOG(LogVRCharacterMovement, Warning, TEXT("Find Floor"));
	// No collision, no floor...
	if (!HasValidData() || !UpdatedComponent->IsQueryCollisionEnabled())
	{
		OutFloorResult.Clear();
		return;
	}

	//UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("[Role:%d] FindFloor: %s at location %s"), (int32)CharacterOwner->Role, *GetNameSafe(CharacterOwner), *CapsuleLocation.ToString());
	check(CharacterOwner->GetCapsuleComponent());

	FVector UseCapsuleLocation = CapsuleLocation;
	if (VRRootCapsule)
		UseCapsuleLocation = VRRootCapsule->OffsetComponentToWorld.GetLocation();

	// Increase height check slightly if walking, to prevent floor height adjustment from later invalidating the floor result.
	const float HeightCheckAdjust = ((IsMovingOnGround() || IsClimbing()) ? MAX_FLOOR_DIST + KINDA_SMALL_NUMBER : -MAX_FLOOR_DIST);

	float FloorSweepTraceDist = FMath::Max(MAX_FLOOR_DIST, MaxStepHeight + HeightCheckAdjust);
	float FloorLineTraceDist = FloorSweepTraceDist;
	bool bNeedToValidateFloor = true;

	// For reverting
	FFindFloorResult LastFloor = CurrentFloor;

	// Sweep floor
	if (FloorLineTraceDist > 0.f || FloorSweepTraceDist > 0.f)
	{
		UCharacterMovementComponent* MutableThis = const_cast<UCharacterMovementComponent*>((UCharacterMovementComponent*)this);

		if (bAlwaysCheckFloor || !bZeroDelta || bForceNextFloorCheck || bJustTeleported)
		{
			MutableThis->bForceNextFloorCheck = false;
			ComputeFloorDist(UseCapsuleLocation, FloorLineTraceDist, FloorSweepTraceDist, OutFloorResult, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius(), DownwardSweepResult);
		}
		else
		{
			// Force floor check if base has collision disabled or if it does not block us.
			UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
			const AActor* BaseActor = MovementBase ? MovementBase->GetOwner() : NULL;
			
			const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();

			if (MovementBase != NULL)
			{
				MutableThis->bForceNextFloorCheck = !MovementBase->IsQueryCollisionEnabled()
					|| MovementBase->GetCollisionResponseToChannel(CollisionChannel) != ECR_Block
					|| MovementBaseUtility::IsDynamicBase(MovementBase);
			}

			const bool IsActorBasePendingKill = BaseActor && BaseActor->IsPendingKill();

			if (!bForceNextFloorCheck && !IsActorBasePendingKill && MovementBase)
			{
				//UE_LOG(LogVRCharacterMovement, Log, TEXT("%s SKIP check for floor"), *CharacterOwner->GetName());
				OutFloorResult = CurrentFloor;
				bNeedToValidateFloor = false;
			}
			else
			{
				MutableThis->bForceNextFloorCheck = false;
				ComputeFloorDist(UseCapsuleLocation, FloorLineTraceDist, FloorSweepTraceDist, OutFloorResult, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius(), DownwardSweepResult);
			}
		}
	}

	// #TODO: Modify the floor compute floor distance instead? Or just run movement logic differently for the bWalkingCollisionOverride setup.
	// #VR Specific - ignore floor traces that are negative, this can be caused by a failed floor check while starting in penetration
	if (VRRootCapsule && VRRootCapsule->bUseWalkingCollisionOverride && OutFloorResult.bBlockingHit && OutFloorResult.FloorDist <= 0.0f)
	{ 

		if (OutFloorResult.FloorDist <= -FMath::Max(MAX_FLOOR_DIST, CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius()))
		{
			// This was a negative trace result, the game wants us to pull out of penetration
			// But with walking collision override we don't want to do that, so check for the correct channel and throw away
			// the new floor if it matches
			ECollisionResponse FloorResponse;
			if (OutFloorResult.HitResult.Component.IsValid())
			{
				FloorResponse = OutFloorResult.HitResult.Component->GetCollisionResponseToChannel(VRRootCapsule->WalkingCollisionOverride);
				if (FloorResponse == ECR_Ignore || FloorResponse == ECR_Overlap)
					OutFloorResult = LastFloor;	// Move back to the last floor value, we are in penetration with a walking override
			}
		}
	}

	// OutFloorResult.HitResult is now the result of the vertical floor check.
	// See if we should try to "perch" at this location.
	if (bNeedToValidateFloor && OutFloorResult.bBlockingHit && !OutFloorResult.bLineTrace)
	{
		const bool bCheckRadius = true;
		if (ShouldComputePerchResult(OutFloorResult.HitResult, bCheckRadius))
		{
			float MaxPerchFloorDist = FMath::Max(MAX_FLOOR_DIST, MaxStepHeight + HeightCheckAdjust);
			if (IsMovingOnGround() || IsClimbing())
			{
				MaxPerchFloorDist += FMath::Max(0.f, PerchAdditionalHeight);
			}
			
			FFindFloorResult PerchFloorResult;
			if (ComputePerchResult(GetValidPerchRadius(), OutFloorResult.HitResult, MaxPerchFloorDist, PerchFloorResult))
			{
				// Don't allow the floor distance adjustment to push us up too high, or we will move beyond the perch distance and fall next time.
				const float AvgFloorDist = (MIN_FLOOR_DIST + MAX_FLOOR_DIST) * 0.5f;
				const float MoveUpDist = (AvgFloorDist - OutFloorResult.FloorDist);
				if (MoveUpDist + PerchFloorResult.FloorDist >= MaxPerchFloorDist)
				{
					OutFloorResult.FloorDist = AvgFloorDist;
				}

				// If the regular capsule is on an unwalkable surface but the perched one would allow us to stand, override the normal to be one that is walkable.
				if (!OutFloorResult.bWalkableFloor)
				{
					OutFloorResult.SetFromLineTrace(PerchFloorResult.HitResult, OutFloorResult.FloorDist, FMath::Min(PerchFloorResult.FloorDist, PerchFloorResult.LineDist), true);
				}
			}
			else
			{
				// We had no floor (or an invalid one because it was unwalkable), and couldn't perch here, so invalidate floor (which will cause us to start falling).
				OutFloorResult.bWalkableFloor = false;
			}
		}
	}
}

// MOVED TO BASE VR CHARCTER MOVEMENT COMPONENT
// Also added a control variable for it there
/*
bool UVRCharacterMovementComponent::FloorSweepTest(
	FHitResult& OutHit,
	const FVector& Start,
	const FVector& End,
	ECollisionChannel TraceChannel,
	const struct FCollisionShape& CollisionShape,
	const struct FCollisionQueryParams& Params,
	const struct FCollisionResponseParams& ResponseParam
	) const
{
	bool bBlockingHit = false;

	if (!bUseFlatBaseForFloorChecks)
	{
		TArray<FHitResult> OutHits;
		GetWorld()->SweepMultiByChannel(OutHits, Start, End, FQuat::Identity, TraceChannel, CollisionShape, Params, ResponseParam);

		for (int i = 0; i < OutHits.Num(); i++)
		{
			if (OutHits[i].bBlockingHit && (OutHits[i].Component.IsValid() && !OutHits[i].Component->IsSimulatingPhysics()))
			{
				OutHit = OutHits[i];
				bBlockingHit = true;
				break;
			}
		}

		//bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, TraceChannel, CollisionShape, Params, ResponseParam);
	}
	else
	{
		// Test with a box that is enclosed by the capsule.
		const float CapsuleRadius = CollisionShape.GetCapsuleRadius();
		const float CapsuleHeight = CollisionShape.GetCapsuleHalfHeight();
		const FCollisionShape BoxShape = FCollisionShape::MakeBox(FVector(CapsuleRadius * 0.707f, CapsuleRadius * 0.707f, CapsuleHeight));

		// First test with the box rotated so the corners are along the major axes (ie rotated 45 degrees).
		TArray<FHitResult> OutHits;
		GetWorld()->SweepMultiByChannel(OutHits, Start, End, FQuat(FVector(0.f, 0.f, -1.f), PI * 0.25f), TraceChannel, BoxShape, Params, ResponseParam);

		for (int i = 0; i < OutHits.Num(); i++)
		{
			if (OutHits[i].bBlockingHit && (OutHits[i].Component.IsValid() && !OutHits[i].Component->IsSimulatingPhysics()))
			{
				OutHit = OutHits[i];
				bBlockingHit = true;
				break;
			}
		}
		
		//bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, Start, End, FQuat(FVector(0.f, 0.f, -1.f), PI * 0.25f), TraceChannel, BoxShape, Params, ResponseParam);

		if (!bBlockingHit)
		{
			// Test again with the same box, not rotated.
			OutHit.Reset(1.f, false);

			OutHits.Reset();
			//TArray<FHitResult> OutHits;
			GetWorld()->SweepMultiByChannel(OutHits, Start, End, FQuat::Identity, TraceChannel, BoxShape, Params, ResponseParam);

			for (int i = 0; i < OutHits.Num(); i++)
			{
				if (OutHits[i].bBlockingHit && (OutHits[i].Component.IsValid() && !OutHits[i].Component->IsSimulatingPhysics()))
				{
					OutHit = OutHits[i];
					bBlockingHit = true;
					break;
				}
			}
			//bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, TraceChannel, BoxShape, Params, ResponseParam);
		}
	}

	return bBlockingHit;
}*/


float UVRCharacterMovementComponent::ImmersionDepth() const
{
	float depth = 0.f;

	if (CharacterOwner && GetPhysicsVolume()->bWaterVolume)
	{
		const float CollisionHalfHeight = CharacterOwner->GetSimpleCollisionHalfHeight();

		if ((CollisionHalfHeight == 0.f) || (Buoyancy == 0.f))
		{
			depth = 1.f;
		}
		else
		{
			UBrushComponent* VolumeBrushComp = GetPhysicsVolume()->GetBrushComponent();
			FHitResult Hit(1.f);
			if (VolumeBrushComp)
			{
				FVector TraceStart;
				FVector TraceEnd;

				if (VRRootCapsule)
				{
					TraceStart = VRRootCapsule->OffsetComponentToWorld.GetLocation() + FVector(0.f, 0.f, CollisionHalfHeight);
					TraceEnd = VRRootCapsule->OffsetComponentToWorld.GetLocation() - FVector(0.f, 0.f, CollisionHalfHeight);
				}
				else
				{
					TraceStart = UpdatedComponent->GetComponentLocation() + FVector(0.f, 0.f, CollisionHalfHeight);
					TraceEnd = UpdatedComponent->GetComponentLocation() -FVector(0.f, 0.f, CollisionHalfHeight);
				}

				FCollisionQueryParams NewTraceParams(CharacterMovementComponentStatics::ImmersionDepthName, true);
				VolumeBrushComp->LineTraceComponent(Hit, TraceStart, TraceEnd, NewTraceParams);
			}

			depth = (Hit.Time == 1.f) ? 1.f : (1.f - Hit.Time);
		}
	}
	return depth;
}

/*void UVRCharacterMovementComponent::VisualizeMovement() const
{
	if (CharacterOwner == nullptr)
	{
		return;
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	FVector TopOfCapsule;
	if (VRRootCapsule)
		TopOfCapsule = VRRootCapsule->GetVRLocation();
	else
		TopOfCapsule = GetActorLocation();

	TopOfCapsule += FVector(0.f, 0.f, CharacterOwner->GetSimpleCollisionHalfHeight());


	float HeightOffset = 0.f;

	// Position
	{
		const FColor DebugColor = FColor::White;
		const FVector DebugLocation = TopOfCapsule + FVector(0.f, 0.f, HeightOffset);
		FString DebugText = FString::Printf(TEXT("Position: %s"), *GetActorLocation().ToCompactString());
		DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
	}

	// Velocity
	{
		const FColor DebugColor = FColor::Green;
		HeightOffset += 15.f;
		const FVector DebugLocation = TopOfCapsule + FVector(0.f, 0.f, HeightOffset);
		DrawDebugDirectionalArrow(GetWorld(), DebugLocation, DebugLocation + Velocity,
			100.f, DebugColor, false, -1.f, (uint8)'\000', 10.f);

		FString DebugText = FString::Printf(TEXT("Velocity: %s (Speed: %.2f)"), *Velocity.ToCompactString(), Velocity.Size());
		DrawDebugString(GetWorld(), DebugLocation + FVector(0.f, 0.f, 5.f), DebugText, nullptr, DebugColor, 0.f, true);
	}

	// Acceleration
	{
		const FColor DebugColor = FColor::Yellow;
		HeightOffset += 15.f;
		const float MaxAccelerationLineLength = 200.f;
		const float CurrentMaxAccel = GetMaxAcceleration();
		const float CurrentAccelAsPercentOfMaxAccel = CurrentMaxAccel > 0.f ? Acceleration.Size() / CurrentMaxAccel : 1.f;
		const FVector DebugLocation = TopOfCapsule + FVector(0.f, 0.f, HeightOffset);
		DrawDebugDirectionalArrow(GetWorld(), DebugLocation,
			DebugLocation + Acceleration.GetSafeNormal(SMALL_NUMBER) * CurrentAccelAsPercentOfMaxAccel * MaxAccelerationLineLength,
			25.f, DebugColor, false, -1.f, (uint8)'\000', 8.f);

		FString DebugText = FString::Printf(TEXT("Acceleration: %s"), *Acceleration.ToCompactString());
		DrawDebugString(GetWorld(), DebugLocation + FVector(0.f, 0.f, 5.f), DebugText, nullptr, DebugColor, 0.f, true);
	}

	// Movement Mode
	{
		const FColor DebugColor = FColor::Blue;
		HeightOffset += 20.f;
		const FVector DebugLocation = TopOfCapsule + FVector(0.f, 0.f, HeightOffset);
		FString DebugText = FString::Printf(TEXT("MovementMode: %s"), *GetMovementName());
		DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
	}

	// Root motion (additive)
	if (CurrentRootMotion.HasAdditiveVelocity())
	{
		const FColor DebugColor = FColor::Cyan;
		HeightOffset += 15.f;
		const FVector DebugLocation = TopOfCapsule + FVector(0.f, 0.f, HeightOffset);

		FVector CurrentAdditiveVelocity(FVector::ZeroVector);
		CurrentRootMotion.AccumulateAdditiveRootMotionVelocity(0.f, *CharacterOwner, *this, CurrentAdditiveVelocity);

		DrawDebugDirectionalArrow(GetWorld(), DebugLocation, DebugLocation + CurrentAdditiveVelocity,
			100.f, DebugColor, false, -1.f, (uint8)'\000', 10.f);

		FString DebugText = FString::Printf(TEXT("RootMotionAdditiveVelocity: %s (Speed: %.2f)"),
			*CurrentAdditiveVelocity.ToCompactString(), CurrentAdditiveVelocity.Size());
		DrawDebugString(GetWorld(), DebugLocation + FVector(0.f, 0.f, 5.f), DebugText, nullptr, DebugColor, 0.f, true);
	}

	// Root motion (override)
	if (CurrentRootMotion.HasOverrideVelocity())
	{
		const FColor DebugColor = FColor::Green;
		HeightOffset += 15.f;
		const FVector DebugLocation = TopOfCapsule + FVector(0.f, 0.f, HeightOffset);
		FString DebugText = FString::Printf(TEXT("Has Override RootMotion"));
		DrawDebugString(GetWorld(), DebugLocation, DebugText, nullptr, DebugColor, 0.f, true);
	}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
}*/

///////////////////////////
// Navigation Functions
///////////////////////////

void UVRCharacterMovementComponent::OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result)
{
	if (AVRCharacter* vrOwner = Cast<AVRCharacter>(GetCharacterOwner()))
	{
		vrOwner->NavigationMoveCompleted(RequestID, Result);
	}
}

bool UVRCharacterMovementComponent::TryToLeaveNavWalking()
{
	SetNavWalkingPhysics(false);

	bool bCanTeleport = true;
	if (CharacterOwner)
	{
		FVector CollisionFreeLocation;
		if (VRRootCapsule)
			CollisionFreeLocation = VRRootCapsule->OffsetComponentToWorld.GetLocation();
		else
			CollisionFreeLocation =	UpdatedComponent->GetComponentLocation();
		
		// Think I need to create a custom "FindTeleportSpot" function, it is using ComponentToWorld location
		bCanTeleport = GetWorld()->FindTeleportSpot(CharacterOwner, CollisionFreeLocation, UpdatedComponent->GetComponentRotation());
		if (bCanTeleport)
		{
			
			if (VRRootCapsule)
			{
				// Technically the same actor but i am keepign the usage convention for clarity.
				// Subtracting actor location from capsule to get difference in worldspace, then removing from collision free location
				// So that it uses the correct location.
				CharacterOwner->SetActorLocation(CollisionFreeLocation - (VRRootCapsule->OffsetComponentToWorld.GetLocation() - UpdatedComponent->GetComponentLocation()));
			}
			else
				CharacterOwner->SetActorLocation(CollisionFreeLocation);
		}
		else
		{
			SetNavWalkingPhysics(true);
		}
	}

	bWantsToLeaveNavWalking = !bCanTeleport;
	return bCanTeleport;
}

void UVRCharacterMovementComponent::PhysFlying(float deltaTime, int32 Iterations)
{
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	// Rewind the players position by the new capsule location
	RewindVRRelativeMovement();

	//RestorePreAdditiveRootMotionVelocity();
	//RestorePreAdditiveVRMotionVelocity();

	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
	{
		if (bCheatFlying && Acceleration.IsZero())
		{
			Velocity = FVector::ZeroVector;
		}
		const float Friction = 0.5f * GetPhysicsVolume()->FluidFriction;
		CalcVelocity(deltaTime, Friction, true, GetMaxBrakingDeceleration());
	}

//	ApplyRootMotionToVelocity(deltaTime);
//	ApplyVRMotionToVelocity(deltaTime);

	Iterations++;
	bJustTeleported = false;

	FVector OldLocation = UpdatedComponent->GetComponentLocation();
	const FVector Adjusted = Velocity * deltaTime;
	FHitResult Hit(1.f);
	SafeMoveUpdatedComponent(Adjusted + AdditionalVRInputVector, UpdatedComponent->GetComponentQuat(), true, Hit);

	if (Hit.Time < 1.f)
	{
		const FVector GravDir = FVector(0.f, 0.f, -1.f);
		const FVector VelDir = Velocity.GetSafeNormal();
		const float UpDown = GravDir | VelDir;

		bool bSteppedUp = false;
		if ((FMath::Abs(Hit.ImpactNormal.Z) < 0.2f) && (UpDown < 0.5f) && (UpDown > -0.2f) && CanStepUp(Hit))
		{
			float stepZ = UpdatedComponent->GetComponentLocation().Z;
			bSteppedUp = StepUp(GravDir, (Adjusted + AdditionalVRInputVector) * (1.f - Hit.Time) /*+ AdditionalVRInputVector.GetSafeNormal2D()*/, Hit, nullptr);
			if (bSteppedUp)
			{
				OldLocation.Z = UpdatedComponent->GetComponentLocation().Z + (OldLocation.Z - stepZ);
			}
		}

		if (!bSteppedUp)
		{
			//adjust and try again
			HandleImpact(Hit, deltaTime, Adjusted);
			SlideAlongSurface(Adjusted, (1.f - Hit.Time), Hit.Normal, Hit, true);
		}
	}

	if (!bJustTeleported && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
	{
		Velocity = ((UpdatedComponent->GetComponentLocation() - OldLocation) - AdditionalVRInputVector) / deltaTime;
	}
}

void UVRCharacterMovementComponent::PhysFalling(float deltaTime, int32 Iterations)
{
	SCOPE_CYCLE_COUNTER(STAT_CharPhysFalling);

	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	FVector FallAcceleration = GetFallingLateralAcceleration(deltaTime);
	FallAcceleration.Z = 0.f;
	const bool bHasAirControl = (FallAcceleration.SizeSquared2D() > 0.f);

	// Rewind the players position by the new capsule location
	RewindVRRelativeMovement();

	float remainingTime = deltaTime;
	while ((remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations))
	{
		Iterations++;
		const float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;

		const FVector OldLocation = UpdatedComponent->GetComponentLocation();
		const FVector OldCapsuleLocation = VRRootCapsule ? VRRootCapsule->OffsetComponentToWorld.GetLocation() : OldLocation;

		const FQuat PawnRotation = UpdatedComponent->GetComponentQuat();
		bJustTeleported = false;

		//RestorePreAdditiveRootMotionVelocity();

		FVector OldVelocity = Velocity;
		FVector VelocityNoAirControl = Velocity;

		// Apply input
		if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
		{
			// Compute VelocityNoAirControl
			if (bHasAirControl)
			{
				// Find velocity *without* acceleration.
				TGuardValue<FVector> RestoreAcceleration(Acceleration, FVector::ZeroVector);
				TGuardValue<FVector> RestoreVelocity(Velocity, Velocity);
				Velocity.Z = 0.f;
				CalcVelocity(timeTick, FallingLateralFriction, false, BrakingDecelerationFalling);
				VelocityNoAirControl = FVector(Velocity.X, Velocity.Y, OldVelocity.Z);
			}

			// Compute Velocity
			{
				// Acceleration = FallAcceleration for CalcVelocity(), but we restore it after using it.
				TGuardValue<FVector> RestoreAcceleration(Acceleration, FallAcceleration);
				Velocity.Z = 0.f;
				CalcVelocity(timeTick, FallingLateralFriction, false, BrakingDecelerationFalling);
				Velocity.Z = OldVelocity.Z;
			}

			// Just copy Velocity to VelocityNoAirControl if they are the same (ie no acceleration).
			if (!bHasAirControl)
			{
				VelocityNoAirControl = Velocity;
			}
		}

		// Apply gravity
		const FVector Gravity(0.f, 0.f, GetGravityZ());

		float GravityTime = timeTick;

		// If jump is providing force, gravity may be affected.
		if (CharacterOwner->JumpForceTimeRemaining > 0.0f)
		{
			// Consume some of the force time. Only the remaining time (if any) is affected by gravity when bApplyGravityWhileJumping=false.
			const float JumpForceTime = FMath::Min(CharacterOwner->JumpForceTimeRemaining, timeTick);
			GravityTime = bApplyGravityWhileJumping ? timeTick : FMath::Max(0.0f, timeTick - JumpForceTime);

			// Update Character state
			CharacterOwner->JumpForceTimeRemaining -= JumpForceTime;
			if (CharacterOwner->JumpForceTimeRemaining <= 0.0f)
			{
				CharacterOwner->ResetJumpState();
			}
		}

		Velocity = NewFallVelocity(Velocity, Gravity, GravityTime);
		VelocityNoAirControl = bHasAirControl ? NewFallVelocity(VelocityNoAirControl, Gravity, GravityTime) : Velocity;

		const FVector AirControlAccel = (Velocity - VelocityNoAirControl) / timeTick;

		//ApplyRootMotionToVelocity(timeTick);

		if (bNotifyApex && (Velocity.Z <= 0.f))
		{
			// Just passed jump apex since now going down
			bNotifyApex = false;
			NotifyJumpApex();
		}


		// Move
		FHitResult Hit(1.f);
		// Adding in the vector here because velocity doesn't care
		FVector Adjusted = (0.5f*(OldVelocity + Velocity) * timeTick) + ((AdditionalVRInputVector / deltaTime) * timeTick);
		SafeMoveUpdatedComponent(Adjusted, PawnRotation, true, Hit);

		if (!HasValidData())
		{
			return;
		}

		float LastMoveTimeSlice = timeTick;
		float subTimeTickRemaining = timeTick * (1.f - Hit.Time);

		if (IsSwimming()) //just entered water
		{
			remainingTime += subTimeTickRemaining;
			StartSwimmingVR(OldCapsuleLocation, OldVelocity, timeTick, remainingTime, Iterations);
			return;
		}
		else if (Hit.bBlockingHit)
		{
			if (IsValidLandingSpot(VRRootCapsule->OffsetComponentToWorld.GetLocation()/*UpdatedComponent->GetComponentLocation()*/, Hit))
			{
				remainingTime += subTimeTickRemaining;
				ProcessLanded(Hit, remainingTime, Iterations);
				return;
			}
			else
			{
				// Compute impact deflection based on final velocity, not integration step.
				// This allows us to compute a new velocity from the deflected vector, and ensures the full gravity effect is included in the slide result.
				Adjusted = Velocity * timeTick;

				// See if we can convert a normally invalid landing spot (based on the hit result) to a usable one.
				if (!Hit.bStartPenetrating && ShouldCheckForValidLandingSpot(timeTick, Adjusted, Hit))
				{
					/*const */FVector PawnLocation = UpdatedComponent->GetComponentLocation();
					if (VRRootCapsule)
						PawnLocation = VRRootCapsule->OffsetComponentToWorld.GetLocation();

					FFindFloorResult FloorResult;
					FindFloor(PawnLocation, FloorResult, false, NULL);
					if (FloorResult.IsWalkableFloor() && IsValidLandingSpot(PawnLocation, FloorResult.HitResult))
					{
						remainingTime += subTimeTickRemaining;
						ProcessLanded(FloorResult.HitResult, remainingTime, Iterations);
						return;
					}
				}

				HandleImpact(Hit, LastMoveTimeSlice, Adjusted);

				// If we've changed physics mode, abort.
				if (!HasValidData() || !IsFalling())
				{
					return;
				}

				// Limit air control based on what we hit.
				// We moved to the impact point using air control, but may want to deflect from there based on a limited air control acceleration.
				if (bHasAirControl)
				{
					const bool bCheckLandingSpot = false; // we already checked above.
					const FVector AirControlDeltaV = LimitAirControl(LastMoveTimeSlice, AirControlAccel, Hit, bCheckLandingSpot) * LastMoveTimeSlice;
					Adjusted = (VelocityNoAirControl + AirControlDeltaV) * LastMoveTimeSlice;
				}

				const FVector OldHitNormal = Hit.Normal;
				const FVector OldHitImpactNormal = Hit.ImpactNormal;
				FVector Delta = ComputeSlideVector(Adjusted, 1.f - Hit.Time, OldHitNormal, Hit);

				// Compute velocity after deflection (only gravity component for RootMotion)
				if (subTimeTickRemaining > KINDA_SMALL_NUMBER && !bJustTeleported)
				{
					const FVector NewVelocity = (Delta / subTimeTickRemaining);
					Velocity = HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() ? FVector(Velocity.X, Velocity.Y, NewVelocity.Z) : NewVelocity;
				}

				if (subTimeTickRemaining > KINDA_SMALL_NUMBER && (Delta | Adjusted) > 0.f)
				{
					// Move in deflected direction.
					SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);

					if (Hit.bBlockingHit)
					{
						// hit second wall
						LastMoveTimeSlice = subTimeTickRemaining;
						subTimeTickRemaining = subTimeTickRemaining * (1.f - Hit.Time);


						if (IsValidLandingSpot(VRRootCapsule->OffsetComponentToWorld.GetLocation()/*UpdatedComponent->GetComponentLocation()*/, Hit))
						{
							remainingTime += subTimeTickRemaining;
							ProcessLanded(Hit, remainingTime, Iterations);
							return;
						}

						HandleImpact(Hit, LastMoveTimeSlice, Delta);

						// If we've changed physics mode, abort.
						if (!HasValidData() || !IsFalling())
						{
							return;
						}

						// Act as if there was no air control on the last move when computing new deflection.
						if (bHasAirControl && Hit.Normal.Z > VERTICAL_SLOPE_NORMAL_Z)
						{
							const FVector LastMoveNoAirControl = VelocityNoAirControl * LastMoveTimeSlice;
							Delta = ComputeSlideVector(LastMoveNoAirControl, 1.f, OldHitNormal, Hit);
						}

						FVector PreTwoWallDelta = Delta;
						TwoWallAdjust(Delta, Hit, OldHitNormal);

						// Limit air control, but allow a slide along the second wall.
						if (bHasAirControl)
						{
							const bool bCheckLandingSpot = false; // we already checked above.
							const FVector AirControlDeltaV = LimitAirControl(subTimeTickRemaining, AirControlAccel, Hit, bCheckLandingSpot) * subTimeTickRemaining;

							// Only allow if not back in to first wall
							if (FVector::DotProduct(AirControlDeltaV, OldHitNormal) > 0.f)
							{
								Delta += (AirControlDeltaV * subTimeTickRemaining);
							}
						}

						// Compute velocity after deflection (only gravity component for RootMotion)
						if (subTimeTickRemaining > KINDA_SMALL_NUMBER && !bJustTeleported)
						{
							const FVector NewVelocity = (Delta / subTimeTickRemaining);
							Velocity = HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() ? FVector(Velocity.X, Velocity.Y, NewVelocity.Z) : NewVelocity;
						}

						// bDitch=true means that pawn is straddling two slopes, neither of which he can stand on
						bool bDitch = ((OldHitImpactNormal.Z > 0.f) && (Hit.ImpactNormal.Z > 0.f) && (FMath::Abs(Delta.Z) <= KINDA_SMALL_NUMBER) && ((Hit.ImpactNormal | OldHitImpactNormal) < 0.f));
						SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);
						if (Hit.Time == 0.f)
						{
							// if we are stuck then try to side step
							FVector SideDelta = (OldHitNormal + Hit.ImpactNormal).GetSafeNormal2D();
							if (SideDelta.IsNearlyZero())
							{
								SideDelta = FVector(OldHitNormal.Y, -OldHitNormal.X, 0).GetSafeNormal();
							}
							SafeMoveUpdatedComponent(SideDelta, PawnRotation, true, Hit);
						}

						if (bDitch || IsValidLandingSpot(VRRootCapsule->OffsetComponentToWorld.GetLocation()/*UpdatedComponent->GetComponentLocation()*/, Hit) || Hit.Time == 0.f)
						{
							remainingTime = 0.f;
							ProcessLanded(Hit, remainingTime, Iterations);
							return;
						}
						else if (GetPerchRadiusThreshold() > 0.f && Hit.Time == 1.f && OldHitImpactNormal.Z >= GetWalkableFloorZ())
						{
							// We might be in a virtual 'ditch' within our perch radius. This is rare.
							const FVector PawnLocation = UpdatedComponent->GetComponentLocation();
							const float ZMovedDist = FMath::Abs(PawnLocation.Z - OldLocation.Z);
							const float MovedDist2DSq = (PawnLocation - OldLocation).SizeSquared2D();
							if (ZMovedDist <= 0.2f * timeTick && MovedDist2DSq <= 4.f * timeTick)
							{
								Velocity.X += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);
								Velocity.Y += 0.25f * GetMaxSpeed() * (FMath::FRand() - 0.5f);
								Velocity.Z = FMath::Max<float>(JumpZVelocity * 0.25f, 1.f);
								Delta = Velocity * timeTick;
								SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);
							}
						}
					}
				}
			}
		}
		else
		{
			// We are finding the floor and adjusting here now
			// This matches the final falling Z up to the PhysWalking floor offset to prevent the visible hitch when going
			// From falling to walking.
			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, false, NULL);

			if (CurrentFloor.IsWalkableFloor())
			{
				// If the current floor distance is within the physwalking required floor offset
				if (CurrentFloor.GetDistanceToFloor() < (MIN_FLOOR_DIST + MAX_FLOOR_DIST) / 2)
				{
					// Adjust to correct height
					AdjustFloorHeight();

					SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);

					// If this is a valid landing spot, stop falling now so that we land correctly
					if (IsValidLandingSpot(VRRootCapsule->OffsetComponentToWorld.GetLocation()/*UpdatedComponent->GetComponentLocation()*/, CurrentFloor.HitResult))
					{
						remainingTime += subTimeTickRemaining;
						ProcessLanded(CurrentFloor.HitResult, remainingTime, Iterations);
						return;
					}
				}
			}
			else if (CurrentFloor.HitResult.bStartPenetrating)
			{
				// The floor check failed because it started in penetration
				// We do not want to try to move downward because the downward sweep failed, rather we'd like to try to pop out of the floor.
				FHitResult Hitt(CurrentFloor.HitResult);
				Hit.TraceEnd = Hit.TraceStart + FVector(0.f, 0.f, MAX_FLOOR_DIST);
				const FVector RequestedAdjustment = GetPenetrationAdjustment(Hit);
				ResolvePenetration(RequestedAdjustment, Hitt, UpdatedComponent->GetComponentQuat());
				bForceNextFloorCheck = true;
			}
		}

		if (Velocity.SizeSquared2D() <= KINDA_SMALL_NUMBER * 10.f)
		{
			Velocity.X = 0.f;
			Velocity.Y = 0.f;
		}
	}
}


void UVRCharacterMovementComponent::PhysNavWalking(float deltaTime, int32 Iterations)
{
	SCOPE_CYCLE_COUNTER(STAT_CharPhysNavWalking);

	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	// Root motion not for VR
	if ((!CharacterOwner || !CharacterOwner->Controller) && !bRunPhysicsWithNoController /*&& !HasRootMotion()*/)
	{
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		return;
	}

	// Rewind the players position by the new capsule location
	RewindVRRelativeMovement();

	//RestorePreAdditiveRootMotionVelocity();
	//RestorePreAdditiveVRMotionVelocity();

	// Ensure velocity is horizontal.
	MaintainHorizontalGroundVelocity();
	devCode(ensureMsgf(!Velocity.ContainsNaN(), TEXT("PhysNavWalking: Velocity contains NaN before CalcVelocity (%s)\n%s"), *GetPathNameSafe(this), *Velocity.ToString()));

	//bound acceleration
	Acceleration.Z = 0.f;
	//if (!HasRootMotion())
	//{
		CalcVelocity(deltaTime, GroundFriction, false, BrakingDecelerationWalking);
		devCode(ensureMsgf(!Velocity.ContainsNaN(), TEXT("PhysNavWalking: Velocity contains NaN after CalcVelocity (%s)\n%s"), *GetPathNameSafe(this), *Velocity.ToString()));
	//}

	//ApplyRootMotionToVelocity(deltaTime);
	ApplyVRMotionToVelocity(deltaTime);

	/*if (IsFalling())
	{
		// Root motion could have put us into Falling
		StartNewPhysics(deltaTime, Iterations);
		return;
	}*/

	Iterations++;

	FVector DesiredMove = Velocity;
	DesiredMove.Z = 0.f;

	//const FVector OldPlayerLocation = GetActorFeetLocation();
	const FVector OldLocation = GetActorFeetLocation();
	const FVector DeltaMove = DesiredMove * deltaTime;

	FVector AdjustedDest = OldLocation + DeltaMove;
	FNavLocation DestNavLocation;

	bool bSameNavLocation = false;
	if (CachedNavLocation.NodeRef != INVALID_NAVNODEREF)
	{
		if (bProjectNavMeshWalking)
		{
			const float DistSq2D = (OldLocation - CachedNavLocation.Location).SizeSquared2D();
			const float DistZ = FMath::Abs(OldLocation.Z - CachedNavLocation.Location.Z);

			const float TotalCapsuleHeight = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 2.0f;
			const float ProjectionScale = (OldLocation.Z > CachedNavLocation.Location.Z) ? NavMeshProjectionHeightScaleUp : NavMeshProjectionHeightScaleDown;
			const float DistZThr = TotalCapsuleHeight * FMath::Max(0.f, ProjectionScale);

			bSameNavLocation = (DistSq2D <= KINDA_SMALL_NUMBER) && (DistZ < DistZThr);
		}
		else
		{
			bSameNavLocation = CachedNavLocation.Location.Equals(OldLocation);
		}
	}


	if (DeltaMove.IsNearlyZero() && bSameNavLocation)
	{
		DestNavLocation = CachedNavLocation;
		UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("%s using cached navmesh location! (bProjectNavMeshWalking = %d)"), *GetNameSafe(CharacterOwner), bProjectNavMeshWalking);
	}
	else
	{
		SCOPE_CYCLE_COUNTER(STAT_CharNavProjectPoint);

		// Start the trace from the Z location of the last valid trace.
		// Otherwise if we are projecting our location to the underlying geometry and it's far above or below the navmesh,
		// we'll follow that geometry's plane out of range of valid navigation.
		if (bSameNavLocation && bProjectNavMeshWalking)
		{
			AdjustedDest.Z = CachedNavLocation.Location.Z;
		}

		// Find the point on the NavMesh
		const bool bHasNavigationData = FindNavFloor(AdjustedDest, DestNavLocation);
		if (!bHasNavigationData)
		{
			RestorePreAdditiveVRMotionVelocity();
			SetMovementMode(MOVE_Walking);
			return;
		}

		CachedNavLocation = DestNavLocation;
	}

	if (DestNavLocation.NodeRef != INVALID_NAVNODEREF)
	{
		FVector NewLocation(AdjustedDest.X, AdjustedDest.Y, DestNavLocation.Location.Z);
		if (bProjectNavMeshWalking)
		{
			SCOPE_CYCLE_COUNTER(STAT_CharNavProjectLocation);
			const float TotalCapsuleHeight = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 2.0f;
			const float UpOffset = TotalCapsuleHeight * FMath::Max(0.f, NavMeshProjectionHeightScaleUp);
			const float DownOffset = TotalCapsuleHeight * FMath::Max(0.f, NavMeshProjectionHeightScaleDown);
			NewLocation = ProjectLocationFromNavMesh(deltaTime, OldLocation, NewLocation, UpOffset, DownOffset);
		}

		FVector AdjustedDelta = NewLocation - OldLocation;

		if (!AdjustedDelta.IsNearlyZero())
		{
			// 4.16 UNCOMMENT
			FHitResult HitResult;
			SafeMoveUpdatedComponent(AdjustedDelta, UpdatedComponent->GetComponentQuat(), bSweepWhileNavWalking, HitResult);
			
			/* 4.16 Delete*/
			//const bool bSweep = UpdatedPrimitive ? UpdatedPrimitive->bGenerateOverlapEvents : false;
			//FHitResult HitResult;
			//SafeMoveUpdatedComponent(AdjustedDelta, UpdatedComponent->GetComponentQuat(), bSweep, HitResult);
			// End 4.16 delete
		}

		// Update velocity to reflect actual move
		if (!bJustTeleported /*&& !HasRootMotion()*/)
		{
			Velocity = (GetActorFeetLocation() - OldLocation) / deltaTime;
			MaintainHorizontalGroundVelocity();
		}

		bJustTeleported = false;
	}
	else
	{
		StartFalling(Iterations, deltaTime, deltaTime, DeltaMove, OldLocation);
	}

	RestorePreAdditiveVRMotionVelocity();
}

void UVRCharacterMovementComponent::PhysSwimming(float deltaTime, int32 Iterations)
{
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	// Rewind the players position by the new capsule location
	RewindVRRelativeMovement();

	//RestorePreAdditiveRootMotionVelocity();

	float NetFluidFriction = 0.f;
	float Depth = ImmersionDepth();
	float NetBuoyancy = Buoyancy * Depth;
	float OriginalAccelZ = Acceleration.Z;
	bool bLimitedUpAccel = false;

	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && (Velocity.Z > 0.33f * MaxSwimSpeed) && (NetBuoyancy != 0.f))
	{
		//damp positive Z out of water
		Velocity.Z = FMath::Max(0.33f * MaxSwimSpeed, Velocity.Z * Depth*Depth);
	}
	else if (Depth < 0.65f)
	{
		bLimitedUpAccel = (Acceleration.Z > 0.f);
		Acceleration.Z = FMath::Min(0.1f, Acceleration.Z);
	}

	Iterations++;
	FVector OldLocation = UpdatedComponent->GetComponentLocation();
	bJustTeleported = false;
	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
	{
		const float Friction = 0.5f * GetPhysicsVolume()->FluidFriction * Depth;
		CalcVelocity(deltaTime, Friction, true, GetMaxBrakingDeceleration());
		Velocity.Z += GetGravityZ() * deltaTime * (1.f - NetBuoyancy);
	}

	//ApplyRootMotionToVelocity(deltaTime);

	FVector Adjusted = Velocity * deltaTime;
	FHitResult Hit(1.f);
	float remainingTime = deltaTime * SwimVR(Adjusted + AdditionalVRInputVector, Hit);

	//may have left water - if so, script might have set new physics mode
	if (!IsSwimming())
	{
		StartNewPhysics(remainingTime, Iterations);
		return;
	}

	if (Hit.Time < 1.f && CharacterOwner)
	{
		HandleSwimmingWallHit(Hit, deltaTime);
		if (bLimitedUpAccel && (Velocity.Z >= 0.f))
		{
			// allow upward velocity at surface if against obstacle
			Velocity.Z += OriginalAccelZ * deltaTime;
			Adjusted = Velocity * (1.f - Hit.Time)*deltaTime;
			SwimVR(Adjusted, Hit);
			if (!IsSwimming())
			{
				StartNewPhysics(remainingTime, Iterations);
				return;
			}
		}

		const FVector GravDir = FVector(0.f, 0.f, -1.f);
		const FVector VelDir = Velocity.GetSafeNormal();
		const float UpDown = GravDir | VelDir;

		bool bSteppedUp = false;
		if ((FMath::Abs(Hit.ImpactNormal.Z) < 0.2f) && (UpDown < 0.5f) && (UpDown > -0.2f) && CanStepUp(Hit))
		{
			float stepZ = UpdatedComponent->GetComponentLocation().Z;
			const FVector RealVelocity = Velocity;
			Velocity.Z = 1.f;	// HACK: since will be moving up, in case pawn leaves the water
			bSteppedUp = StepUp(GravDir, (Adjusted + AdditionalVRInputVector) * (1.f - Hit.Time), Hit);
			if (bSteppedUp)
			{
				//may have left water - if so, script might have set new physics mode
				if (!IsSwimming())
				{
					StartNewPhysics(remainingTime, Iterations);
					return;
				}
				OldLocation.Z = UpdatedComponent->GetComponentLocation().Z + (OldLocation.Z - stepZ);
			}
			Velocity = RealVelocity;
		}

		if (!bSteppedUp)
		{
			//adjust and try again
			HandleImpact(Hit, deltaTime, Adjusted);
			SlideAlongSurface(Adjusted, (1.f - Hit.Time), Hit.Normal, Hit, true);
		}
	}

	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && !bJustTeleported && ((deltaTime - remainingTime) > KINDA_SMALL_NUMBER) && CharacterOwner)
	{
		bool bWaterJump = !GetPhysicsVolume()->bWaterVolume;
		float velZ = Velocity.Z;
		Velocity = ((UpdatedComponent->GetComponentLocation() - OldLocation) - AdditionalVRInputVector) / (deltaTime - remainingTime);
		if (bWaterJump)
		{
			Velocity.Z = velZ;
		}
	}

	if (!GetPhysicsVolume()->bWaterVolume && IsSwimming())
	{
		SetMovementMode(MOVE_Falling); //in case script didn't change it (w/ zone change)
	}

	//may have left water - if so, script might have set new physics mode
	if (!IsSwimming())
	{
		StartNewPhysics(remainingTime, Iterations);
	}
}


void UVRCharacterMovementComponent::StartSwimmingVR(FVector OldLocation, FVector OldVelocity, float timeTick, float remainingTime, int32 Iterations)
{
	if (remainingTime < MIN_TICK_TIME || timeTick < MIN_TICK_TIME)
	{
		return;
	}

	FVector NewLocation = VRRootCapsule ? VRRootCapsule->OffsetComponentToWorld.GetLocation() : UpdatedComponent->GetComponentLocation();

	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && !bJustTeleported)
	{
		Velocity = (NewLocation - OldLocation) / timeTick; //actual average velocity
		Velocity = 2.f*Velocity - OldVelocity; //end velocity has 2* accel of avg
		Velocity = Velocity.GetClampedToMaxSize(GetPhysicsVolume()->TerminalVelocity);
	}
	const FVector End = FindWaterLine(NewLocation, OldLocation);
	float waterTime = 0.f;
	if (End != NewLocation)
	{
		const float ActualDist = (NewLocation - OldLocation).Size();
		if (ActualDist > KINDA_SMALL_NUMBER)
		{
			waterTime = timeTick * (End - NewLocation).Size() / ActualDist;
			remainingTime += waterTime;
		}
		MoveUpdatedComponent(End - NewLocation, UpdatedComponent->GetComponentQuat(), true);
	}
	if (!HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && (Velocity.Z > 2.f*SWIMBOBSPEED) && (Velocity.Z < 0.f)) //allow for falling out of water
	{
		Velocity.Z = SWIMBOBSPEED - Velocity.Size2D() * 0.7f; //smooth bobbing
	}
	if ((remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations))
	{
		PhysSwimming(remainingTime, Iterations);
	}
}

float UVRCharacterMovementComponent::SwimVR(FVector Delta, FHitResult& Hit)
{
	FVector Start = VRRootCapsule ? VRRootCapsule->OffsetComponentToWorld.GetLocation() : UpdatedComponent->GetComponentLocation();
	
	float airTime = 0.f;
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);

	if (!GetPhysicsVolume()->bWaterVolume) //then left water
	{
		FVector NewLoc = VRRootCapsule ? VRRootCapsule->OffsetComponentToWorld.GetLocation() : UpdatedComponent->GetComponentLocation();

		const FVector End = FindWaterLine(Start, NewLoc);
		const float DesiredDist = Delta.Size();
		if (End != NewLoc && DesiredDist > KINDA_SMALL_NUMBER)
		{
			airTime = (End - NewLoc).Size() / DesiredDist;
			if (((NewLoc - Start) | (End - NewLoc)) > 0.f)
			{
				airTime = 0.f;
			}
			SafeMoveUpdatedComponent(End - NewLoc, UpdatedComponent->GetComponentQuat(), true, Hit);
		}
	}
	return airTime;
}

bool UVRCharacterMovementComponent::CheckWaterJump(FVector CheckPoint, FVector& WallNormal)
{
	if (!HasValidData())
	{
		return false;
	}
	FVector currentLoc = VRRootCapsule ? VRRootCapsule->OffsetComponentToWorld.GetLocation() : UpdatedComponent->GetComponentLocation();

	// check if there is a wall directly in front of the swimming pawn
	CheckPoint.Z = 0.f;
	FVector CheckNorm = CheckPoint.GetSafeNormal();
	float PawnCapsuleRadius, PawnCapsuleHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnCapsuleRadius, PawnCapsuleHalfHeight);
	CheckPoint = currentLoc + 1.2f * PawnCapsuleRadius * CheckNorm;
	FVector Extent(PawnCapsuleRadius, PawnCapsuleRadius, PawnCapsuleHalfHeight);
	FHitResult HitInfo(1.f);
	FCollisionQueryParams CapsuleParams(SCENE_QUERY_STAT(CheckWaterJump), false, CharacterOwner);
	FCollisionResponseParams ResponseParam;
	InitCollisionParams(CapsuleParams, ResponseParam);
	FCollisionShape CapsuleShape = GetPawnCapsuleCollisionShape(SHRINK_None);
	const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();
	bool bHit = GetWorld()->SweepSingleByChannel(HitInfo, currentLoc, CheckPoint, FQuat::Identity, CollisionChannel, CapsuleShape, CapsuleParams, ResponseParam);

	if (bHit && !Cast<APawn>(HitInfo.GetActor()))
	{
		// hit a wall - check if it is low enough
		WallNormal = -1.f * HitInfo.ImpactNormal;
		FVector Start = currentLoc;//UpdatedComponent->GetComponentLocation();
		Start.Z += MaxOutOfWaterStepHeight;
		CheckPoint = Start + 3.2f * PawnCapsuleRadius * WallNormal;
		FCollisionQueryParams LineParams(SCENE_QUERY_STAT(CheckWaterJump), true, CharacterOwner);
		FCollisionResponseParams LineResponseParam;
		InitCollisionParams(LineParams, LineResponseParam);
		bHit = GetWorld()->LineTraceSingleByChannel(HitInfo, Start, CheckPoint, CollisionChannel, LineParams, LineResponseParam);
		// if no high obstruction, or it's a valid floor, then pawn can jump out of water
		return !bHit || IsWalkable(HitInfo);
	}
	return false;
}



void UVRCharacterMovementComponent::ProcessLanded(const FHitResult& Hit, float remainingTime, int32 Iterations)
{
	SCOPE_CYCLE_COUNTER(STAT_CharProcessLanded);

	if (CharacterOwner && CharacterOwner->ShouldNotifyLanded(Hit))
	{
		CharacterOwner->Landed(Hit);
	}
	if (IsFalling())
	{
		
		if (GetGroundMovementMode() == MOVE_NavWalking)
		{
			// verify navmesh projection and current floor
			// otherwise movement will be stuck in infinite loop:
			// navwalking -> (no navmesh) -> falling -> (standing on something) -> navwalking -> ....

			const FVector TestLocation = GetActorFeetLocation();
			FNavLocation NavLocation;

			const bool bHasNavigationData = FindNavFloor(TestLocation, NavLocation);
			if (!bHasNavigationData || NavLocation.NodeRef == INVALID_NAVNODEREF)
			{
				SetGroundMovementMode(MOVE_Walking);
				//GroundMovementMode = MOVE_Walking;
				UE_LOG(LogVRCharacterMovement, Verbose, TEXT("ProcessLanded(): %s tried to go to NavWalking but couldn't find NavMesh! Using Walking instead."), *GetNameSafe(CharacterOwner));
			}
		}

		SetPostLandedPhysics(Hit);
	}

	IPathFollowingAgentInterface* PFAgent = GetPathFollowingAgent();
	if (PFAgent)
	{
		PFAgent->OnLanded();
	}

	StartNewPhysics(remainingTime, Iterations);
}

///////////////////////////
// End Navigation Functions
///////////////////////////

void UVRCharacterMovementComponent::PostPhysicsTickComponent(float DeltaTime, FCharacterMovementComponentPostPhysicsTickFunction& ThisTickFunction)
{
	if (bDeferUpdateBasedMovement)
	{
		FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);
		UpdateBasedMovement(DeltaTime);
		SaveBaseLocation();
		bDeferUpdateBasedMovement = false;
	}
}


void UVRCharacterMovementComponent::SimulateMovement(float DeltaSeconds)
{
	if (!HasValidData() || UpdatedComponent->Mobility != EComponentMobility::Movable || UpdatedComponent->IsSimulatingPhysics())
	{
		return;
	}

	const bool bIsSimulatedProxy = (CharacterOwner->Role == ROLE_SimulatedProxy);

	// Workaround for replication not being updated initially
	if (bIsSimulatedProxy &&
		CharacterOwner->ReplicatedMovement.Location.IsZero() &&
		CharacterOwner->ReplicatedMovement.Rotation.IsZero() &&
		CharacterOwner->ReplicatedMovement.LinearVelocity.IsZero())
	{
		return;
	}

	// If base is not resolved on the client, we should not try to simulate at all
	if (CharacterOwner->GetReplicatedBasedMovement().IsBaseUnresolved())
	{
		UE_LOG(LogVRCharacterMovement, Verbose, TEXT("Base for simulated character '%s' is not resolved on client, skipping SimulateMovement"), *CharacterOwner->GetName());
		return;
	}

	FVector OldVelocity;
	FVector OldLocation;

	// Scoped updates can improve performance of multiple MoveComponent calls.
	{
		FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);

		bool bHandledNetUpdate = false;
		if (bIsSimulatedProxy)
		{
			// Handle network changes
			if (bNetworkUpdateReceived)
			{
				bNetworkUpdateReceived = false;
				bHandledNetUpdate = true;
				UE_LOG(LogVRCharacterMovement, Verbose, TEXT("Proxy %s received net update"), *GetNameSafe(CharacterOwner));
				if (bNetworkMovementModeChanged)
				{
					ApplyNetworkMovementMode(CharacterOwner->GetReplicatedMovementMode());
					bNetworkMovementModeChanged = false;
				}
				else if (bJustTeleported || bForceNextFloorCheck)
				{
					// Make sure floor is current. We will continue using the replicated base, if there was one.
					bJustTeleported = false;
					UpdateFloorFromAdjustment();
				}
			}
			else if (bForceNextFloorCheck)
			{
				UpdateFloorFromAdjustment();
			}
		}

		if (MovementMode == MOVE_None)
		{
			ClearAccumulatedForces();
			return;
		}

		//TODO: Also ApplyAccumulatedForces()?
		HandlePendingLaunch();
		ClearAccumulatedForces();

		Acceleration = Velocity.GetSafeNormal();	// Not currently used for simulated movement
		AnalogInputModifier = 1.0f;				// Not currently used for simulated movement

		MaybeUpdateBasedMovement(DeltaSeconds);

		// simulated pawns predict location
		OldVelocity = Velocity;
		OldLocation = UpdatedComponent->GetComponentLocation();

		static const auto CVarNetEnableSkipProxyPredictionOnNetUpdate = IConsoleManager::Get().FindConsoleVariable(TEXT("p.NetEnableSkipProxyPredictionOnNetUpdate"));
		// May only need to simulate forward on frames where we haven't just received a new position update.
		if (!bHandledNetUpdate || !bNetworkSkipProxyPredictionOnNetUpdate || !CVarNetEnableSkipProxyPredictionOnNetUpdate->GetInt())
		{
			UE_LOG(LogVRCharacterMovement, Verbose, TEXT("Proxy %s simulating movement"), *GetNameSafe(CharacterOwner));
			FStepDownResult StepDownResult;
			MoveSmooth(Velocity, DeltaSeconds, &StepDownResult);

			// find floor and check if falling
			if (IsMovingOnGround() || MovementMode == MOVE_Falling)
			{
				const bool bSimGravityDisabled = (CharacterOwner->bSimGravityDisabled && bIsSimulatedProxy);
				if (StepDownResult.bComputedFloor)
				{
					CurrentFloor = StepDownResult.FloorResult;
				}
				else if (Velocity.Z <= 0.f)
				{
					FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, Velocity.IsZero(), NULL);
				}
				else
				{
					CurrentFloor.Clear();
				}

				if (!CurrentFloor.IsWalkableFloor())
				{
					if (!bSimGravityDisabled)
					{
						// No floor, must fall.
						if (Velocity.Z <= 0.f || bApplyGravityWhileJumping || !CharacterOwner->IsJumpProvidingForce())
						{
							Velocity = NewFallVelocity(Velocity, FVector(0.f, 0.f, GetGravityZ()), DeltaSeconds);
						}
					}
					SetMovementMode(MOVE_Falling);
				}
				else
				{
					// Walkable floor
					if (IsMovingOnGround())
					{
						AdjustFloorHeight();
						SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
					}
					else if (MovementMode == MOVE_Falling)
					{
						if (CurrentFloor.FloorDist <= MIN_FLOOR_DIST || (bSimGravityDisabled && CurrentFloor.FloorDist <= MAX_FLOOR_DIST))
						{
							// Landed
							SetPostLandedPhysics(CurrentFloor.HitResult);
						}
						else
						{
							if (!bSimGravityDisabled)
							{
								// Continue falling.
								Velocity = NewFallVelocity(Velocity, FVector(0.f, 0.f, GetGravityZ()), DeltaSeconds);
							}
							CurrentFloor.Clear();
						}
					}
				}
			}
		}
		else
		{
			UE_LOG(LogVRCharacterMovement, Verbose, TEXT("Proxy %s SKIPPING simulate movement"), *GetNameSafe(CharacterOwner));
		}

		// consume path following requested velocity
		bHasRequestedVelocity = false;

		OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);
	} // End scoped movement update

	  // Call custom post-movement events. These happen after the scoped movement completes in case the events want to use the current state of overlaps etc.
	CallMovementUpdateDelegate(DeltaSeconds, OldLocation, OldVelocity);

	MaybeSaveBaseLocation();
	UpdateComponentVelocity();
	bJustTeleported = false;

	LastUpdateLocation = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
	LastUpdateRotation = UpdatedComponent ? UpdatedComponent->GetComponentQuat() : FQuat::Identity;
	LastUpdateVelocity = Velocity;
}

void UVRCharacterMovementComponent::MoveSmooth(const FVector& InVelocity, const float DeltaSeconds, FStepDownResult* OutStepDownResult)
{
	if (!HasValidData())
	{
		return;
	}

	// Custom movement mode.
	// Custom movement may need an update even if there is zero velocity.
	if (MovementMode == MOVE_Custom)
	{
		FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);
		PhysCustom(DeltaSeconds, 0);
		return;
	}

	FVector Delta = InVelocity * DeltaSeconds;
	if (Delta.IsZero())
	{
		return;
	}

	FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);

	if (IsMovingOnGround())
	{
		MoveAlongFloor(InVelocity, DeltaSeconds, OutStepDownResult);
	}
	else
	{
		FHitResult Hit(1.f);
		SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);

		if (Hit.IsValidBlockingHit())
		{
			bool bSteppedUp = false;

			if (IsFlying())
			{
				if (CanStepUp(Hit))
				{
					OutStepDownResult = NULL; // No need for a floor when not walking.
					if (FMath::Abs(Hit.ImpactNormal.Z) < 0.2f)
					{
						const FVector GravDir = FVector(0.f, 0.f, -1.f);
						const FVector DesiredDir = Delta.GetSafeNormal();
						const float UpDown = GravDir | DesiredDir;
						if ((UpDown < 0.5f) && (UpDown > -0.2f))
						{
							bSteppedUp = StepUp(GravDir, Delta * (1.f - Hit.Time), Hit, OutStepDownResult);
						}
					}
				}
			}

			// If StepUp failed, try sliding.
			if (!bSteppedUp)
			{
				SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, false);
			}
		}
	}
}

void UVRCharacterMovementComponent::SendClientAdjustment()
{
	if (!HasValidData())
	{
		return;
	}

	FNetworkPredictionData_Server_Character* ServerData = GetPredictionData_Server_Character();
	check(ServerData);

	if (ServerData->PendingAdjustment.TimeStamp <= 0.f)
	{
		return;
	}

	const float CurrentTime = GetWorld()->GetTimeSeconds();
	if (ServerData->PendingAdjustment.bAckGoodMove)
	{
		// just notify client this move was received
		if (CurrentTime - ServerLastClientGoodMoveAckTime > NetworkMinTimeBetweenClientAckGoodMoves)
		{
			ServerLastClientGoodMoveAckTime = CurrentTime;
			ClientAckGoodMove(ServerData->PendingAdjustment.TimeStamp);
		}
	}
	else
	{
		// We won't be back in here until the next client move and potential correction is received, so use the correct time now.
		// Protect against bad data by taking appropriate min/max of editable values.
		const float AdjustmentTimeThreshold = bNetworkLargeClientCorrection ?
			FMath::Min(NetworkMinTimeBetweenClientAdjustmentsLargeCorrection, NetworkMinTimeBetweenClientAdjustments) :
			FMath::Max(NetworkMinTimeBetweenClientAdjustmentsLargeCorrection, NetworkMinTimeBetweenClientAdjustments);

		// Check if correction is throttled based on time limit between updates.
		if (CurrentTime - ServerLastClientAdjustmentTime > AdjustmentTimeThreshold)
		{
			ServerLastClientAdjustmentTime = CurrentTime;

			const bool bIsPlayingNetworkedRootMotionMontage = CharacterOwner->IsPlayingNetworkedRootMotionMontage();
			if (HasRootMotionSources())
			{
				FRotator Rotation = ServerData->PendingAdjustment.NewRot.GetNormalized();
				FVector_NetQuantizeNormal CompressedRotation(Rotation.Pitch / 180.f, Rotation.Yaw / 180.f, Rotation.Roll / 180.f);
				ClientAdjustRootMotionSourcePosition
				(
					ServerData->PendingAdjustment.TimeStamp,
					CurrentRootMotion,
					bIsPlayingNetworkedRootMotionMontage,
					bIsPlayingNetworkedRootMotionMontage ? CharacterOwner->GetRootMotionAnimMontageInstance()->GetPosition() : -1.f,
					ServerData->PendingAdjustment.NewLoc,
					CompressedRotation,
					ServerData->PendingAdjustment.NewVel.Z,
					ServerData->PendingAdjustment.NewBase,
					ServerData->PendingAdjustment.NewBaseBoneName,
					ServerData->PendingAdjustment.NewBase != NULL,
					ServerData->PendingAdjustment.bBaseRelativePosition,
					PackNetworkMovementMode()
				);
			}
			else if (bIsPlayingNetworkedRootMotionMontage)
			{
				FRotator Rotation = ServerData->PendingAdjustment.NewRot.GetNormalized();
				FVector_NetQuantizeNormal CompressedRotation(Rotation.Pitch / 180.f, Rotation.Yaw / 180.f, Rotation.Roll / 180.f);
				ClientAdjustRootMotionPosition
				(
					ServerData->PendingAdjustment.TimeStamp,
					CharacterOwner->GetRootMotionAnimMontageInstance()->GetPosition(),
					ServerData->PendingAdjustment.NewLoc,
					CompressedRotation,
					ServerData->PendingAdjustment.NewVel.Z,
					ServerData->PendingAdjustment.NewBase,
					ServerData->PendingAdjustment.NewBaseBoneName,
					ServerData->PendingAdjustment.NewBase != NULL,
					ServerData->PendingAdjustment.bBaseRelativePosition,
					PackNetworkMovementMode()
				);
			}
			else if (ServerData->PendingAdjustment.NewVel.IsZero())
			{
				ClientVeryShortAdjustPositionVR
				(
					ServerData->PendingAdjustment.TimeStamp,
					ServerData->PendingAdjustment.NewLoc,
					FRotator::CompressAxisToShort(ServerData->PendingAdjustment.NewRot.Yaw),
					ServerData->PendingAdjustment.NewBase,
					ServerData->PendingAdjustment.NewBaseBoneName,
					ServerData->PendingAdjustment.NewBase != NULL,
					ServerData->PendingAdjustment.bBaseRelativePosition,
					PackNetworkMovementMode()
				);
			}
			else
			{
				ClientAdjustPositionVR
				(
					ServerData->PendingAdjustment.TimeStamp,
					ServerData->PendingAdjustment.NewLoc,
					FRotator::CompressAxisToShort(ServerData->PendingAdjustment.NewRot.Yaw),
					ServerData->PendingAdjustment.NewVel,
					ServerData->PendingAdjustment.NewBase,
					ServerData->PendingAdjustment.NewBaseBoneName,
					ServerData->PendingAdjustment.NewBase != NULL,
					ServerData->PendingAdjustment.bBaseRelativePosition,
					PackNetworkMovementMode()
				);
			}
		}
	}

	ServerData->PendingAdjustment.TimeStamp = 0;
	ServerData->PendingAdjustment.bAckGoodMove = false;
	ServerData->bForceClientUpdate = false;
}


void UVRCharacterMovementComponent::ClientVeryShortAdjustPositionVR(float TimeStamp, FVector NewLoc, uint16 NewYaw, UPrimitiveComponent* NewBase, FName NewBaseBoneName, bool bHasBase, bool bBaseRelativePosition, uint8 ServerMovementMode)
{
	((AVRCharacter*)CharacterOwner)->ClientVeryShortAdjustPositionVR(TimeStamp, NewLoc, NewYaw, NewBase, NewBaseBoneName, bHasBase, bBaseRelativePosition, ServerMovementMode);
}

void UVRCharacterMovementComponent::ClientVeryShortAdjustPositionVR_Implementation
(
	float TimeStamp,
	FVector NewLoc,
	uint16 NewYaw,
	UPrimitiveComponent* NewBase,
	FName NewBaseBoneName,
	bool bHasBase,
	bool bBaseRelativePosition,
	uint8 ServerMovementMode
)
{
	if (HasValidData())
	{
		ClientAdjustPositionVR(TimeStamp, NewLoc, NewYaw, FVector::ZeroVector, NewBase, NewBaseBoneName, bHasBase, bBaseRelativePosition, ServerMovementMode);
	}
}


void UVRCharacterMovementComponent::ClientAdjustPositionVR(float TimeStamp, FVector NewLoc, uint16 NewYaw, FVector NewVel, UPrimitiveComponent* NewBase, FName NewBaseBoneName, bool bHasBase, bool bBaseRelativePosition, uint8 ServerMovementMode)
{
	((AVRCharacter*)CharacterOwner)->ClientAdjustPositionVR(TimeStamp, NewLoc, NewYaw, NewVel, NewBase, NewBaseBoneName, bHasBase, bBaseRelativePosition, ServerMovementMode);
}

void UVRCharacterMovementComponent::ClientAdjustPositionVR_Implementation
(
	float TimeStamp,
	FVector NewLocation,
	uint16 NewYaw,
	FVector NewVelocity,
	UPrimitiveComponent* NewBase,
	FName NewBaseBoneName,
	bool bHasBase,
	bool bBaseRelativePosition,
	uint8 ServerMovementMode
)
{
	if (!HasValidData() || !IsActive())
	{
		return;
	}


	FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
	check(ClientData);

	// Make sure the base actor exists on this client.
	const bool bUnresolvedBase = bHasBase && (NewBase == NULL);
	if (bUnresolvedBase)
	{
		if (bBaseRelativePosition)
		{
			UE_LOG(LogNetPlayerMovement, Warning, TEXT("ClientAdjustPosition_Implementation could not resolve the new relative movement base actor, ignoring server correction!"));
			return;
		}
		else
		{
			UE_LOG(LogNetPlayerMovement, Verbose, TEXT("ClientAdjustPosition_Implementation could not resolve the new absolute movement base actor, but WILL use the position!"));
		}
	}

	// Ack move if it has not expired.
	int32 MoveIndex = ClientData->GetSavedMoveIndex(TimeStamp);
	if (MoveIndex == INDEX_NONE)
	{
		if (ClientData->LastAckedMove.IsValid())
		{
			UE_LOG(LogNetPlayerMovement, Log, TEXT("ClientAdjustPosition_Implementation could not find Move for TimeStamp: %f, LastAckedTimeStamp: %f, CurrentTimeStamp: %f"), TimeStamp, ClientData->LastAckedMove->TimeStamp, ClientData->CurrentTimeStamp);
		}
		return;
	}

	if (!bUseClientControlRotation)
	{
		float YawValue = FRotator::DecompressAxisFromShort(NewYaw);
		// Trust the server's control yaw
		if (ClientData->LastAckedMove.IsValid() && !FMath::IsNearlyEqual(ClientData->LastAckedMove->SavedControlRotation.Yaw, YawValue))
		{
			if (AVRBaseCharacter * BaseChar = Cast<AVRBaseCharacter>(CharacterOwner))
			{
				if (BaseChar->bUseControllerRotationYaw)
				{
					AController * myController = BaseChar->GetController();
					if (myController)
					{
						//FRotator newRot = myController->GetControlRotation();
						myController->SetControlRotation(FRotator(0.f, YawValue, 0.f));//(ClientData->LastAckedMove->SavedControlRotation);
					}
				}

				BaseChar->SetActorRotation(FRotator(0.f, YawValue, 0.f));
			}
		}
	}

	ClientData->AckMove(MoveIndex);

	FVector WorldShiftedNewLocation;
	//  Received Location is relative to dynamic base
	if (bBaseRelativePosition)
	{
		FVector BaseLocation;
		FQuat BaseRotation;
		MovementBaseUtility::GetMovementBaseTransform(NewBase, NewBaseBoneName, BaseLocation, BaseRotation); // TODO: error handling if returns false		
		WorldShiftedNewLocation = NewLocation + BaseLocation;
	}
	else
	{
		WorldShiftedNewLocation = FRepMovement::RebaseOntoLocalOrigin(NewLocation, this);
	}


	// Trigger event
	OnClientCorrectionReceived(*ClientData, TimeStamp, NewLocation, NewVelocity, NewBase, NewBaseBoneName, bHasBase, bBaseRelativePosition, ServerMovementMode);

	// Trust the server's positioning.
	UpdatedComponent->SetWorldLocation(WorldShiftedNewLocation, false);
	Velocity = NewVelocity;

	// Trust the server's movement mode
	UPrimitiveComponent* PreviousBase = CharacterOwner->GetMovementBase();
	ApplyNetworkMovementMode(ServerMovementMode);

	// Set base component
	UPrimitiveComponent* FinalBase = NewBase;
	FName FinalBaseBoneName = NewBaseBoneName;
	if (bUnresolvedBase)
	{
		check(NewBase == NULL);
		check(!bBaseRelativePosition);

		// We had an unresolved base from the server
		// If walking, we'd like to continue walking if possible, to avoid falling for a frame, so try to find a base where we moved to.
		if (PreviousBase)
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, false, NULL); //-V595
			if (CurrentFloor.IsWalkableFloor())
			{
				FinalBase = CurrentFloor.HitResult.Component.Get();
				FinalBaseBoneName = CurrentFloor.HitResult.BoneName;
			}
			else
			{
				FinalBase = nullptr;
				FinalBaseBoneName = NAME_None;
			}
		}
	}
	SetBase(FinalBase, FinalBaseBoneName);

	// Update floor at new location
	UpdateFloorFromAdjustment();
	bJustTeleported = true;

	// Even if base has not changed, we need to recompute the relative offsets (since we've moved).
	SaveBaseLocation();

	LastUpdateLocation = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
	LastUpdateRotation = UpdatedComponent ? UpdatedComponent->GetComponentQuat() : FQuat::Identity;
	LastUpdateVelocity = Velocity;

	UpdateComponentVelocity();
	ClientData->bUpdatePosition = true;
}

bool UVRCharacterMovementComponent::ServerCheckClientErrorVR(float ClientTimeStamp, float DeltaTime, const FVector& Accel, const FVector& ClientWorldLocation, float ClientYaw, const FVector& RelativeClientLocation, UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName, uint8 ClientMovementMode)
{
	// Check location difference against global setting
	if (!bIgnoreClientMovementErrorChecksAndCorrection)
	{
		const FVector LocDiff = UpdatedComponent->GetComponentLocation() - ClientWorldLocation;

#if ROOT_MOTION_DEBUG
		if (RootMotionSourceDebug::CVarDebugRootMotionSources.GetValueOnAnyThread() == 1)
		{
			FString AdjustedDebugString = FString::Printf(TEXT("ServerCheckClientError LocDiff(%.1f) ExceedsAllowablePositionError(%d) TimeStamp(%f)"),
				LocDiff.Size(), GetDefault<AGameNetworkManager>()->ExceedsAllowablePositionError(LocDiff), ClientTimeStamp);
			RootMotionSourceDebug::PrintOnScreen(*CharacterOwner, AdjustedDebugString);
		}
#endif
		if (GetDefault<AGameNetworkManager>()->ExceedsAllowablePositionError(LocDiff))
		{
			bNetworkLargeClientCorrection = (LocDiff.SizeSquared() > FMath::Square(NetworkLargeClientCorrectionDistance));
			return true;
		}
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		static const auto CVarNetForceClientAdjustmentPercent = IConsoleManager::Get().FindConsoleVariable(TEXT("p.NetForceClientAdjustmentPercent"));
		if (CVarNetForceClientAdjustmentPercent->GetFloat() > SMALL_NUMBER)
		{
			if (FMath::SRand() < CVarNetForceClientAdjustmentPercent->GetFloat())
			{
				UE_LOG(LogVRCharacterMovement, VeryVerbose, TEXT("** ServerCheckClientError forced by p.NetForceClientAdjustmentPercent"));
				return true;
			}
		}
#endif
	}
	else
	{
#if !UE_BUILD_SHIPPING
		static const auto CVarNetShowCorrections = IConsoleManager::Get().FindConsoleVariable(TEXT("p.NetShowCorrections"));
		if (CVarNetShowCorrections->GetInt() != 0)
		{
			UE_LOG(LogVRCharacterMovement, Warning, TEXT("*** Server: %s is set to ignore error checks and corrections."), *GetNameSafe(CharacterOwner));
		}
#endif // !UE_BUILD_SHIPPING
	}

	// Check for disagreement in movement mode
	const uint8 CurrentPackedMovementMode = PackNetworkMovementMode();
	if (CurrentPackedMovementMode != ClientMovementMode)
	{
		return true;
	}
	
	// If we are rolling back client rotation
	if (!bUseClientControlRotation && !FMath::IsNearlyEqual(FRotator::ClampAxis(ClientYaw), FRotator::ClampAxis(UpdatedComponent->GetComponentRotation().Yaw), CharacterMovementComponentStatics::fRotationCorrectionThreshold))
	{
		return true;
	}

	return false;
}


void UVRCharacterMovementComponent::ServerMoveHandleClientErrorVR(float ClientTimeStamp, float DeltaTime, const FVector& Accel, const FVector& RelativeClientLoc, float ClientYaw, UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName, uint8 ClientMovementMode)
{
	if (RelativeClientLoc == FVector(1.f, 2.f, 3.f)) // first part of double servermove
	{
		return;
	}

	FNetworkPredictionData_Server_Character* ServerData = GetPredictionData_Server_Character();
	check(ServerData);

	// Don't prevent more recent updates from being sent if received this frame.
	// We're going to send out an update anyway, might as well be the most recent one.
	APlayerController* PC = Cast<APlayerController>(CharacterOwner->GetController());
	if ((ServerData->LastUpdateTime != GetWorld()->TimeSeconds) && GetDefault<AGameNetworkManager>()->WithinUpdateDelayBounds(PC, ServerData->LastUpdateTime))
	{
		return;
	}

	// Offset may be relative to base component
	FVector ClientLoc = RelativeClientLoc;
	if (MovementBaseUtility::UseRelativeLocation(ClientMovementBase))
	{
		FVector BaseLocation;
		FQuat BaseRotation;
		MovementBaseUtility::GetMovementBaseTransform(ClientMovementBase, ClientBaseBoneName, BaseLocation, BaseRotation);
		ClientLoc += BaseLocation;
	}

	// Compute the client error from the server's position
	// If client has accumulated a noticeable positional error, correct them.
	bNetworkLargeClientCorrection = ServerData->bForceClientUpdate;

	if (ServerData->bForceClientUpdate || ServerCheckClientErrorVR(ClientTimeStamp, DeltaTime, Accel, ClientLoc, ClientYaw, RelativeClientLoc, ClientMovementBase, ClientBaseBoneName, ClientMovementMode))
	{
		UPrimitiveComponent* MovementBase = CharacterOwner->GetMovementBase();
		ServerData->PendingAdjustment.NewVel = Velocity;
		ServerData->PendingAdjustment.NewBase = MovementBase;
		ServerData->PendingAdjustment.NewBaseBoneName = CharacterOwner->GetBasedMovement().BoneName;
		ServerData->PendingAdjustment.NewLoc = FRepMovement::RebaseOntoZeroOrigin(UpdatedComponent->GetComponentLocation(), this);
		ServerData->PendingAdjustment.NewRot = UpdatedComponent->GetComponentRotation();

		ServerData->PendingAdjustment.bBaseRelativePosition = MovementBaseUtility::UseRelativeLocation(MovementBase);
		if (ServerData->PendingAdjustment.bBaseRelativePosition)
		{
			// Relative location
			ServerData->PendingAdjustment.NewLoc = CharacterOwner->GetBasedMovement().Location;

			// TODO: this could be a relative rotation, but all client corrections ignore rotation right now except the root motion one, which would need to be updated.
			//ServerData->PendingAdjustment.NewRot = CharacterOwner->GetBasedMovement().Rotation;
		}

#if !UE_BUILD_SHIPPING
		static const auto CVarNetShowCorrections = IConsoleManager::Get().FindConsoleVariable(TEXT("p.NetShowCorrections"));
		static const auto CVarNetCorrectionLifetime = IConsoleManager::Get().FindConsoleVariable(TEXT("p.NetCorrectionLifetime"));
		if (CVarNetShowCorrections->GetInt() != 0)
		{
			const FVector LocDiff = UpdatedComponent->GetComponentLocation() - ClientLoc;
			const FString BaseString = MovementBase ? MovementBase->GetPathName(MovementBase->GetOutermost()) : TEXT("None");
			UE_LOG(LogVRCharacterMovement, Warning, TEXT("*** Server: Error for %s at Time=%.3f is %3.3f LocDiff(%s) ClientLoc(%s) ServerLoc(%s) Base: %s Bone: %s Accel(%s) Velocity(%s)"),
				*GetNameSafe(CharacterOwner), ClientTimeStamp, LocDiff.Size(), *LocDiff.ToString(), *ClientLoc.ToString(), *UpdatedComponent->GetComponentLocation().ToString(), *BaseString, *ServerData->PendingAdjustment.NewBaseBoneName.ToString(), *Accel.ToString(), *Velocity.ToString());
			const float DebugLifetime = CVarNetCorrectionLifetime->GetFloat();
			DrawDebugCapsule(GetWorld(), UpdatedComponent->GetComponentLocation(), CharacterOwner->GetSimpleCollisionHalfHeight(), CharacterOwner->GetSimpleCollisionRadius(), FQuat::Identity, FColor(100, 255, 100), true, DebugLifetime);
			DrawDebugCapsule(GetWorld(), ClientLoc, CharacterOwner->GetSimpleCollisionHalfHeight(), CharacterOwner->GetSimpleCollisionRadius(), FQuat::Identity, FColor(255, 100, 100), true, DebugLifetime);
		}
#endif

		ServerData->LastUpdateTime = GetWorld()->TimeSeconds;
		ServerData->PendingAdjustment.DeltaTime = DeltaTime;
		ServerData->PendingAdjustment.TimeStamp = ClientTimeStamp;
		ServerData->PendingAdjustment.bAckGoodMove = false;
		ServerData->PendingAdjustment.MovementMode = PackNetworkMovementMode();

		//PerfCountersIncrement(PerfCounter_NumServerMoveCorrections);
	}
	else
	{
		if (GetDefault<AGameNetworkManager>()->ClientAuthorativePosition)
		{
			const FVector LocDiff = UpdatedComponent->GetComponentLocation() - ClientLoc; //-V595
			if (!LocDiff.IsZero() || ClientMovementMode != PackNetworkMovementMode() || GetMovementBase() != ClientMovementBase || (CharacterOwner && CharacterOwner->GetBasedMovement().BoneName != ClientBaseBoneName))
			{
				// Just set the position. On subsequent moves we will resolve initially overlapping conditions.
				UpdatedComponent->SetWorldLocation(ClientLoc, false); //-V595

																	  // Trust the client's movement mode.
				ApplyNetworkMovementMode(ClientMovementMode);

				// Update base and floor at new location.
				SetBase(ClientMovementBase, ClientBaseBoneName);
				UpdateFloorFromAdjustment();

				// Even if base has not changed, we need to recompute the relative offsets (since we've moved).
				SaveBaseLocation();

				LastUpdateLocation = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
				LastUpdateRotation = UpdatedComponent ? UpdatedComponent->GetComponentQuat() : FQuat::Identity;
				LastUpdateVelocity = Velocity;
			}
		}

		// acknowledge receipt of this successful servermove()
		ServerData->PendingAdjustment.TimeStamp = ClientTimeStamp;
		ServerData->PendingAdjustment.bAckGoodMove = true;
	}

	//PerfCountersIncrement(PerfCounter_NumServerMoves);

	ServerData->bForceClientUpdate = false;
}