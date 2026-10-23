using System;
using System.IO;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using Harken.Unity.Editor;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenWaveformTests
    {
        private static string Golden => Path.Combine(Application.streamingAssetsPath, "Audio", "EditorWaveformGolden");

        [Test]
        public void DirectoryRefreshClearsMissingMalformedAndStaleWaveformsWithoutLosingTheCatalog()
        {
            HarkenEditorServiceTests.RequirePublicationTooling();
            string root = Path.Combine(Application.temporaryCachePath, "HarkenWaveform_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                Assert.That(PackManifest.TryLoadManifest(Golden, out var packs), Is.True);
                foreach (string name in new[] { PackManifest.ManifestFileName, HarkenWaveformPublication.FileName })
                    File.Copy(Path.Combine(Golden, name), Path.Combine(root, name));
                foreach (var pack in packs)
                {
                    string destination = Path.Combine(root, pack.Path);
                    Directory.CreateDirectory(Path.GetDirectoryName(destination));
                    File.Copy(Path.Combine(Golden, pack.Path), destination);
                }
                var catalog = new HarkenEditorCatalog();
                string companionPath = Path.Combine(root, HarkenWaveformPublication.FileName);
                byte[] companion = File.ReadAllBytes(companionPath);
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
                AssertWaveform(catalog);
                File.Delete(companionPath);
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.GetActionWaveform("Play_Golden", out var missing, out _), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(missing, Is.Null);
                Assert.That(catalog.CheckReference("Play_Golden", catalog.Publication.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                File.WriteAllText(companionPath, "{}");
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.GetActionWaveform("Play_Golden", out missing, out _), Is.EqualTo(VnResult.VN_BAD_PACK));
                Assert.That(missing, Is.Null);
                File.WriteAllBytes(companionPath, companion);
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK));
                AssertWaveform(catalog);
                File.AppendAllText(Path.Combine(root, PackManifest.ManifestFileName), "\n");
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.GetWaveform("Play_Golden", out var samples, out var duration, out _), Is.EqualTo(VnResult.VN_PACK_STALE_REVISION));
                Assert.That(samples, Is.Empty);
                Assert.That(duration, Is.Null);
                File.WriteAllText(Path.Combine(root, ".harken-pack-publishing"), string.Empty);
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_PACK_STALE_REVISION));
                Assert.That(catalog.Publication, Is.Null);
            }
            finally { Directory.Delete(root, true); }
        }

        [Test]
        public void TextAssetRefreshUsesOnlyItsExplicitCompanionAndOwnsTheCapturedEnvelope()
        {
            HarkenEditorServiceTests.RequirePublicationTooling();
            string folder = "Assets/HarkenWaveform_" + Guid.NewGuid().ToString("N");
            AssetDatabase.CreateFolder("Assets", Path.GetFileName(folder));
            var source = ScriptableObject.CreateInstance<HarkenPackSource>();
            try
            {
                TextAsset Import(string name, string destination)
                {
                    string path = folder + "/" + destination;
                    File.Copy(Path.Combine(Golden, name), path);
                    AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceSynchronousImport);
                    return AssetDatabase.LoadAssetAtPath<TextAsset>(path);
                }
                var serialized = new SerializedObject(source);
                serialized.FindProperty("inventory").objectReferenceValue = Import(PackManifest.ManifestFileName, "Inventory.json");
                serialized.FindProperty("initPack").objectReferenceValue = Import("Init.pack", "Init.bytes");
                serialized.ApplyModifiedPropertiesWithoutUndo();
                var companion = Import(HarkenWaveformPublication.FileName, HarkenWaveformPublication.FileName);
                var catalog = new HarkenEditorCatalog();
                Assert.That(catalog.Refresh(source, null, companion), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
                AssertWaveform(catalog);
                Assert.That(catalog.GetActionWaveform("Play_Golden", out var captured, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.Refresh(source), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.GetActionWaveform("Play_Golden", out var missing, out _), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(missing, Is.Null);
                Assert.That(captured.Source.GetMergedMinMax(), Is.EqualTo(new float[] { 0, 1, -0.25f, 0.5f, -1, 0 }));
                var malformed = new TextAsset("{}");
                try
                {
                    Assert.That(catalog.Refresh(source, null, malformed), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(catalog.WaveformResult, Is.EqualTo(VnResult.VN_BAD_PACK));
                    Assert.That(catalog.Publication, Is.Not.Null);
                }
                finally { Object.DestroyImmediate(malformed); }
                Assert.That(catalog.Refresh(source, null, companion), Is.EqualTo(VnResult.VN_OK));
                AssertWaveform(catalog);
            }
            finally { Object.DestroyImmediate(source); AssetDatabase.DeleteAsset(folder); }
        }

        [Test]
        public void RandomBytesPublicationCachesAllCandidatesAndInvalidatesBothApiProjections()
        {
            HarkenEditorServiceTests.RequirePublicationTooling();
            string random = Path.Combine(Application.streamingAssetsPath, "Audio", "EditorPreviewRandom");
            string direct = Path.Combine(Application.streamingAssetsPath, "Audio", "EditorPreviewDirect");
            string root = Path.Combine(Application.dataPath, "HarkenPreview_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            void Copy(string from)
            {
                foreach (string file in Directory.GetFiles(from))
                    if (!file.EndsWith(".meta"))
                        File.Copy(file, Path.Combine(root, Path.GetFileName(file) + (file.EndsWith(".pack") ? ".bytes" : "")), true);
                AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
            }
            try
            {
                Copy(random);
                var catalog = new HarkenEditorCatalog();
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
                Assert.That(catalog.GetActionPreview("Play_Golden", out var preview, out string error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(preview.Kind, Is.EqualTo(HarkenActionPreviewKind.RandomCandidates));
                Assert.That(preview.Candidates.Count, Is.EqualTo(3));
                Assert.That(preview.Candidates[0].Source, Is.SameAs(preview.Candidates[2].Source));
                Assert.That(preview.DurationKind, Is.EqualTo(HarkenActionDurationKind.Bounded));
                Assert.That(preview.MaximumDurationSeconds, Is.EqualTo(8.0 / 24000));
                Assert.That(preview.Overview.MinMax[15], Is.EqualTo(0));
                Assert.That(catalog.GetActionPreview("Play_Golden", out var cached, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(cached, Is.SameAs(preview));
                Assert.That(cached.Overview, Is.SameAs(preview.Overview));
                Assert.That(catalog.GetWaveform("Play_Golden", out var samples, out var duration, out _), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(samples, Is.Empty); Assert.That(duration, Is.Null);
                string path = Path.Combine(root, HarkenWaveformPublication.FileName);
                File.Delete(path);
                AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.GetActionPreview("Play_Golden", out cached, out _), Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
                Assert.That(cached, Is.Null);
                Copy(random);
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.GetActionPreview("Play_Golden", out cached, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(cached, Is.Not.SameAs(preview));
                File.WriteAllText(path, "{}");
                AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.GetActionPreview("Play_Golden", out cached, out _), Is.EqualTo(VnResult.VN_BAD_PACK));
                Assert.That(cached, Is.Null);
                Assert.That(catalog.CheckReference("Play_Golden", catalog.Publication.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                Copy(direct);
                Assert.That(catalog.Refresh(root), Is.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.GetActionPreview("Play_Golden", out cached, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(cached.Kind, Is.EqualTo(HarkenActionPreviewKind.DirectSound));
                AssertWaveform(catalog);
            }
            finally { AssetDatabase.DeleteAsset("Assets/" + Path.GetFileName(root)); }
        }

        private static void AssertWaveform(HarkenEditorCatalog catalog)
        {
            Assert.That(catalog.GetWaveform("Play_Golden", out var samples, out var duration, out string error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(samples, Is.EqualTo(new float[] { 0, 1, -0.25f, 0.5f, -1, 0 }));
            Assert.That(duration, Is.EqualTo(3.0 / 48000));
        }
    }
}
