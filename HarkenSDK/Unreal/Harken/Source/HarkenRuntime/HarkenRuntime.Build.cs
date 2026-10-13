using UnrealBuildTool;

public class HarkenRuntime : ModuleRules
{
    public HarkenRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "DeveloperSettings"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Projects",
            "Json",
            "HarkenCore"
        });

        RuntimeDependencies.Add("$(ProjectDir)/Content/Harken/...", StagedFileType.NonUFS);
    }
}
