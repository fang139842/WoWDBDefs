using UnrealBuildTool;
using System.IO;

public class wow : ModuleRules
{
	public wow(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"Json",
			"OpenSSL",
			"Sockets",
			"Networking",
			"ImageWrapper",
			"XmlParser",
			"ProceduralMeshComponent",
			"Slate",
			"SlateCore",
			"ApplicationCore"
		});

		PublicIncludePaths.AddRange(new[]
		{
			Path.Combine(ModuleDirectory, "..", "ThirdParty", "Lua", "lua-5.1.5", "src")
		});

		PublicAdditionalLibraries.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", "Lua", "Lib", "Win64", "lua51.lib"));
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new[]
			{
				"AssetRegistry",
				"UnrealEd"
			});
		}
	}
}
