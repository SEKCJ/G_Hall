// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;
using System.Collections.Generic;

public class G_Hall_ProjectTarget : TargetRules
{
	public G_Hall_ProjectTarget(TargetInfo Target) : base(Target)
	{
        //bUseLoggingInShipping = true;
        Type = TargetType.Game;
        ExtraModuleNames.AddRange(new string[] { "G_Hall_Project" });
		//bUsePCHFiles = false;
		//bUseUnityBuild = false;
    }

	//
	// TargetRules interface.
	//

	/*public override void SetupBinaries(
		TargetInfo Target,
		ref List<UEBuildBinaryConfiguration> OutBuildBinaryConfigurations,
		ref List<string> OutExtraModuleNames
		)
	{
		OutExtraModuleNames.AddRange( new string[] { "G_Hall_Project" } );
	}*/
}
