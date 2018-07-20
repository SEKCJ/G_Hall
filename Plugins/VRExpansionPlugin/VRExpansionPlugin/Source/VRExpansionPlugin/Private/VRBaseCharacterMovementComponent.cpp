// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	Movement.cpp: Character movement implementation

=============================================================================*/

#include "VRBaseCharacterMovementComponent.h"
#include "VRBPDatatypes.h"
#include "VRBaseCharacter.h"
#include "VRRootComponent.h"
#include "VRPlayerController.h"
#include "GameFramework/PhysicsVolume.h"


UVRBaseCharacterMovementComponent::UVRBaseCharacterMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.TickGroup = TG_PrePhysics;


	//#TODO: Might not be ready to make this change globally yet....

	// Set Acceleration and braking deceleration walking to high values to avoid ramp up on speed
	// Realized that I wasn't doing this here for people to default to no acceleration.
	/*this->bRequestedMoveUseAcceleration = false;
	this->MaxAcceleration = 200048.0f;
	this->BrakingDecelerationWalking = 200048.0f;
	*/


	AdditionalVRInputVector = FVector::ZeroVector;	
	CustomVRInputVector = FVector::ZeroVector;
	bApplyAdditionalVRInputVectorAsNegative = true;
	VRClimbingStepHeight = 96.0f;
	VRClimbingEdgeRejectDistance = 5.0f;
	VRClimbingStepUpMultiplier = 1.0f;
	VRClimbingMaxReleaseVelocitySize = 800.0f;
	SetDefaultPostClimbMovementOnStepUp = true;
	DefaultPostClimbMovement = EVRConjoinedMovementModes::C_MOVE_Falling;

	bIgnoreSimulatingComponentsInFloorCheck = true;

	VRWallSlideScaler = 1.0f;
	VRLowGravWallFrictionScaler = 1.0f;
	VRLowGravIgnoresDefaultFluidFriction = true;

	VREdgeRejectDistance = 0.01f; // Rounded minimum of root movement

	VRReplicatedMovementMode = EVRConjoinedMovementModes::C_MOVE_MAX;

	NetworkSmoothingMode = ENetworkSmoothingMode::Disabled;//Exponential;

	bWasInPushBack = false;
	bIsInPushBack = false;

	bRunControlRotationInMovementComponent = true;
}

void UVRBaseCharacterMovementComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{

	if (MovementMode == MOVE_Custom && CustomMovementMode == (uint8)EVRCustomMovementMode::VRMOVE_Seated)
	{
		const FVector InputVector = ConsumeInputVector();
		if (!HasValidData() || ShouldSkipUpdate(DeltaTime))
		{
			return;
		}

		// Skip the perform movement logic, run the re-seat logic instead - running base movement component tick instead
		Super::Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

		// See if we fell out of the world.
		const bool bIsSimulatingPhysics = UpdatedComponent->IsSimulatingPhysics();
		if (CharacterOwner->Role == ROLE_Authority && (!bCheatFlying || bIsSimulatingPhysics) && !CharacterOwner->CheckStillInWorld())
		{
			return;
		}

		// If we are the owning client or the server then run the re-basing
		if (CharacterOwner->Role > ROLE_SimulatedProxy)
		{
			// Run offset logic here, the server will update simulated proxies with the movement replication
			if (AVRBaseCharacter * BaseChar = Cast<AVRBaseCharacter>(CharacterOwner))
			{
				BaseChar->TickSeatInformation(DeltaTime);
			}

		}

	}
	else
		Super::TickComponent(DeltaTime, TickType, ThisTickFunction);


	// This should be valid for both Simulated and owning clients as well as the server
	// Better here than in perform movement
	if (UVRRootComponent * VRRoot = Cast<UVRRootComponent>(CharacterOwner->GetCapsuleComponent()))
	{
		// If we didn't move the capsule, have it update itself here so the visual and physics representation is correct
		// We do this specifically to avoid double calling into the render / physics threads.
		if (!VRRoot->bCalledUpdateTransform)
			VRRoot->OnUpdateTransform_Public(EUpdateTransformFlags::None, ETeleportType::None);
	}
}

void UVRBaseCharacterMovementComponent::StartPushBackNotification(FHitResult HitResult)
{
	bIsInPushBack = true;

	if (bWasInPushBack)
		return;

	bWasInPushBack = true;

	if (AVRBaseCharacter * OwningCharacter = Cast<AVRBaseCharacter>(GetCharacterOwner()))
	{
		OwningCharacter->OnBeginWallPushback(HitResult, !Acceleration.Equals(FVector::ZeroVector), AdditionalVRInputVector);
	}
}

void UVRBaseCharacterMovementComponent::EndPushBackNotification()
{
	if (bIsInPushBack || !bWasInPushBack)
		return;

	bIsInPushBack = false;
	bWasInPushBack = false;

	if (AVRBaseCharacter * OwningCharacter = Cast<AVRBaseCharacter>(GetCharacterOwner()))
	{
		OwningCharacter->OnEndWallPushback();
	}
}

/*
bool UVRBaseCharacterMovementComponent::FloorSweepTest(
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
	TArray<FHitResult> OutHits;

	if (!bUseFlatBaseForFloorChecks)
	{
		if (bIgnoreSimulatingComponentsInFloorCheck)
		{
			// Testing all components in the way, skipping simulating components
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
		}
		else
			bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, TraceChannel, CollisionShape, Params, ResponseParam);
	}
	else
	{
		// Test with a box that is enclosed by the capsule.
		const float CapsuleRadius = CollisionShape.GetCapsuleRadius();
		const float CapsuleHeight = CollisionShape.GetCapsuleHalfHeight();
		const FCollisionShape BoxShape = FCollisionShape::MakeBox(FVector(CapsuleRadius * 0.707f, CapsuleRadius * 0.707f, CapsuleHeight));

		// First test with the box rotated so the corners are along the major axes (ie rotated 45 degrees).
		//TArray<FHitResult> OutHits;
		OutHits.Reset();

		if (bIgnoreSimulatingComponentsInFloorCheck)
		{
			// Testing all components in the way, skipping simulating components
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
		}
		else
			bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, Start, End, FQuat(FVector(0.f, 0.f, -1.f), PI * 0.25f), TraceChannel, BoxShape, Params, ResponseParam);

		if (!bBlockingHit)
		{
			// Test again with the same box, not rotated.
			OutHit.Reset(1.f, false);

			if (bIgnoreSimulatingComponentsInFloorCheck)
			{
				OutHits.Reset();
				// Testing all components in the way, skipping simulating components
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
			}
			else
				bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, TraceChannel, BoxShape, Params, ResponseParam);
		}
	}

	return bBlockingHit;
}*/

