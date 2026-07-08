using UnrealBuildTool;

public class wowEditor : ModuleRules
{
	public wowEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"AssetRegistry",
			"AssetTools",
			"AnimationCore",
			"AnimationDataController",
			"MeshConversion",
			"MeshDescription",
			"SkeletalMeshDescription",
			"SkeletalMeshUtilitiesCommon",
			"UnrealEd"
		});
	}
}
