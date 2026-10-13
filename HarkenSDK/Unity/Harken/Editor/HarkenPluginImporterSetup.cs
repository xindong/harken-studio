#if UNITY_EDITOR
using System;
using UnityEditor;
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

        internal static void ConfigureImporters()
        {
            EditorApplication.delayCall -= ConfigureImporters;
            scheduled = false;
            string pluginRoot = FindPluginRoot();
            if (pluginRoot == null) return;
            Configure(
                pluginRoot, "x86_64/HarkenCore.dll", true, "Windows",
                "x86_64", BuildTarget.StandaloneWindows64, "x86_64",
                "Windows");
            Configure(
                pluginRoot, "x86_64/libHarkenCore.so", true, "Linux",
                "x86_64", BuildTarget.StandaloneLinux64, "x86_64", null);
            Configure(
                pluginRoot, "macOS/libHarkenCore.dylib", true, "OSX",
                "AnyCPU", BuildTarget.StandaloneOSX, "AnyCPU", null);
            Configure(
                pluginRoot, "Android/arm64-v8a/libHarkenCore.so", false,
                null, null, BuildTarget.Android, "ARM64", null);
            Configure(
                pluginRoot, "iOS/libHarkenCore.a", false, null, null,
                BuildTarget.iOS, "AnyCPU", null, false);
        }

        private static void Configure(
            string pluginRoot, string relativePath, bool editorCompatible,
            string editorOs, string editorCpu, BuildTarget target,
            string targetCpu, string targetOs,
            bool? addToEmbeddedBinaries = null)
        {
            string path = pluginRoot + relativePath;
            if (!(AssetImporter.GetAtPath(path) is PluginImporter importer))
            {
                return;
            }
            if (IsConfigured(
                importer, editorCompatible, editorOs, editorCpu, target,
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
            importer.SetCompatibleWithPlatform(target, true);
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
            PluginImporter importer, bool editorCompatible, string editorOs,
            string editorCpu, BuildTarget target, string targetCpu,
            string targetOs, bool? addToEmbeddedBinaries)
        {
            if (importer.GetCompatibleWithAnyPlatform() ||
                importer.GetCompatibleWithEditor() != editorCompatible)
            {
                return false;
            }
            foreach (BuildTarget supportedTarget in SupportedTargets)
            {
                bool expected = supportedTarget == target;
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
