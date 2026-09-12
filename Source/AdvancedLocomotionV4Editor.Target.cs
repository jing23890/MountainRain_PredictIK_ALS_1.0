

using UnrealBuildTool;
using System.Collections.Generic;

public class AdvancedLocomotionV4EditorTarget : TargetRules
{
	public AdvancedLocomotionV4EditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;

		ExtraModuleNames.AddRange( new string[] { "AdvancedLocomotionV4" } );
	}
}