void UVRBaseCharacterMovementComponent::ComputeFloorDist(const FVector& CapsuleLocation, float LineDistance, float SweepDistance, FFindFloorResult& OutFloorResult, float SweepRadius, const FHitResult* DownwardSweepResult) const
{
	//UE_LOG(LogCharacterMovement, VeryVerbose, TEXT("[Role:%d] ComputeFloorDist: %s at location %s"), (int32)CharacterOwner->Role, *GetNameSafe(CharacterOwner), *CapsuleLocation.ToString());
	OutFloorResult.Clear();

	float PawnRadius, PawnHalfHeight;
	CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);

	bool bSkipSweep = false;
	if (DownwardSweepResult != NULL && DownwardSweepResult->IsValidBlockingHit())
	{
		// Only if the supplied sweep was vertical and downward.
		if ((DownwardSweepResult->TraceStart.Z > DownwardSweepResult->TraceEnd.Z) &&
			(DownwardSweepResult->TraceStart - DownwardSweepResult->TraceEnd).SizeSquared2D() <= KINDA_SMALL_NUMBER)
		{
			// Reject hits that are barely on the cusp of the radius of the capsule
			if (IsWithinEdgeTolerance(DownwardSweepResult->Location, DownwardSweepResult->ImpactPoint, PawnRadius))
			{
				// Don't try a redundant sweep, regardless of whether this sweep is usable.
				bSkipSweep = true;

				const bool bIsWalkable = IsWalkable(*DownwardSweepResult);
				const float FloorDist = (CapsuleLocation.Z - DownwardSweepResult->Location.Z);
				OutFloorResult.SetFromSweep(*DownwardSweepResult, FloorDist, bIsWalkable);

				if (bIsWalkable)
				{
					// Use the supplied downward sweep as the floor hit result.			
					return;
				}
			}
		}
	}

	// We require the sweep distance to be >= the line distance, otherwise the HitResult can't be interpreted as the sweep result.
	if (SweepDistance < LineDistance)
	{
		ensure(SweepDistance >= LineDistance);
		return;
	}

	bool bBlockingHit = false;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(ComputeFloorDist), false, CharacterOwner);
	FCollisionResponseParams ResponseParam;
	InitCollisionParams(QueryParams, ResponseParam);
	const ECollisionChannel CollisionChannel = UpdatedComponent->GetCollisionObjectType();

	// Skip physics bodies for floor check if we are skipping simulated objects
	if (bIgnoreSimulatingComponentsInFloorCheck)
		ResponseParam.CollisionResponse.PhysicsBody = ECollisionResponse::ECR_Ignore;

	// Sweep test
	if (!bSkipSweep && SweepDistance > 0.f && SweepRadius > 0.f)
	{
		// Use a shorter height to avoid sweeps giving weird results if we start on a surface.
		// This also allows us to adjust out of penetrations.
		const float ShrinkScale = 0.9f;
		const float ShrinkScaleOverlap = 0.1f;
		float ShrinkHeight = (PawnHalfHeight - PawnRadius) * (1.f - ShrinkScale);
		float TraceDist = SweepDistance + ShrinkHeight;
		FCollisionShape CapsuleShape = FCollisionShape::MakeCapsule(SweepRadius, PawnHalfHeight - ShrinkHeight);

		FHitResult Hit(1.f);
		bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation + FVector(0.f, 0.f, -TraceDist), CollisionChannel, CapsuleShape, QueryParams, ResponseParam);

		if (bBlockingHit)
		{
			// Reject hits adjacent to us, we only care about hits on the bottom portion of our capsule.
			// Check 2D distance to impact point, reject if within a tolerance from radius.
			if (Hit.bStartPenetrating || !IsWithinEdgeTolerance(CapsuleLocation, Hit.ImpactPoint, CapsuleShape.Capsule.Radius))
			{
				// Use a capsule with a slightly smaller radius and shorter height to avoid the adjacent object.
				// Capsule must not be nearly zero or the trace will fall back to a line trace from the start point and have the wrong length.
				CapsuleShape.Capsule.Radius = FMath::Max(0.f, CapsuleShape.Capsule.Radius - SWEEP_EDGE_REJECT_DISTANCE - KINDA_SMALL_NUMBER);
				if (!CapsuleShape.IsNearlyZero())
				{
					ShrinkHeight = (PawnHalfHeight - PawnRadius) * (1.f - ShrinkScaleOverlap);
					TraceDist = SweepDistance + ShrinkHeight;
					CapsuleShape.Capsule.HalfHeight = FMath::Max(PawnHalfHeight - ShrinkHeight, CapsuleShape.Capsule.Radius);
					Hit.Reset(1.f, false);

					bBlockingHit = FloorSweepTest(Hit, CapsuleLocation, CapsuleLocation + FVector(0.f, 0.f, -TraceDist), CollisionChannel, CapsuleShape, QueryParams, ResponseParam);
				}
			}

			// Reduce hit distance by ShrinkHeight because we shrank the capsule for the trace.
			// We allow negative distances here, because this allows us to pull out of penetrations.
			const float MaxPenetrationAdjust = FMath::Max(MAX_FLOOR_DIST, PawnRadius);
			const float SweepResult = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);

			OutFloorResult.SetFromSweep(Hit, SweepResult, false);
			if (Hit.IsValidBlockingHit() && IsWalkable(Hit))
			{
				if (SweepResult <= SweepDistance)
				{
					// Hit within test distance.
					OutFloorResult.bWalkableFloor = true;
					return;
				}
			}
		}
	}

	// Since we require a longer sweep than line trace, we don't want to run the line trace if the sweep missed everything.
	// We do however want to try a line trace if the sweep was stuck in penetration.
	if (!OutFloorResult.bBlockingHit && !OutFloorResult.HitResult.bStartPenetrating)
	{
		OutFloorResult.FloorDist = SweepDistance;
		return;
	}

	// Line trace
	if (LineDistance > 0.f)
	{
		const float ShrinkHeight = PawnHalfHeight;
		const FVector LineTraceStart = CapsuleLocation;
		const float TraceDist = LineDistance + ShrinkHeight;
		const FVector Down = FVector(0.f, 0.f, -TraceDist);
		QueryParams.TraceTag = SCENE_QUERY_STAT_NAME_ONLY(FloorLineTrace);

		FHitResult Hit(1.f);
		bBlockingHit = GetWorld()->LineTraceSingleByChannel(Hit, LineTraceStart, LineTraceStart + Down, CollisionChannel, QueryParams, ResponseParam);

		if (bBlockingHit)
		{
			if (Hit.Time > 0.f)
			{
				// Reduce hit distance by ShrinkHeight because we started the trace higher than the base.
				// We allow negative distances here, because this allows us to pull out of penetrations.
				const float MaxPenetrationAdjust = FMath::Max(MAX_FLOOR_DIST, PawnRadius);
				const float LineResult = FMath::Max(-MaxPenetrationAdjust, Hit.Time * TraceDist - ShrinkHeight);

				OutFloorResult.bBlockingHit = true;
				if (LineResult <= LineDistance && IsWalkable(Hit))
				{
					OutFloorResult.SetFromLineTrace(Hit, OutFloorResult.FloorDist, LineResult, true);
					return;
				}
			}
		}
	}

	// No hits were acceptable.
	OutFloorResult.bWalkableFloor = false;
	OutFloorResult.FloorDist = SweepDistance;
}


