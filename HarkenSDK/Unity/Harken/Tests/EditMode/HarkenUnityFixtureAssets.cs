using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Tests
{
    public static class HarkenUnityFixtureAssets
    {
        private const string Root = "Assets/HarkenSdkFixtureAssets";

        [MenuItem("Harken/SDK Tests/Prepare Player TextAssets")]
        public static void Prepare()
        {
            if (EditorApplication.isPlayingOrWillChangePlaymode)
                throw new InvalidOperationException("Prepare fixture assets outside Play Mode.");
            HarkenProjectSettings settings = Editor.HarkenProjectSettingsProvider.GetOrCreateSettings();
            string audioRoot = Path.Combine(Application.streamingAssetsPath, "Audio");
            settings.SetPlayerExports(Directory.GetFiles(audioRoot, "Init.json", SearchOption.AllDirectories)
                .Select(path => Path.GetFileName(Path.GetDirectoryName(path)))
                .Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(name => name, StringComparer.Ordinal).ToArray());
            EditorUtility.SetDirty(settings);
            PrepareSource(Path.Combine(audioRoot, "U4Original"), "Source");
            PrepareVariableSource();
            AssetDatabase.SaveAssets();
        }

        public static void PrepareVariableSource()
        {
            PrepareSource(Path.Combine(Application.streamingAssetsPath, "Audio/VariableNames"), "VariableSource");
        }

        private static void PrepareSource(string publicationRoot, string name)
        {
            string resourceRoot = Root + "/Resources/HarkenSdkFixture";
            EnsureFolder(Root + "/PackBytes/" + name);
            EnsureFolder(resourceRoot);
            string sourcePath = resourceRoot + "/" + name + ".asset";
            HarkenPackSource source = AssetDatabase.LoadAssetAtPath<HarkenPackSource>(sourcePath);
            if (source == null)
            {
                source = ScriptableObject.CreateInstance<HarkenPackSource>();
                AssetDatabase.CreateAsset(source, sourcePath);
            }
            TextAsset initJson = Import(Path.Combine(publicationRoot, "Init.json"), resourceRoot + "/" + name + "Init.json");
            var serialized = new SerializedObject(source);
            serialized.FindProperty("language").stringValue = string.Empty;
            serialized.FindProperty("initPack").objectReferenceValue = Import(
                Path.Combine(publicationRoot, "Init.bytes"), Root + "/PackBytes/" + name + "/Init.bytes");
            string[] content = Directory.GetFiles(publicationRoot, "*", SearchOption.AllDirectories)
                .Where(path => (path.EndsWith(".bytes", StringComparison.Ordinal) || path.EndsWith(".json", StringComparison.Ordinal)) &&
                    Path.GetFileName(path) != "Init.bytes" && Path.GetFileName(path) != "Init.json")
                .OrderBy(path => path, StringComparer.Ordinal).ToArray();
            SerializedProperty rows = serialized.FindProperty("content");
            rows.arraySize = content.Length;
            for (int i = 0; i < content.Length; ++i)
            {
                string relative = Path.GetRelativePath(publicationRoot, content[i]).Replace('\\', '/');
                SerializedProperty row = rows.GetArrayElementAtIndex(i);
                row.FindPropertyRelative("relativePath").stringValue = relative;
                row.FindPropertyRelative("asset").objectReferenceValue = Import(content[i], Root + "/PackBytes/" + name + "/" + relative);
            }
            serialized.ApplyModifiedPropertiesWithoutUndo();
            VnResult result = source.CaptureCompanion(initJson, out _, out string error);
            if (result != VnResult.VN_OK) throw new InvalidDataException(error);
            EditorUtility.SetDirty(source);
            AssetDatabase.SaveAssets();
        }

        private static TextAsset Import(string source, string destination)
        {
            EnsureFolder(Path.GetDirectoryName(destination).Replace('\\', '/'));
            File.Copy(source, destination, true);
            AssetDatabase.ImportAsset(destination, ImportAssetOptions.ForceSynchronousImport);
            TextAsset asset = AssetDatabase.LoadAssetAtPath<TextAsset>(destination);
            if (asset == null || !asset.bytes.SequenceEqual(File.ReadAllBytes(source)))
                throw new InvalidDataException("Imported fixture bytes differ: " + destination);
            return asset;
        }

        private static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            string parent = Path.GetDirectoryName(path).Replace('\\', '/');
            EnsureFolder(parent);
            AssetDatabase.CreateFolder(parent, Path.GetFileName(path));
        }
    }
}
