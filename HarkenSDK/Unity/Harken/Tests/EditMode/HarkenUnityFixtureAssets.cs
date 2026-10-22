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
            // Player startup enforces the configured export selection. Include the
            // installed fixture publications just as a game build selects its exports.
            string audioRoot = Path.Combine(Application.streamingAssetsPath, "Audio");
            settings.SetPlayerExports(Directory.GetFiles(audioRoot, PackManifest.ManifestFileName,
                SearchOption.AllDirectories).Select(path => Path.GetFileName(Path.GetDirectoryName(path)))
                .Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(name => name, StringComparer.Ordinal).ToArray());
            EditorUtility.SetDirty(settings);
            string publicationRoot = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
            PrepareSource(publicationRoot, "Source");
            string sampleRoot = Path.Combine(Application.streamingAssetsPath, "Audio/U4Sample/Windows");
            PrepareSource(sampleRoot, "VariableSource");
            VnResult result = Editor.HarkenVariableCatalogGenerator.Generate(
                Path.Combine(sampleRoot, PackManifest.ManifestFileName),
                Editor.HarkenVariableCatalogGenerator.ResolveOutputPath(), out string error);
            if (result != VnResult.VN_OK) throw new InvalidDataException(error);
        }

        public static void PrepareVariableSource()
        {
            PrepareSource(Path.Combine(Application.streamingAssetsPath, "Audio/U4Sample/Windows"), "VariableSource");
        }

        private static void PrepareSource(string publicationRoot, string name)
        {
            VnResult result = GameplayPublication.Read(publicationRoot, null, out GameplayPublication publication, out string error);
            if (result != VnResult.VN_OK) throw new InvalidDataException(error);
            EnsureFolder(Root + "/PackBytes");
            EnsureFolder(Root + "/Resources/HarkenSdkFixture");
            string sourcePath = Root + "/Resources/HarkenSdkFixture/" + name + ".asset";
            HarkenPackSource source = AssetDatabase.LoadAssetAtPath<HarkenPackSource>(sourcePath);
            if (source == null)
            {
                source = ScriptableObject.CreateInstance<HarkenPackSource>();
                AssetDatabase.CreateAsset(source, sourcePath);
            }
            var serialized = new SerializedObject(source);
            serialized.FindProperty("inventory").objectReferenceValue = Import(
                Path.Combine(publicationRoot, PackManifest.ManifestFileName), name + "Inventory.json");
            serialized.FindProperty("selectedManifest").objectReferenceValue = null;
            serialized.FindProperty("language").stringValue = publication.Language;
            serialized.FindProperty("initPack").objectReferenceValue = Import(
                Path.Combine(publicationRoot, publication.InitPack.Path), name + "Init.bytes");
            PackManifestEntry[] content = publication.Packs.Where(row => row.PackStableId != publication.InitPackStableId).ToArray();
            SerializedProperty rows = serialized.FindProperty("content");
            rows.arraySize = content.Length;
            for (int i = 0; i < content.Length; ++i)
            {
                SerializedProperty row = rows.GetArrayElementAtIndex(i);
                row.FindPropertyRelative("relativePath").stringValue = content[i].Path;
                row.FindPropertyRelative("asset").objectReferenceValue = Import(
                    Path.Combine(publicationRoot, content[i].Path), content[i].PackStableId + ".bytes");
            }
            serialized.ApplyModifiedPropertiesWithoutUndo();
            result = source.Capture(out _, out error);
            if (result != VnResult.VN_OK) throw new InvalidDataException(error);
            EditorUtility.SetDirty(source);
            AssetDatabase.SaveAssets();
            Debug.Log("Harken SDK Player fixture prepared: " + sourcePath);
        }

        private static TextAsset Import(string source, string name)
        {
            string destination = Root + "/PackBytes/" + name;
            File.Copy(source, destination, true);
            AssetDatabase.ImportAsset(destination, ImportAssetOptions.ForceSynchronousImport);
            TextAsset asset = AssetDatabase.LoadAssetAtPath<TextAsset>(destination);
            if (asset == null || !asset.bytes.SequenceEqual(File.ReadAllBytes(source)))
                throw new InvalidDataException("Imported fixture bytes differ: " + destination);
            return asset;
        }

        private static void EnsureFolder(string folder)
        {
            if (AssetDatabase.IsValidFolder(folder)) return;
            string parent = Path.GetDirectoryName(folder).Replace('\\', '/');
            EnsureFolder(parent);
            AssetDatabase.CreateFolder(parent, Path.GetFileName(folder));
        }
    }
}
