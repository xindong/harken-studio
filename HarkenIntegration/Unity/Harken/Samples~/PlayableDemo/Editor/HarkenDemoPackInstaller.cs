using System.IO;
using UnityEditor;
using UnityEngine;
using Harken.Unity.Editor;

namespace Harken.Samples.PlayableDemo.Editor
{
    // Temporarily suspend automatic installation and the menu until the demo ships current Pack companions.
    // [InitializeOnLoad]
    public static class HarkenDemoPackInstaller
    {
        private const string Destination =
            "Assets/StreamingAssets/Audio/GeneratedPacks";

        static HarkenDemoPackInstaller()
        {
            if (HarkenPlayableDemoController.IsAvailable)
                EditorApplication.delayCall += InstallIfImported;
        }

        // [MenuItem("Harken/Playable Demo/Install Packs")]
        public static void InstallIfImported()
        {
            if (!HarkenPlayableDemoController.IsAvailable)
            {
                Debug.LogWarning(HarkenPlayableDemoController.UnavailableReason);
                return;
            }
            string source = FindPackSource();
            if (string.IsNullOrEmpty(source) || !Directory.Exists(source))
            {
                return;
            }
            CopyTree(source, Destination);
            AssetDatabase.Refresh();
            var settings = HarkenProjectSettingsProvider.GetOrCreateSettings();
            var serialized = new SerializedObject(settings);
            var directory = serialized.FindProperty("packDirectory");
            if (string.IsNullOrWhiteSpace(directory.stringValue))
            {
                directory.stringValue = Destination;
                serialized.ApplyModifiedPropertiesWithoutUndo();
                AssetDatabase.SaveAssetIfDirty(settings);
            }
            Debug.Log("Harken demo Packs installed at " + Destination);
        }

        private static void CopyTree(string sourceRoot, string destinationRoot)
        {
            Directory.CreateDirectory(destinationRoot);
            foreach (string directory in Directory.GetDirectories(
                         sourceRoot, "*", SearchOption.AllDirectories))
            {
                string relative = RelativePath(sourceRoot, directory);
                if (IsHiddenRelative(relative))
                {
                    continue;
                }
                Directory.CreateDirectory(Path.Combine(destinationRoot, relative));
            }
            foreach (string file in Directory.GetFiles(
                         sourceRoot, "*", SearchOption.AllDirectories))
            {
                if (file.EndsWith(".meta"))
                {
                    continue;
                }
                string relative = RelativePath(sourceRoot, file);
                if (IsHiddenRelative(relative))
                {
                    continue;
                }
                string target = Path.Combine(destinationRoot, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                if (!File.Exists(target) || !FilesEqual(file, target))
                {
                    File.Copy(file, target, true);
                }
            }
        }

        private static string RelativePath(string root, string path)
        {
            string fullRoot = Path.GetFullPath(root).TrimEnd(
                Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) +
                Path.DirectorySeparatorChar;
            string fullPath = Path.GetFullPath(path);
            return fullPath.Substring(fullRoot.Length);
        }

        private static bool IsHiddenRelative(string relative)
        {
            string[] parts = relative.Replace('\\', '/').Split('/');
            foreach (string part in parts)
            {
                if (part.StartsWith("."))
                {
                    return true;
                }
            }
            return false;
        }

        private static string FindPackSource()
        {
            string[] candidates = AssetDatabase.FindAssets(
                "HarkenDemoPackInstaller t:MonoScript");
            foreach (string guid in candidates)
            {
                string script = AssetDatabase.GUIDToAssetPath(guid);
                if (!script.StartsWith("Assets/"))
                {
                    continue;
                }
                string editor = Path.GetDirectoryName(script);
                return Path.GetFullPath(Path.Combine(editor, "../PackSource"));
            }
            return string.Empty;
        }

        private static bool FilesEqual(string left, string right)
        {
            var leftInfo = new FileInfo(left);
            var rightInfo = new FileInfo(right);
            if (leftInfo.Length != rightInfo.Length)
            {
                return false;
            }
            byte[] leftBytes = File.ReadAllBytes(left);
            byte[] rightBytes = File.ReadAllBytes(right);
            for (int i = 0; i < leftBytes.Length; ++i)
            {
                if (leftBytes[i] != rightBytes[i])
                {
                    return false;
                }
            }
            return true;
        }
    }
}
