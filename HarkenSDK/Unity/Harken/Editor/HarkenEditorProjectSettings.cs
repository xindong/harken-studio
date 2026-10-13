using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEngine;

namespace Harken.Unity.Editor
{
    public sealed class HarkenEditorProjectSettings : ScriptableObject
    {
        public const string AssetPath = "Assets/Harken/Editor/HarkenEditorProjectSettings.asset";

        [SerializeField] private string audioProjectDirectory = string.Empty;

        public string AudioProjectDirectory => audioProjectDirectory ?? string.Empty;

        [InitializeOnLoadMethod]
        private static void InstallProbe()
        {
            HarkenProjectSettings.ReadAudioProjectDirectory = ReadDirectory;
        }

        private static string ReadDirectory()
        {
            HarkenEditorProjectSettings settings = Load();
            return settings != null ? settings.AudioProjectDirectory : string.Empty;
        }

        public static HarkenEditorProjectSettings Load() =>
            AssetDatabase.LoadAssetAtPath<HarkenEditorProjectSettings>(AssetPath);

        public static HarkenEditorProjectSettings GetOrCreate()
        {
            HarkenEditorProjectSettings settings = Load();
            if (settings != null) return settings;
            EnsureFolder("Assets/Harken");
            EnsureFolder("Assets/Harken/Editor");
            settings = CreateInstance<HarkenEditorProjectSettings>();
            AssetDatabase.CreateAsset(settings, AssetPath);
            AssetDatabase.SaveAssetIfDirty(settings);
            return settings;
        }

        public static void ValidateNotInResources()
        {
            foreach (string guid in AssetDatabase.FindAssets("t:HarkenEditorProjectSettings"))
            {
                string path = AssetDatabase.GUIDToAssetPath(guid);
                if (path.IndexOf("/Resources/", System.StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    throw new BuildFailedException(
                        "The Harken audio project directory must not ship in Resources: " + path);
                }
            }
        }

        private static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            string parent = Path.GetDirectoryName(path)?.Replace('\\', '/');
            string name = Path.GetFileName(path);
            if (!string.IsNullOrEmpty(parent) && !AssetDatabase.IsValidFolder(parent))
                EnsureFolder(parent);
            AssetDatabase.CreateFolder(parent, name);
        }
    }
}