float UVRBaseCharacterMovementComponent::SlideAlongSurface(const FVector& Delta, float Time, const FVector& InNormal, FHitResult& Hit, bool bHandleImpact)
{
	// Am running the CharacterMovementComponents calculations manually here now prior to scaling down the delta

	if (!Hit.bBlockingHit)
	{
		return 0.f;
	}

	FVector Normal(InNormal);
	if (IsMovingOnGround())
	{
		// We don't want to be pushed up an unwalkable surface.
		if (Normal.Z > 0.f)
		{
			if (!IsWalkable(Hit))
			{
				Normal = Normal.GetSafeNormal2D();
			}
		}
		else if (Normal.Z < -KINDA_SMALL_NUMBER)
		{
			// Don't push down into the floor when the impact is on the upper portion of the capsule.
			if (CurrentFloor.FloorDist < MIN_FLOOR_DIST && CurrentFloor.bBlockingHit)
			{
				const FVector FloorNormal = CurrentFloor.HitResult.Normal;
				const bool bFloorOpposedToMovement = (Delta | FloorNormal) < 0.f && (FloorNormal.Z < 1.f - DELTA);
				if (bFloorOpposedToMovement)
				{
					Normal = FloorNormal;
				}

				Normal = Normal.GetSafeNormal2D();
			}
		}
	}


	/*if ((Delta | InNormal) <= -0.2)
	{

	}*/

	StartPushBackNotification(Hit);

	// If the movement mode is one where sliding is an issue in VR, scale the delta by the custom scaler now
	// that we have already validated the floor normal.
	// Otherwise just pass in as normal, either way skip the parents implementation as we are doing it now.
	if (IsMovingOnGround() || (MovementMode == MOVE_Custom && CustomMovementMode == (uint8)EVRCustomMovementMode::VRMOVE_Climbing))
		return Super::Super::SlideAlongSurface(Delta * VRWallSlideScaler, Time, Normal, Hit, bHandleImpact);
	else
		return Super::Super::SlideAlongSurface(Delta, Time, Normal, Hit, bHandleImpact);


}

void UVRBaseCharacterMovementComponent::SetCrouchedHalfHeight(float NewCrouchedHalfHeight)
{
	this->CrouchedHalfHeight = NewCrouchedHalfHeight;
}

void UVRBaseCharacterMovementComponent::AddCustomReplicatedMovement(FVector Movement)
{
	// if we are a client then lets round this to match what it will be after net Replication
	if (GetNetMode() == NM_Client)
		CustomVRInputVector += RoundDirectMovement(Movement);
	else
		CustomVRInputVector += Movement; // If not a client, don't bother to round this down.
}

void UVRBaseCharacterMovementComponent::PerformMoveAction_SnapTurn(float DeltaYawAngle)
{
	FVRMoveActionContainer MoveAction;
	MoveAction.MoveAction = EVRMoveAction::VRMOVEACTION_SnapTurn; 

	MoveAction.MoveActionRot = FRotator(0.0f, FMath::RoundToFloat(((FRotator(0.f,DeltaYawAngle, 0.f).Quaternion() * UpdatedComponent->GetComponentQuat()).Rotator().Yaw) * 100.f) / 100.f, 0.0f);
	MoveActionArray.MoveActions.Add(MoveAction);
}

void UVRBaseCharacterMovementComponent::PerformMoveAction_SetRotation(float NewYaw)
{
	FVRMoveActionContainer MoveAction;
	MoveAction.MoveAction = EVRMoveAction::VRMOVEACTION_SetRotation;
	MoveAction.MoveActionRot = FRotator(0.0f, FMath::RoundToFloat(NewYaw * 100.f) / 100.f, 0.0f);
	MoveActionArray.MoveActions.Add(MoveAction);
}

void UVRBaseCharacterMovementComponent::PerformMoveAction_Teleport(FVector TeleportLocation, FRotator TeleportRotation, bool bSkipEncroachmentCheck)
{
	FVRMoveActionContainer MoveAction;
	MoveAction.MoveAction = EVRMoveAction::VRMOVEACTION_Teleport;
	MoveAction.MoveActionLoc = RoundDirectMovement(TeleportLocation);
	MoveAction.MoveActionRot.Yaw = FMath::RoundToFloat(TeleportRotation.Yaw * 100.f) / 100.f;
	MoveAction.MoveActionRot.Pitch = bSkipEncroachmentCheck ? 1.0f : 0.0f;
	MoveActionArray.MoveActions.Add(MoveAction);
}

void UVRBaseCharacterMovementComponent::PerformMoveAction_StopAllMovement()
{
	FVRMoveActionContainer MoveAction;
	MoveAction.MoveAction = EVRMoveAction::VRMOVEACTION_StopAllMovement;
	MoveActionArray.MoveActions.Add(MoveAction);
}

void UVRBaseCharacterMovementComponent::PerformMoveAction_Custom(EVRMoveAction MoveActionToPerform, EVRMoveActionDataReq DataRequirementsForMoveAction, FVector MoveActionVector, FRotator MoveActionRotator)
{
	FVRMoveActionContainer MoveAction;
	MoveAction.MoveAction = MoveActionToPerform;
	MoveAction.MoveActionLoc = MoveActionVector;
	MoveAction.MoveActionRot = MoveActionRotator;
	MoveAction.MoveActionDataReq = DataRequirementsForMoveAction;
	MoveActionArray.MoveActions.Add(MoveAction);
}

bool UVRBaseCharacterMovementComponent::CheckForMoveAction()
{
	for (FVRMoveActionContainer& MoveAction : MoveActionArray.MoveActions)
	{
		switch (MoveAction.MoveAction)
		{
		case EVRMoveAction::VRMOVEACTION_SnapTurn:
		{
			/*return */DoMASnapTurn(MoveAction);
		}break;
		case EVRMoveAction::VRMOVEACTION_Teleport:
		{
			/*return */DoMATeleport(MoveAction);
		}break;
		case EVRMoveAction::VRMOVEACTION_StopAllMovement:
		{
			/*return */DoMAStopAllMovement(MoveAction);
		}break;
		case EVRMoveAction::VRMOVEACTION_SetRotation:
		{
			/*return */DoMASetRotation(MoveAction);
		}break;
		case EVRMoveAction::VRMOVEACTION_None:
		{}break;
		default: // All other move actions (CUSTOM)
		{
			if (AVRBaseCharacter * OwningCharacter = Cast<AVRBaseCharacter>(GetCharacterOwner()))
			{
				OwningCharacter->OnCustomMoveActionPerformed(MoveAction.MoveAction, MoveAction.MoveActionLoc, MoveAction.MoveActionRot);
			}
		}break;
		}
	}

	return true;
}

