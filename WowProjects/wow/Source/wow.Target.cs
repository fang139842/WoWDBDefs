using UnrealBuildTool;
using System.Collections.Generic;

public class wowTarget : TargetRules
{
	public wowTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("wow");
	}
}
