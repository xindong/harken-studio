using System.IO;
using UnrealBuildTool;

public class HarkenCore : ModuleRules
{
    private static string CombineRelative(string BasePath, params string[] RelativeSegments)
    {
        string Result = BasePath;
        foreach (string Segment in RelativeSegments)
        {
            if (Path.IsPathRooted(Segment))
            {
                throw new BuildException("HarkenCore path segment must be relative: {0}", Segment);
            }
            Result = Path.Join(Result, Segment);
        }
        return Result;
    }

    private string ModulePath(params string[] RelativeSegments)
    {
        return Path.GetFullPath(CombineRelative(ModuleDirectory, RelativeSegments));
    }

    public HarkenCore(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;
        PublicSystemIncludePaths.Add(ModulePath("Include"));

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicAdditionalLibraries.Add(ModulePath("Lib", "Win64", "HarkenCoreImport.lib"));
            PublicDelayLoadDLLs.Add("HarkenCore.dll");
            RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/HarkenCore/Win64/HarkenCore.dll");
        }
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
            string LibraryPath = ModulePath("..", "..", "Binaries", "ThirdParty", "HarkenCore", "Mac", "libHarkenCore.dylib");
            PublicAdditionalLibraries.Add(LibraryPath);
            PublicDelayLoadDLLs.Add(LibraryPath);
            RuntimeDependencies.Add(LibraryPath);
        }
        else if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            string LibraryPath = ModulePath("..", "..", "Binaries", "ThirdParty", "HarkenCore", "Linux", "x86_64", "libHarkenCore.so");
            PublicAdditionalLibraries.Add(LibraryPath);
            PublicDelayLoadDLLs.Add(LibraryPath);
            RuntimeDependencies.Add(LibraryPath);
        }
        else if (Target.Platform == UnrealTargetPlatform.Android)
        {
            PublicAdditionalLibraries.Add(ModulePath("Lib", "Android", "arm64-v8a", "libHarkenCore.so"));
            string PluginPath = Utils.MakePathRelativeTo(ModuleDirectory, Target.RelativeEnginePath);
            AdditionalPropertiesForReceipt.Add("AndroidPlugin", CombineRelative(PluginPath, "HarkenCore_APL.xml"));
        }
        else if (Target.Platform == UnrealTargetPlatform.IOS)
        {
            PublicAdditionalLibraries.Add(ModulePath("Lib", "IOS", "libHarkenCore.a"));
            PublicFrameworks.AddRange(new[]
            {
                "CoreFoundation",
                "CoreAudio",
                "AudioToolbox",
                "AVFoundation"
            });
        }
        else
        {
            throw new BuildException("HarkenCore does not support target platform {0}.", Target.Platform);
        }
    }
}