bool UVRBaseCharacterMovementComponent::DoMASnapTurn(FVRMoveActionContainer& MoveAction)
{
	if (AVRBaseCharacter * OwningCharacter = Cast<AVRBaseCharacter>(GetCharacterOwner()))
	{
		OwningCharacter->SetActorRotationVR(MoveAction.MoveActionRot, true, false);
	}

	return false;
}

bool UVRBaseCharacterMovementComponent::DoMASetRotation(FVRMoveActionContainer& MoveAction)
{
	if (AVRBaseCharacter * OwningCharacter = Cast<AVRBaseCharacter>(GetCharacterOwner()))
	{
		OwningCharacter->SetActorRotationVR(MoveAction.MoveActionRot, true);
	}

	return false;
}

bool UVRBaseCharacterMovementComponent::DoMATeleport(FVRMoveActionContainer& MoveAction)
{
	if (AVRBaseCharacter * OwningCharacter = Cast<AVRBaseCharacter>(GetCharacterOwner()))
	{
		AController* OwningController = OwningCharacter->GetController();

		if (!OwningController)
		{
			MoveAction.MoveAction = EVRMoveAction::VRMOVEACTION_None;
			return false;
		}

		OwningCharacter->TeleportTo(MoveAction.MoveActionLoc, MoveAction.MoveActionRot, false, MoveAction.MoveActionRot.Pitch > 0.0f);

		if (OwningCharacter->bUseControllerRotationYaw)
			OwningController->SetControlRotation(MoveAction.MoveActionRot);

		return true;
	}

	return false;
}

bool UVRBaseCharacterMovementComponent::DoMAStopAllMovement(FVRMoveActionContainer& MoveAction)
{
	if (AVRBaseCharacter * OwningCharacter = Cast<AVRBaseCharacter>(GetCharacterOwner()))
	{
		this->StopMovementImmediately();
		return true;
	}

	return false;
}

void UVRBaseCharacterMovementComponent::PhysCustom(float deltaTime, int32 Iterations)
{
	switch (static_cast<EVRCustomMovementMode>(CustomMovementMode))
	{
	case EVRCustomMovementMode::VRMOVE_Climbing:
		PhysCustom_Climbing(deltaTime, Iterations);
		break;
	case EVRCustomMovementMode::VRMOVE_LowGrav:
		PhysCustom_LowGrav(deltaTime, Iterations);
		break;
	case EVRCustomMovementMode::VRMOVE_Seated:
		break;
	default:
		Super::PhysCustom(deltaTime, Iterations);
		break;
	}
}

bool UVRBaseCharacterMovementComponent::VRClimbStepUp(const FVector& GravDir, const FVector& Delta, const FHitResult &InHit, FStepDownResult* OutStepDownResult)
{
	return StepUp(GravDir, Delta, InHit, OutStepDownResult);
}

