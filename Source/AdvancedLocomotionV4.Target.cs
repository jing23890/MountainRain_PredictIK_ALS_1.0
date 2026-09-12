

using UnrealBuildTool;
using System.Collections.Generic;

public class AdvancedLocomotionV4Target : TargetRules
{
	public AdvancedLocomotionV4Target(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V6;

		ExtraModuleNames.AddRange( new string[] { "AdvancedLocomotionV4" } );
	}
}
