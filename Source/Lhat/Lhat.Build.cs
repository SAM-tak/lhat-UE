using System.IO;
using UnrealBuildTool;

public class Lhat : ModuleRules
{
	public Lhat(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Keep this small binding module's translation units independently checked.
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine"
		});
		PrivateDependencyModuleNames.Add("Projects");

		if (Target.Platform != UnrealTargetPlatform.Win64)
		{
			throw new BuildException("Lhat currently supports Win64 only.");
		}

		string PluginDirectory = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string LhatCoreDirectory = Path.Combine(PluginDirectory, "LhatCore");
		string BuildDirectory = Path.Combine(PluginDirectory, "Intermediate", "LhatCore", "Win64", "Native");
		string CMakeConfiguration = Target.Configuration == UnrealTargetConfiguration.Debug ? "Debug" : "Release";
		string GeneratedIncludeDirectory = Path.Combine(BuildDirectory, "include");
		string CoreLibrary = Path.Combine(BuildDirectory, CMakeConfiguration, "lhat.lib");
		string PortLibrary = Path.Combine(BuildDirectory, CMakeConfiguration, "lhatport.lib");

		if (!Directory.Exists(LhatCoreDirectory))
		{
			throw new BuildException("LhatCore submodule is missing. Run 'git submodule update --init --recursive' from the lhat-UE repository.");
		}

		if (!Directory.Exists(GeneratedIncludeDirectory) || !File.Exists(CoreLibrary) || !File.Exists(PortLibrary))
		{
			throw new BuildException("Lhat native libraries are missing. Run Plugins/lhat-UE/Scripts/BuildLhat.ps1 before building this target.");
		}

		PublicSystemIncludePaths.Add(Path.Combine(LhatCoreDirectory, "include"));
		PublicSystemIncludePaths.Add(GeneratedIncludeDirectory);
		PublicAdditionalLibraries.Add(CoreLibrary);
		PublicAdditionalLibraries.Add(PortLibrary);
	}
}