void UVRBaseCharacterMovementComponent::PhysCustom_Climbing(float deltaTime, int32 Iterations)
{
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	// I am forcing this to 0 to avoid some legacy velocity coming out of other movement modes, climbing should only be direct movement anyway.
	Velocity = FVector::ZeroVector;

	if (bApplyAdditionalVRInputVectorAsNegative)
	{
		// Rewind the players position by the new capsule location
		RewindVRRelativeMovement();
	}

	Iterations++;
	bJustTeleported = false;

	FVector OldLocation = UpdatedComponent->GetComponentLocation();
	const FVector Adjusted = /*(Velocity * deltaTime) + */CustomVRInputVector;
	FVector Delta = Adjusted + AdditionalVRInputVector;
	bool bZeroDelta = Delta.IsNearlyZero();

	FStepDownResult StepDownResult;

	// Instead of remaking the step up function, temp assign a custom step height and then fall back to the old one afterward
	// This isn't the "proper" way to do it, but it saves on re-making stepup() for both vr characters seperatly (due to different hmd injection)
	float OldMaxStepHeight = MaxStepHeight;
	MaxStepHeight = VRClimbingStepHeight;
	bool bSteppedUp = false;

	if (!bZeroDelta)
	{
		FHitResult Hit(1.f);
		SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);

		if (Hit.Time < 1.f)
		{
			const FVector GravDir = FVector(0.f, 0.f, -1.f);
			const FVector VelDir = (CustomVRInputVector).GetSafeNormal();//Velocity.GetSafeNormal();
			const float UpDown = GravDir | VelDir;

			//bool bSteppedUp = false;
			if ((FMath::Abs(Hit.ImpactNormal.Z) < 0.2f) && (UpDown < 0.5f) && (UpDown > -0.2f) && CanStepUp(Hit))
			{
				float stepZ = UpdatedComponent->GetComponentLocation().Z;

				// Making it easier to step up here with the multiplier, helps avoid falling back off
				bSteppedUp = VRClimbStepUp(GravDir, ((Adjusted * VRClimbingStepUpMultiplier) + AdditionalVRInputVector) * (1.f - Hit.Time), Hit, &StepDownResult);

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
	}

	// Revert to old max step height
	MaxStepHeight = OldMaxStepHeight;

	if (bSteppedUp)
	{
		if (AVRBaseCharacter * ownerCharacter = Cast<AVRBaseCharacter>(CharacterOwner))
		{
			if (SetDefaultPostClimbMovementOnStepUp)
			{
				// Takes effect next frame, this allows server rollback to correctly handle auto step up
				SetReplicatedMovementMode(DefaultPostClimbMovement);
				// Before doing this the server could rollback the client from a bad step up and leave it hanging in climb mode
				// This way the rollback replay correctly sets the movement mode from the step up request

				Velocity = FVector::ZeroVector;
			}

			// Notify the end user that they probably want to stop gripping now
			ownerCharacter->OnClimbingSteppedUp();
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

	if (CurrentFloor.IsWalkableFloor())
	{
		if(CurrentFloor.GetDistanceToFloor() < (MIN_FLOOR_DIST + MAX_FLOOR_DIST) / 2)
			AdjustFloorHeight();

		// This was causing based movement to apply to climbing
		//SetBase(CurrentFloor.HitResult.Component.Get(), CurrentFloor.HitResult.BoneName);
	}
	else if (CurrentFloor.HitResult.bStartPenetrating)
	{
		// The floor check failed because it started in penetration
		// We do not want to try to move downward because the downward sweep failed, rather we'd like to try to pop out of the floor.
		FHitResult Hit(CurrentFloor.HitResult);
		Hit.TraceEnd = Hit.TraceStart + FVector(0.f, 0.f, MAX_FLOOR_DIST);
		const FVector RequestedAdjustment = GetPenetrationAdjustment(Hit);
		ResolvePenetration(RequestedAdjustment, Hit, UpdatedComponent->GetComponentQuat());
		bForceNextFloorCheck = true;
	}

	if(!bSteppedUp || !SetDefaultPostClimbMovementOnStepUp)
	{
		if (!bJustTeleported && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
		{
			Velocity = (((UpdatedComponent->GetComponentLocation() - OldLocation) - AdditionalVRInputVector) / deltaTime).GetClampedToMaxSize(VRClimbingMaxReleaseVelocitySize);
		}
	}
}

void UVRBaseCharacterMovementComponent::PhysCustom_LowGrav(float deltaTime, int32 Iterations)
{

	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	float Friction = 0.0f; 

	// If we are not in the default physics volume then accept the custom fluid friction setting
	// I set this by default to be ignored as many will not alter the default fluid friction
	if(!VRLowGravIgnoresDefaultFluidFriction || GetWorld()->GetDefaultPhysicsVolume() != GetPhysicsVolume())
		Friction = 0.5f * GetPhysicsVolume()->FluidFriction;

	CalcVelocity(deltaTime, Friction, true, 0.0f);

	if (bApplyAdditionalVRInputVectorAsNegative)
	{
		// Rewind the players position by the new capsule location
		RewindVRRelativeMovement();
	}

	// Adding in custom VR input vector here, can be used for custom movement during it
	// AddImpulse is not multiplayer compatible client side
	Velocity += CustomVRInputVector; 

	Iterations++;
	bJustTeleported = false;

	FVector OldLocation = UpdatedComponent->GetComponentLocation();
	const FVector Adjusted = (Velocity * deltaTime);
	FHitResult Hit(1.f);
	SafeMoveUpdatedComponent(Adjusted + AdditionalVRInputVector, UpdatedComponent->GetComponentQuat(), true, Hit);

	if (Hit.Time < 1.f)
	{
		// Still running step up with grav dir
		const FVector GravDir = FVector(0.f, 0.f, -1.f);
		const FVector VelDir = Velocity.GetSafeNormal();
		const float UpDown = GravDir | VelDir;

		bool bSteppedUp = false;
		if ((FMath::Abs(Hit.ImpactNormal.Z) < 0.2f) && (UpDown < 0.5f) && (UpDown > -0.2f) && CanStepUp(Hit))
		{
			float stepZ = UpdatedComponent->GetComponentLocation().Z;
			bSteppedUp = StepUp(GravDir, Adjusted * (1.f - Hit.Time), Hit);
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

		if (!bJustTeleported && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
		{
			Velocity = (((UpdatedComponent->GetComponentLocation() - OldLocation) - AdditionalVRInputVector) / deltaTime) * VRLowGravWallFrictionScaler;
		}
	}
	else
	{
		if (!bJustTeleported && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity())
		{
			Velocity = (((UpdatedComponent->GetComponentLocation() - OldLocation) - AdditionalVRInputVector) / deltaTime);
		}
	}
}


void UVRBaseCharacterMovementComponent::SetClimbingMode(bool bIsClimbing)
{
	if (bIsClimbing)
		VRReplicatedMovementMode = EVRConjoinedMovementModes::C_VRMOVE_Climbing;
	else
		VRReplicatedMovementMode = DefaultPostClimbMovement;
}

void UVRBaseCharacterMovementComponent::SetReplicatedMovementMode(EVRConjoinedMovementModes NewMovementMode)
{
	// Only have up to 15 that it can go up to, the previous 7 index's are used up for std movement modes
	VRReplicatedMovementMode = NewMovementMode;
}

/*void UVRBaseCharacterMovementComponent::SendClientAdjustment()
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

	if (ServerData->PendingAdjustment.bAckGoodMove == true)
	{
		// just notify client this move was received
		ClientAckGoodMove(ServerData->PendingAdjustment.TimeStamp);
	}
	else
	{
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
			if (AVRBaseCharacter * VRC = Cast<AVRBaseCharacter>(GetOwner()))
			{
				FVector CusVec = VRC->GetVRLocation();
				GEngine->AddOnScreenDebugMessage(-1, 125.f, IsLocallyControlled() ? FColor::Red : FColor::Green, FString::Printf(TEXT("VrLoc: x: %f, y: %f, X: %f"), CusVec.X, CusVec.Y, CusVec.Z));
			}
			GEngine->AddOnScreenDebugMessage(-1, 125.f, FColor::Red, TEXT("Correcting Client Location!"));
			ClientVeryShortAdjustPosition
			(
				ServerData->PendingAdjustment.TimeStamp,
				ServerData->PendingAdjustment.NewLoc,
				ServerData->PendingAdjustment.NewBase,
				ServerData->PendingAdjustment.NewBaseBoneName,
				ServerData->PendingAdjustment.NewBase != NULL,
				ServerData->PendingAdjustment.bBaseRelativePosition,
				PackNetworkMovementMode()
			);
		}
		else
		{
			if (AVRBaseCharacter * VRC = Cast<AVRBaseCharacter>(GetOwner()))
			{
				FVector CusVec = VRC->GetVRLocation();
				GEngine->AddOnScreenDebugMessage(-1, 125.f, IsLocallyControlled() ? FColor::Red : FColor::Green, FString::Printf(TEXT("VrLoc: x: %f, y: %f, X: %f"), CusVec.X, CusVec.Y, CusVec.Z));
			}
			GEngine->AddOnScreenDebugMessage(-1, 125.f, FColor::Red, TEXT("Correcting Client Location!"));
			ClientAdjustPosition
			(
				ServerData->PendingAdjustment.TimeStamp,
				ServerData->PendingAdjustment.NewLoc,
				ServerData->PendingAdjustment.NewVel,
				ServerData->PendingAdjustment.NewBase,
				ServerData->PendingAdjustment.NewBaseBoneName,
				ServerData->PendingAdjustment.NewBase != NULL,
				ServerData->PendingAdjustment.bBaseRelativePosition,
				PackNetworkMovementMode()
			);
		}
	}

	ServerData->PendingAdjustment.TimeStamp = 0;
	ServerData->PendingAdjustment.bAckGoodMove = false;
	ServerData->bForceClientUpdate = false;
}
*/
void UVRBaseCharacterMovementComponent::PerformMovement(float DeltaSeconds)
{
	// Scope these, they nest with Outer references so it should work fine
	FVRCharacterScopedMovementUpdate ScopedMovementUpdate(UpdatedComponent, bEnableScopedMovementUpdates ? EScopedUpdate::DeferredUpdates : EScopedUpdate::ImmediateUpdates);

	// This moves it into update scope
	if (bRunControlRotationInMovementComponent && IsLocallyControlled())
	{
		if (AVRPlayerController * PC = Cast<AVRPlayerController>(CharacterOwner->GetController()))
		{
			PC->RotationInput = PC->LastRotationInput;
			PC->UpdateRotation(DeltaSeconds);
			PC->LastRotationInput = FRotator::ZeroRotator;
			PC->RotationInput = FRotator::ZeroRotator;
		}
	}

	if (VRReplicatedMovementMode != EVRConjoinedMovementModes::C_MOVE_MAX)//None)
	{
		if (VRReplicatedMovementMode <= EVRConjoinedMovementModes::C_MOVE_MAX)
		{
			// Is a default movement mode, just directly set it
			SetMovementMode((EMovementMode)VRReplicatedMovementMode);
		}
		else // Is Custom
		{
			// Auto calculates the difference for our VR movements, index is from 0 so using climbing should get me correct index's as it is the first custom mode
			SetMovementMode(EMovementMode::MOVE_Custom, (((int8)VRReplicatedMovementMode - (uint8)EVRConjoinedMovementModes::C_VRMOVE_Climbing)) );
		}

		// Clearing it here instead now, as this way the code can inject it during PerformMovement
		// Specifically used by the Climbing Step up, so that server rollbacks are supported
		VRReplicatedMovementMode = EVRConjoinedMovementModes::C_MOVE_MAX;//None;
	}

	// Handle move actions here - Should be scoped
	CheckForMoveAction();

	// Clear out this flag prior to movement so we can see if it gets changed
	bIsInPushBack = false;

	Super::PerformMovement(DeltaSeconds);

	EndPushBackNotification(); // Check if we need to notify of ending pushback

	// Make sure these are cleaned out for the next frame
	AdditionalVRInputVector = FVector::ZeroVector;
	CustomVRInputVector = FVector::ZeroVector;

	// Only clear it here if we are the server, the client clears it later
	if (CharacterOwner->Role == ROLE_Authority)
	{
		MoveActionArray.Clear();
	}
}

void FSavedMove_VRBaseCharacter::SetInitialPosition(ACharacter* C)
{
	// See if we can get the VR capsule location
	if (AVRBaseCharacter * VRC = Cast<AVRBaseCharacter>(C))
	{
		if (UVRBaseCharacterMovementComponent * moveComp = Cast<UVRBaseCharacterMovementComponent>(VRC->GetMovementComponent()))
		{

			// Saving this out early because it will be wiped before the PostUpdate gets the values
			//ConditionalValues.MoveAction.MoveAction = moveComp->MoveAction.MoveAction;

			VRReplicatedMovementMode = moveComp->VRReplicatedMovementMode;

			ConditionalValues.CustomVRInputVector = moveComp->CustomVRInputVector;

			if (moveComp->HasRequestedVelocity())
				ConditionalValues.RequestedVelocity = moveComp->RequestedVelocity;
			else
				ConditionalValues.RequestedVelocity = FVector::ZeroVector;
				
			// Throw out the Z value of the headset, its not used anyway for movement
			// Instead, re-purpose it to be the capsule half height
			if (AVRBaseCharacter * BaseChar = Cast<AVRBaseCharacter>(moveComp->GetCharacterOwner()))
			{
				if (BaseChar->VRReplicateCapsuleHeight && C)
					LFDiff.Z = C->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
				else
					LFDiff.Z = 0.0f;
			}
			else
				LFDiff.Z = 0.0f;
		}
		else
		{
			VRReplicatedMovementMode = EVRConjoinedMovementModes::C_MOVE_MAX;//None;
			ConditionalValues.CustomVRInputVector = FVector::ZeroVector;
			ConditionalValues.RequestedVelocity = FVector::ZeroVector;
		}
	}
	else
	{
		VRReplicatedMovementMode = EVRConjoinedMovementModes::C_MOVE_MAX;//None;
		ConditionalValues.CustomVRInputVector = FVector::ZeroVector;
	}

	FSavedMove_Character::SetInitialPosition(C);
}

void FSavedMove_VRBaseCharacter::CombineWith(const FSavedMove_Character* OldMove, ACharacter* InCharacter, APlayerController* PC, const FVector& OldStartLocation)
{
	UCharacterMovementComponent* CharMovement = InCharacter->GetCharacterMovement();
	
	// to combine move, first revert pawn position to PendingMove start position, before playing combined move on client
	CharMovement->UpdatedComponent->SetWorldLocationAndRotation(OldStartLocation, OldMove->StartRotation, false, nullptr, CharMovement->GetTeleportType());
	CharMovement->Velocity = OldMove->StartVelocity;

	CharMovement->SetBase(OldMove->StartBase.Get(), OldMove->StartBoneName);
	CharMovement->CurrentFloor = OldMove->StartFloor;

	// Now that we have reverted to the old position, prepare a new move from that position,
	// using our current velocity, acceleration, and rotation, but applied over the combined time from the old and new move.

	// Combine times for both moves
	DeltaTime += OldMove->DeltaTime;

	//FSavedMove_VRBaseCharacter * BaseSavedMove = (FSavedMove_VRBaseCharacter *)NewMove.Get();
	FSavedMove_VRBaseCharacter * BaseSavedMovePending = (FSavedMove_VRBaseCharacter *)OldMove;

	if (/*BaseSavedMove && */BaseSavedMovePending)
	{
		LFDiff.X += BaseSavedMovePending->LFDiff.X;
		LFDiff.Y += BaseSavedMovePending->LFDiff.Y;
	}

	// Roll back jump force counters. SetInitialPosition() below will copy them to the saved move.
	// Changes in certain counters like JumpCurrentCount don't allow move combining, so no need to roll those back (they are the same).
	InCharacter->JumpForceTimeRemaining = OldMove->JumpForceTimeRemaining;
	InCharacter->JumpKeyHoldTime = OldMove->JumpKeyHoldTime;
}

void FSavedMove_VRBaseCharacter::PostUpdate(ACharacter* C, EPostUpdateMode PostUpdateMode)
{
	FSavedMove_Character::PostUpdate(C, PostUpdateMode);

	// See if we can get the VR capsule location
	if (AVRBaseCharacter * VRC = Cast<AVRBaseCharacter>(C))
	{
		if (UVRBaseCharacterMovementComponent * moveComp = Cast<UVRBaseCharacterMovementComponent>(VRC->GetMovementComponent()))
		{
			ConditionalValues.MoveActionArray = moveComp->MoveActionArray;
			moveComp->MoveActionArray.Clear();
		}
	}
	/*if (ConditionalValues.MoveAction.MoveAction != EVRMoveAction::VRMOVEACTION_None)
	{
		// See if we can get the VR capsule location
		if (AVRBaseCharacter * VRC = Cast<AVRBaseCharacter>(C))
		{
			if (UVRBaseCharacterMovementComponent * moveComp = Cast<UVRBaseCharacterMovementComponent>(VRC->GetMovementComponent()))
			{
				// This is cleared out in perform movement so I need to save it before applying below
				EVRMoveAction tempAction = ConditionalValues.MoveAction.MoveAction;
				ConditionalValues.MoveAction = moveComp->MoveAction;
				ConditionalValues.MoveAction.MoveAction = tempAction;
			}
			else
			{
				ConditionalValues.MoveAction.Clear();
			}
		}
		else
		{
			ConditionalValues.MoveAction.Clear();
		}
	}*/
}

void FSavedMove_VRBaseCharacter::Clear()
{
	VRReplicatedMovementMode = EVRConjoinedMovementModes::C_MOVE_MAX;// None;

	VRCapsuleLocation = FVector::ZeroVector;
	VRCapsuleRotation = FRotator::ZeroRotator;
	LFDiff = FVector::ZeroVector;

	ConditionalValues.CustomVRInputVector = FVector::ZeroVector;
	ConditionalValues.RequestedVelocity = FVector::ZeroVector;
	ConditionalValues.MoveActionArray.Clear();
	//ConditionalValues.MoveAction.Clear();

	FSavedMove_Character::Clear();
}

void FSavedMove_VRBaseCharacter::PrepMoveFor(ACharacter* Character)
{
	UVRBaseCharacterMovementComponent * BaseCharMove = Cast<UVRBaseCharacterMovementComponent>(Character->GetCharacterMovement());

	if (BaseCharMove)
	{
		BaseCharMove->MoveActionArray = ConditionalValues.MoveActionArray;
		//BaseCharMove->MoveAction = ConditionalValues.MoveAction; 
		BaseCharMove->CustomVRInputVector = ConditionalValues.CustomVRInputVector;//this->CustomVRInputVector;
		BaseCharMove->VRReplicatedMovementMode = this->VRReplicatedMovementMode;
	}
	
	if (!ConditionalValues.RequestedVelocity.IsZero())
	{
		BaseCharMove->RequestedVelocity = ConditionalValues.RequestedVelocity;
		BaseCharMove->SetHasRequestedVelocity(true);
	}
	else
	{
		BaseCharMove->SetHasRequestedVelocity(false);
	}

	FSavedMove_Character::PrepMoveFor(Character);
}

void UVRBaseCharacterMovementComponent::SmoothCorrection(const FVector& OldLocation, const FQuat& OldRotation, const FVector& NewLocation, const FQuat& NewRotation)
{
	//SCOPE_CYCLE_COUNTER(STAT_CharacterMovementSmoothCorrection);
	if (!HasValidData())
	{
		return;
	}

	AVRBaseCharacter * Basechar = Cast<AVRBaseCharacter>(CharacterOwner);

	if (!Basechar)
		Super::SmoothCorrection(OldLocation, OldRotation, NewLocation, NewRotation);

	// We shouldn't be running this on a server that is not a listen server.
	checkSlow(GetNetMode() != NM_DedicatedServer);
	checkSlow(GetNetMode() != NM_Standalone);

	// Only client proxies or remote clients on a listen server should run this code.
	const bool bIsSimulatedProxy = (CharacterOwner->Role == ROLE_SimulatedProxy);
	const bool bIsRemoteAutoProxy = (CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy);
	ensure(bIsSimulatedProxy || bIsRemoteAutoProxy);

	// Skip smoothing in set situations
	if (
		NetworkSmoothingMode != ENetworkSmoothingMode::Disabled &&
		NetworkSmoothingMode != ENetworkSmoothingMode::Replay &&
		(!OldRotation.Equals(NewRotation, 1e-5f)/* || Velocity.IsNearlyZero()*/)
		)
	{
		if (Basechar)
		{
			Basechar->NetSmoother->SetRelativeLocation(FVector::ZeroVector);
		}
		FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
		if (ClientData)
		{
			ClientData->MeshTranslationOffset = FVector::ZeroVector;
			ClientData->MeshRotationOffset = FQuat::Identity;
		}
		//UpdatedComponent->SetWorldRotation(NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
		UpdatedComponent->SetWorldLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
		bNetworkSmoothingComplete = true;
		return;
	}

	// Getting a correction means new data, so smoothing needs to run.
	bNetworkSmoothingComplete = false;

	// Handle selected smoothing mode.
	if (NetworkSmoothingMode == ENetworkSmoothingMode::Replay)
	{
		// Replays use pure interpolation in this mode, all of the work is done in SmoothClientPosition_Interpolate
		return;
	}
	else if (NetworkSmoothingMode == ENetworkSmoothingMode::Disabled)
	{
		UpdatedComponent->SetWorldLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
		bNetworkSmoothingComplete = true;
	}
	else if (FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character())
	{
		const UWorld* MyWorld = GetWorld();
		if (!ensure(MyWorld != nullptr))
		{
			return;
		}

		// The mesh doesn't move, but the capsule does so we have a new offset.
		FVector NewToOldVector = (OldLocation - NewLocation);
		if (bIsNavWalkingOnServer && FMath::Abs(NewToOldVector.Z) < NavWalkingFloorDistTolerance)
		{
			// ignore smoothing on Z axis
			// don't modify new location (local simulation result), since it's probably more accurate than server data
			// and shouldn't matter as long as difference is relatively small
			NewToOldVector.Z = 0;
		}

		const float DistSq = NewToOldVector.SizeSquared();
		if (DistSq > FMath::Square(ClientData->MaxSmoothNetUpdateDist))
		{
			ClientData->MeshTranslationOffset = (DistSq > FMath::Square(ClientData->NoSmoothNetUpdateDist))
				? FVector::ZeroVector
				: ClientData->MeshTranslationOffset + ClientData->MaxSmoothNetUpdateDist * NewToOldVector.GetSafeNormal();
		}
		else
		{
			ClientData->MeshTranslationOffset = ClientData->MeshTranslationOffset + NewToOldVector;
		}

		//UE_LOG(LogCharacterNetSmoothing, Verbose, TEXT("Proxy %s SmoothCorrection(%.2f)"), *GetNameSafe(CharacterOwner), FMath::Sqrt(DistSq));
		if (NetworkSmoothingMode == ENetworkSmoothingMode::Linear)
		{
			ClientData->OriginalMeshTranslationOffset = ClientData->MeshTranslationOffset;

			// Remember the current and target rotation, we're going to lerp between them
			ClientData->OriginalMeshRotationOffset = OldRotation;
			ClientData->MeshRotationOffset = OldRotation;
			ClientData->MeshRotationTarget = NewRotation;

			// Move the capsule, but not the mesh.
			// Note: we don't change rotation, we lerp towards it in SmoothClientPosition.
			const FScopedPreventAttachedComponentMove PreventMeshMove(Basechar->NetSmoother);
			UpdatedComponent->SetWorldLocation(NewLocation, false, nullptr, GetTeleportType());
		}
		else
		{
			// Calc rotation needed to keep current world rotation after UpdatedComponent moves.
			// Take difference between where we were rotated before, and where we're going
			ClientData->MeshRotationOffset = (NewRotation.Inverse() * OldRotation) * ClientData->MeshRotationOffset;
			ClientData->MeshRotationTarget = FQuat::Identity;

			const FScopedPreventAttachedComponentMove PreventMeshMove(Basechar->NetSmoother);
			UpdatedComponent->SetWorldLocationAndRotation(NewLocation, NewRotation, false, nullptr, GetTeleportType());
		}

		//////////////////////////////////////////////////////////////////////////
		// Update smoothing timestamps

		// If running ahead, pull back slightly. This will cause the next delta to seem slightly longer, and cause us to lerp to it slightly slower.
		if (ClientData->SmoothingClientTimeStamp > ClientData->SmoothingServerTimeStamp)
		{
			const double OldClientTimeStamp = ClientData->SmoothingClientTimeStamp;
			ClientData->SmoothingClientTimeStamp = FMath::LerpStable(ClientData->SmoothingServerTimeStamp, OldClientTimeStamp, 0.5);

			//UE_LOG(LogCharacterNetSmoothing, VeryVerbose, TEXT("SmoothCorrection: Pull back client from ClientTimeStamp: %.6f to %.6f, ServerTimeStamp: %.6f for %s"),
			//	OldClientTimeStamp, ClientData->SmoothingClientTimeStamp, ClientData->SmoothingServerTimeStamp, *GetNameSafe(CharacterOwner));
		}

		// Using server timestamp lets us know how much time actually elapsed, regardless of packet lag variance.
		double OldServerTimeStamp = ClientData->SmoothingServerTimeStamp;
		ClientData->SmoothingServerTimeStamp = (bIsSimulatedProxy ? CharacterOwner->GetReplicatedServerLastTransformUpdateTimeStamp() : ServerLastTransformUpdateTimeStamp);

		// Initial update has no delta.
		if (ClientData->LastCorrectionTime == 0)
		{
			ClientData->SmoothingClientTimeStamp = ClientData->SmoothingServerTimeStamp;
			OldServerTimeStamp = ClientData->SmoothingServerTimeStamp;
		}

		// Don't let the client fall too far behind or run ahead of new server time.
		const double ServerDeltaTime = ClientData->SmoothingServerTimeStamp - OldServerTimeStamp;
		const double MaxDelta = FMath::Clamp(ServerDeltaTime * 1.25, 0.0, ClientData->MaxMoveDeltaTime * 2.0);
		ClientData->SmoothingClientTimeStamp = FMath::Clamp(ClientData->SmoothingClientTimeStamp, ClientData->SmoothingServerTimeStamp - MaxDelta, ClientData->SmoothingServerTimeStamp);

		// Compute actual delta between new server timestamp and client simulation.
		ClientData->LastCorrectionDelta = ClientData->SmoothingServerTimeStamp - ClientData->SmoothingClientTimeStamp;
		ClientData->LastCorrectionTime = MyWorld->GetTimeSeconds();

		//UE_LOG(LogCharacterNetSmoothing, VeryVerbose, TEXT("SmoothCorrection: WorldTime: %.6f, ServerTimeStamp: %.6f, ClientTimeStamp: %.6f, Delta: %.6f for %s"),
		//MyWorld->GetTimeSeconds(), ClientData->SmoothingServerTimeStamp, ClientData->SmoothingClientTimeStamp, ClientData->LastCorrectionDelta, *GetNameSafe(CharacterOwner));
		/*
		Visualize network smoothing was here, removed it
		*/
	}
}

void UVRBaseCharacterMovementComponent::SmoothClientPosition(float DeltaSeconds)
{
	if (!HasValidData() || NetworkSmoothingMode == ENetworkSmoothingMode::Disabled)
	{
		return;
	}

	// We shouldn't be running this on a server that is not a listen server.
	checkSlow(GetNetMode() != NM_DedicatedServer);
	checkSlow(GetNetMode() != NM_Standalone);

	// Only client proxies or remote clients on a listen server should run this code.
	const bool bIsSimulatedProxy = (CharacterOwner->Role == ROLE_SimulatedProxy);
	const bool bIsRemoteAutoProxy = (CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy);
	if (!ensure(bIsSimulatedProxy || bIsRemoteAutoProxy))
	{
		return;
	}

	SmoothClientPosition_Interpolate(DeltaSeconds);

	//SmoothClientPosition_UpdateVisuals(); No mesh, don't bother to run this
	SmoothClientPosition_UpdateVRVisuals();
}

void UVRBaseCharacterMovementComponent::SmoothClientPosition_UpdateVRVisuals()
{
	//SCOPE_CYCLE_COUNTER(STAT_CharacterMovementSmoothClientPosition_Visual);
	FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();

	AVRBaseCharacter * Basechar = Cast<AVRBaseCharacter>(CharacterOwner);

	if (!Basechar || !ClientData)
		return;

	if (ClientData)
	{
		if (NetworkSmoothingMode == ENetworkSmoothingMode::Linear)
		{
			// Erased most of the code here, check back in later
			const FVector NewRelLocation = ClientData->MeshRotationOffset.UnrotateVector(ClientData->MeshTranslationOffset) + CharacterOwner->GetBaseTranslationOffset();
			Basechar->NetSmoother->SetRelativeLocation(NewRelLocation);
		}
		else if (NetworkSmoothingMode == ENetworkSmoothingMode::Exponential)
		{
			// Adjust mesh location and rotation
			const FVector NewRelTranslation = UpdatedComponent->GetComponentToWorld().InverseTransformVectorNoScale(ClientData->MeshTranslationOffset) + CharacterOwner->GetBaseTranslationOffset();
			const FQuat NewRelRotation = ClientData->MeshRotationOffset * CharacterOwner->GetBaseRotationOffset();
			//Basechar->NetSmoother->SetRelativeLocation(NewRelTranslation);

			Basechar->NetSmoother->SetRelativeLocationAndRotation(NewRelTranslation, NewRelRotation);
		}
		else if (NetworkSmoothingMode == ENetworkSmoothingMode::Replay)
		{
			if (!UpdatedComponent->GetComponentQuat().Equals(ClientData->MeshRotationOffset, SCENECOMPONENT_QUAT_TOLERANCE) || !UpdatedComponent->GetComponentLocation().Equals(ClientData->MeshTranslationOffset, KINDA_SMALL_NUMBER))
			{
				//UpdatedComponent->SetWorldLocation(ClientData->MeshTranslationOffset);
				UpdatedComponent->SetWorldLocationAndRotation(ClientData->MeshTranslationOffset, ClientData->MeshRotationOffset);
			}
		}
		else
		{
			// Unhandled mode
		}
	}
}

FVRCharacterScopedMovementUpdate::FVRCharacterScopedMovementUpdate(USceneComponent* Component, EScopedUpdate::Type ScopeBehavior, bool bRequireOverlapsEventFlagToQueueOverlaps)
	: FScopedMovementUpdate(Component, ScopeBehavior, bRequireOverlapsEventFlagToQueueOverlaps)
{
	UVRRootComponent* RootComponent = Cast<UVRRootComponent>(Owner);
	if (RootComponent)
	{
		InitialVRTransform = RootComponent->OffsetComponentToWorld;
	}
}

void FVRCharacterScopedMovementUpdate::RevertMove()
{
	bool bTransformIsDirty = IsTransformDirty();

	FScopedMovementUpdate::RevertMove();

	UVRRootComponent* RootComponent = Cast<UVRRootComponent>(Owner);
	if (RootComponent)
	{
		// If the base class was going to miss bad overlaps, ie: the offsetcomponent to world is different but the transform isn't
		if (!bTransformIsDirty && !IsDeferringUpdates() && !InitialVRTransform.Equals(RootComponent->OffsetComponentToWorld))
		{
			RootComponent->UpdateOverlaps();
		}

		// Fix offset
		RootComponent->GenerateOffsetToWorld();
	}
}
