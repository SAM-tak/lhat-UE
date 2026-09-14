using UnrealBuildTool;
using System.IO;

public class LhatEditor : ModuleRules
{
	public LhatEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;
		PrecompileForTargets = PrecompileTargetsType.Editor;
		if (File.Exists(Path.Combine(ModuleDirectory, "..", "..", "Build", "LhatPrecompiled.json"))) bUsePrecompiled = true;
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "Lhat", "Json", "AssetRegistry", "Projects",
			"UnrealEd", "BlueprintGraph", "KismetCompiler"
		});
	}
}
