using System.IO;
using EpicGames.Core;
using UnrealBuildTool;

public class Lhat : ModuleRules
{
	public Lhat(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Keep this small binding module's translation units independently checked.
		bUseUnity = false;
		PrecompileForTargets = PrecompileTargetsType.Any;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine"
		});
		PrivateDependencyModuleNames.AddRange(new[] { "Projects", "Json", "AssetRegistry" });

		if (Target.Platform != UnrealTargetPlatform.Win64)
		{
			throw new BuildException("Lhat currently supports Win64 only.");
		}

		string PluginDirectory = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		// Only release packages carry this marker. Maintainer/source builds still
		// compile normally; binary consumers use the retained precompiled manifests.
		string PrecompiledMarker = Path.Combine(PluginDirectory, "Build", "LhatPrecompiled.json");
		if (File.Exists(PrecompiledMarker))
		{
			bUsePrecompiled = true;
			if (!Target.bBuildEditor && JsonObject.Read(new FileReference(PrecompiledMarker)).GetBoolField("editor_only"))
				throw new BuildException("This Lhat package is Editor-only. Install a package containing Win64 game targets.");
		}
		string PackagedCoreDirectory = Path.Combine(PluginDirectory, "Build", "LhatCore", "Win64", "Native");
		string BuildDirectory = Directory.Exists(PackagedCoreDirectory) ? PackagedCoreDirectory
			: Path.Combine(PluginDirectory, "Intermediate", "LhatCore", "Win64", "Native");
		string BundledManifest = Path.Combine(PluginDirectory, "Bindings", "BundledEngineApi.generated.json");
		if (!File.Exists(BundledManifest)) throw new BuildException("Bundled Engine bindings are missing. Run Scripts/GenerateBundledBindings.py as a plugin maintainer.");
		ExternalDependencies.Add(BundledManifest);
		{
			JsonObject Groups = JsonObject.Read(new FileReference(BundledManifest)).GetObjectField("groups");
			foreach (string Group in new[] { "LhatGeneratedRuntime", "LhatGeneratedEditor" })
			{
				if (Group == "LhatGeneratedEditor" && !Target.bBuildEditor) continue;
				PrivateDependencyModuleNames.AddRange(Groups.GetObjectField(Group).GetStringArrayField("dependencies"));
				if (Target.bBuildEditor)
					PrivateDependencyModuleNames.AddRange(Groups.GetObjectField(Group).GetStringArrayField("editor_dependencies"));
			}
		}
		string CMakeConfiguration = Target.Configuration == UnrealTargetConfiguration.Debug ? "Debug" : "Release";
		string GeneratedIncludeDirectory = Path.Combine(BuildDirectory, "include");
		string BridgeDirectory = Path.Combine(BuildDirectory, "UEBridge");
		string CoreLibrary = Path.Combine(BuildDirectory, CMakeConfiguration, "lhat.lib");
		string PortLibrary = Path.Combine(BuildDirectory, CMakeConfiguration, "lhatport.lib");

		if (!Directory.Exists(GeneratedIncludeDirectory) || !File.Exists(CoreLibrary) || !File.Exists(PortLibrary)
			|| !File.Exists(Path.Combine(BridgeDirectory, "LhatCoreExports.inl")))
		{
			throw new BuildException("Lhat native libraries are missing. Run Plugins/lhat-UE/Scripts/BuildLhat.ps1 before building this target.");
		}

		// These generated copies decorate public C declarations with LHAT_API.
		// Do not expose the undecorated headers to other UE DLLs.
		PublicSystemIncludePaths.Add(Path.Combine(BridgeDirectory, "include"));
		PublicSystemIncludePaths.Add(GeneratedIncludeDirectory);
		PrivateIncludePaths.Add(BridgeDirectory);
		PublicAdditionalLibraries.Add(CoreLibrary);
		PublicAdditionalLibraries.Add(PortLibrary);
	}
}
