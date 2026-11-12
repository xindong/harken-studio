#if UNITY_EDITOR
using System;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace Harken.Unity.Editor
{
    internal static class HarkenPluginImporterSetup
    {
        private const string ScriptGuid = "4b615681c32c41208a1bdd5a7be4779c";
        private const string ScriptSuffix = "/Editor/HarkenPluginImporterSetup.cs";

        [Serializable]
        private sealed class PackageIdentity
        {
            public string name = string.Empty;
        }

        internal static string FindPluginRoot()
        {
            string scriptPath = AssetDatabase.GUIDToAssetPath(ScriptGuid);
            if (!scriptPath.EndsWith(ScriptSuffix, StringComparison.Ordinal) ||
                AssetDatabase.LoadAssetAtPath<MonoScript>(scriptPath) == null)
                return null;

            string root = scriptPath.Substring(0, scriptPath.Length - ScriptSuffix.Length);
            TextAsset manifest = AssetDatabase.LoadAssetAtPath<TextAsset>(root + "/package.json");
            if (manifest == null) return null;
            try
            {
                if (JsonUtility.FromJson<PackageIdentity>(manifest.text)?.name != "com.harken.core")
                    return null;
            }
            catch (ArgumentException)
            {
                return null;
            }
            return root + "/Runtime/Plugins/";
        }

        internal static bool IsRelevantAsset(string path, string pluginRoot)
        {
            if (path.EndsWith(ScriptSuffix, StringComparison.Ordinal) ||
                path.EndsWith("/package.json", StringComparison.Ordinal))
                return true;
            if (pluginRoot == null) return false;
            string root = pluginRoot.Substring(0, pluginRoot.Length - "/Runtime/Plugins/".Length);
            return path.Equals(root, StringComparison.Ordinal) ||
                path.StartsWith(root + "/", StringComparison.Ordinal) ||
                root.StartsWith(path + "/", StringComparison.Ordinal);
        }

        private static readonly BuildTarget[] SupportedTargets =
        {
            BuildTarget.StandaloneWindows64,
            BuildTarget.StandaloneLinux64,
            BuildTarget.StandaloneOSX,
            BuildTarget.Android,
            BuildTarget.iOS
        };

        private static bool scheduled;

        [InitializeOnLoadMethod]
        private static void Initialize()
        {
            Schedule();
        }

        internal static void Schedule()
        {
            if (scheduled)
            {
                return;
            }
            scheduled = true;
            EditorApplication.delayCall += ConfigureImporters;
        }

        private readonly struct NativePlugin
        {
            internal readonly string Release, Development, EditorOs, EditorCpu, TargetCpu, TargetOs;
            internal readonly bool EditorCompatible;
            internal readonly BuildTarget Target;
            internal readonly bool? AddToEmbeddedBinaries;

            internal NativePlugin(string release, string development, bool editorCompatible,
                string editorOs, string editorCpu, BuildTarget target, string targetCpu,
                string targetOs, bool? addToEmbeddedBinaries = null)
            {
                Release = release;
                Development = development;
                EditorCompatible = editorCompatible;
                EditorOs = editorOs;
                EditorCpu = editorCpu;
                Target = target;
                TargetCpu = targetCpu;
                TargetOs = targetOs;
                AddToEmbeddedBinaries = addToEmbeddedBinaries;
            }
        }

        private static readonly NativePlugin[] NativePlugins =
        {
            new NativePlugin("x86_64/HarkenEngine.dll", "x86_64/HarkenEngineDev.dll", true,
                "Windows", "x86_64", BuildTarget.StandaloneWindows64, "x86_64", "Windows"),
            new NativePlugin("x86_64/libHarkenEngine.so", "x86_64/libHarkenEngineDev.so", true,
                "Linux", "x86_64", BuildTarget.StandaloneLinux64, "x86_64", null),
            new NativePlugin("macOS/libHarkenEngine.dylib", "macOS/libHarkenEngineDev.dylib", true,
                "OSX", "AnyCPU", BuildTarget.StandaloneOSX, "AnyCPU", null),
            new NativePlugin("Android/arm64-v8a/libHarkenEngine.so", "Android/arm64-v8a/libHarkenEngineDev.so",
                false, null, null, BuildTarget.Android, "ARM64", null),
            new NativePlugin("iOS/libHarkenEngine.a", "iOS/libHarkenEngineDev.a", false,
                null, null, BuildTarget.iOS, "AnyCPU", null, false),
        };

        internal static void ConfigureImporters()
        {
            EditorApplication.delayCall -= ConfigureImporters;
            scheduled = false;
            HarkenNativeLibrarySelection.ApplyScriptingDefines();
            ConfigureFor(HarkenNativeLibrarySelection.PlayerUsesDevelopment(false));
        }

        internal static void ConfigureFor(bool playerDevelopment)
        {
            string pluginRoot = FindPluginRoot();
            if (pluginRoot == null) return;
            bool editorDevelopment = HarkenNativeLibrarySelection.EditorUsesDevelopment();
            foreach (NativePlugin plugin in NativePlugins)
            {
                Configure(pluginRoot, plugin.Release, plugin.EditorCompatible && !editorDevelopment,
                    !playerDevelopment, plugin);
                Configure(pluginRoot, plugin.Development, plugin.EditorCompatible && editorDevelopment,
                    playerDevelopment, plugin);
            }
        }

        private static void Configure(
            string pluginRoot, string relativePath, bool editorCompatible,
            bool platformEnabled, NativePlugin plugin)
        {
            string editorOs = plugin.EditorOs, editorCpu = plugin.EditorCpu;
            string targetCpu = plugin.TargetCpu, targetOs = plugin.TargetOs;
            BuildTarget target = plugin.Target;
            bool? addToEmbeddedBinaries = plugin.AddToEmbeddedBinaries;
            string path = pluginRoot + relativePath;
            if (!(AssetImporter.GetAtPath(path) is PluginImporter importer))
            {
                return;
            }
            if (IsConfigured(
                importer, editorCompatible, platformEnabled, editorOs, editorCpu, target,
                targetCpu, targetOs, addToEmbeddedBinaries))
            {
                return;
            }

            importer.SetCompatibleWithAnyPlatform(false);
            importer.SetCompatibleWithEditor(editorCompatible);
            foreach (BuildTarget supportedTarget in SupportedTargets)
            {
                importer.SetCompatibleWithPlatform(supportedTarget, false);
            }
            importer.SetCompatibleWithPlatform(target, platformEnabled);
            if (editorCompatible)
            {
                importer.SetEditorData("OS", editorOs);
                importer.SetEditorData("CPU", editorCpu);
            }
            importer.SetPlatformData(target, "CPU", targetCpu);
            if (targetOs != null)
            {
                importer.SetPlatformData(target, "OS", targetOs);
            }
            if (addToEmbeddedBinaries.HasValue)
            {
                importer.SetPlatformData(
                    target, "AddToEmbeddedBinaries",
                    addToEmbeddedBinaries.Value ? "true" : "false");
            }

            importer.SaveAndReimport();
        }

        private static bool IsConfigured(
            PluginImporter importer, bool editorCompatible, bool platformEnabled,
            string editorOs, string editorCpu, BuildTarget target, string targetCpu,
            string targetOs, bool? addToEmbeddedBinaries)
        {
            if (importer.GetCompatibleWithAnyPlatform() ||
                importer.GetCompatibleWithEditor() != editorCompatible)
            {
                return false;
            }
            foreach (BuildTarget supportedTarget in SupportedTargets)
            {
                bool expected = supportedTarget == target && platformEnabled;
                if (importer.GetCompatibleWithPlatform(supportedTarget) !=
                    expected)
                {
                    return false;
                }
            }
            if (editorCompatible &&
                (importer.GetEditorData("OS") != editorOs ||
                 importer.GetEditorData("CPU") != editorCpu))
            {
                return false;
            }
            if (importer.GetPlatformData(target, "CPU") != targetCpu ||
                (targetOs != null &&
                 importer.GetPlatformData(target, "OS") != targetOs))
            {
                return false;
            }
            return !addToEmbeddedBinaries.HasValue ||
                importer.GetPlatformData(
                    target, "AddToEmbeddedBinaries") ==
                (addToEmbeddedBinaries.Value ? "true" : "false");
        }
    }

    internal static class HarkenNativeLibrarySelection
    {
        internal const string ReleaseDefine = "HARKEN_NATIVE_RELEASE";
        internal const string DevelopmentDefine = "HARKEN_NATIVE_DEVELOPMENT";

        private static readonly NamedBuildTarget[] DefineTargets =
        {
            NamedBuildTarget.Standalone, NamedBuildTarget.Android, NamedBuildTarget.iOS
        };

        internal static HarkenNativeLibrary Current =>
            HarkenEditorProjectSettings.Load()?.NativeLibrary ?? HarkenNativeLibrary.Automatic;

        internal static bool EditorUsesDevelopment() => Current != HarkenNativeLibrary.Release;

        internal static bool PlayerUsesDevelopment(bool developmentBuild) =>
            Current == HarkenNativeLibrary.Development ||
            (Current == HarkenNativeLibrary.Automatic && developmentBuild);

        internal static void ApplyScriptingDefines()
        {
            HarkenNativeLibrary current = Current;
            foreach (NamedBuildTarget target in DefineTargets)
            {
                PlayerSettings.GetScriptingDefineSymbols(target, out string[] defines);
                var next = defines.Where(define => define != ReleaseDefine && define != DevelopmentDefine).ToList();
                if (current == HarkenNativeLibrary.Release) next.Add(ReleaseDefine);
                else if (current == HarkenNativeLibrary.Development) next.Add(DevelopmentDefine);
                if (!next.SequenceEqual(defines))
                    PlayerSettings.SetScriptingDefineSymbols(target, next.ToArray());
            }
        }
    }

    internal sealed class HarkenNativeLibraryBuildHook : IPreprocessBuildWithReport, IPostprocessBuildWithReport
    {
        public int callbackOrder => 0;

        public void OnPreprocessBuild(BuildReport report)
        {
            HarkenNativeLibrarySelection.ApplyScriptingDefines();
            HarkenPluginImporterSetup.ConfigureFor(HarkenNativeLibrarySelection.PlayerUsesDevelopment(
                (report.summary.options & BuildOptions.Development) != 0));
        }

        public void OnPostprocessBuild(BuildReport report) => HarkenPluginImporterSetup.ConfigureImporters();
    }

    internal sealed class HarkenPluginImporterPostprocessor : AssetPostprocessor
    {
        private static void OnPostprocessAllAssets(
            string[] importedAssets, string[] deletedAssets,
            string[] movedAssets, string[] movedFromAssetPaths)
        {
            string pluginRoot = HarkenPluginImporterSetup.FindPluginRoot();
            foreach (string[] paths in new[] { importedAssets, deletedAssets, movedAssets, movedFromAssetPaths })
            {
                foreach (string path in paths)
                {
                    if (HarkenPluginImporterSetup.IsRelevantAsset(path, pluginRoot))
                    {
                        HarkenPluginImporterSetup.Schedule();
                        return;
                    }
                }
            }
        }
    }
}
#endif
