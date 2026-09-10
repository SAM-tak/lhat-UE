using UnrealBuildTool;

public class LhatEditor : ModuleRules
{
	public LhatEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "Lhat", "Json"
		});
	}
}
