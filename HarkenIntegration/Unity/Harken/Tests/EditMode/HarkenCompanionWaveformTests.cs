using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using Harken.Unity.Editor;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using UnityEngine.TestTools;

namespace Harken.Unity.Tests
{
    public sealed class HarkenCompanionWaveformTests
    {
        private static string Root => Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");

        private static byte[] Vkd()
        {
            using (var stream = new MemoryStream())
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write(0x41444b56u); writer.Write(2u); writer.Write(0ul); writer.Write(0ul);
                writer.Write(1u); writer.Write(0u);
                writer.Write(0x45564157u); writer.Write(44u); writer.Write(56u);
                writer.Write(4u); writer.Write(44100u); writer.Write(1u); writer.Write(44100u);
                writer.Write(176401d / 44100);
                for (int i = 0; i < 4; ++i) { writer.Write(-0.25f * (i + 1)); writer.Write(0.5f * (i + 1)); }
                return stream.ToArray();
            }
        }
        private static string Name(byte[] bytes)
        { using (var hash = SHA256.Create()) return BitConverter.ToString(hash.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant() + ".vkd"; }

        [Test]
        public void VkdUsesSourceFrameBoundariesAndAppliesGainExactlyOnce()
        {
            byte[] bytes = Vkd();
            var vkd = HarkenVkd.Read(bytes, Name(bytes));
            var source = vkd.Trim(default, 100, 132350, 2, 3, 4);
            Assert.That(source.SampleRate, Is.EqualTo(44100));
            Assert.That(source.FrameCount, Is.EqualTo(132250));
            Assert.That(Enumerable.Range(0, 5).Select(source.GetBucketFrameBoundary), Is.EqualTo(new ulong[] { 0, 44000, 88100, 132200, 132250 }));
            Assert.That(source.GetMergedMinMax(), Is.EqualTo(new[] { -0.5f, 1, -1, 2, -1.5f, 3, -2, 4 }));
            var tail = vkd.Trim(default, 176400, 176401, 1, 0, 0);
            Assert.That(tail.FrameCount, Is.EqualTo(1));
            Assert.That(tail.GetMergedMinMax(), Is.EqualTo(new[] { -1f, 2f }));
            bytes[bytes.Length - 1] ^= 1;
            Assert.Throws<InvalidDataException>(() => HarkenVkd.Read(bytes, Name(Vkd())));
            var malformed = Vkd(); Array.Copy(BitConverter.GetBytes(float.NaN), 0, malformed, 68, 4);
            Assert.Throws<InvalidDataException>(() => HarkenVkd.Read(malformed, Name(malformed)));
        }

        private sealed class Fixture
        {
            internal readonly Dictionary<string, byte[]> Files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
            internal readonly HarkenEditorCatalog Catalog = new HarkenEditorCatalog();
            internal readonly PackSource Source;
            internal readonly HarkenEditorResources Resources;
            internal readonly string WavePath;
            internal Fixture()
            {
                foreach (string path in Directory.GetFiles(Root)) Files[Path.GetFileName(path)] = File.ReadAllBytes(path);
                Source = PackSource.FromReader(path => Files.TryGetValue(path, out byte[] bytes) ? bytes : null);
                Assert.That(PackCatalogSnapshot.Capture(Source, new[] { "Play_U4_A", "Play_U4_Caller" }, out var snapshot, out string error), Is.EqualTo(VnResult.VN_OK), error);
                byte[] wave = Vkd(); WavePath = "Waveforms/" + Name(wave); Files[WavePath] = wave;
                foreach (var row in snapshot.Actions)
                {
                    string refs = string.Join(",", row.Value.Media.Select(media => "{\"soundGuid\":\"" + media.soundId +
                        "\",\"file\":\"../" + WavePath + "\",\"trimStartFrame\":100,\"trimEndFrame\":132350}"));
                    string json = Encoding.UTF8.GetString(Files[row.Key + ".json"]).TrimEnd();
                    Files[row.Key + ".json"] = Encoding.UTF8.GetBytes(json.Substring(0, json.Length - 1) + ",\"waveforms\":[" + refs + "]}");
                }
                Resources = HarkenEditorResources.FromReader("Windows", path => Files.TryGetValue(path, out byte[] bytes) ? bytes : null);
                Refresh();
            }
            internal void Refresh() => Assert.That(Catalog.Refresh(Source, new[] { "Play_U4_A", "Play_U4_Caller" }, Resources), Is.EqualTo(VnResult.VN_OK), Catalog.Diagnostic);
        }

        [Test]
        public void OptionalFailureRetryParentCollectionAndCopyKeepExecutionIndependent()
        {
            var fixture = new Fixture();
            var catalog = fixture.Catalog;
            var prepared = catalog.PreparedPacks;
            Assert.That(catalog.GetSourceWaveforms("Play_U4_Caller", out var parent, out string error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(parent.Count, Is.EqualTo(prepared.Snapshot.Actions["Play_U4_Caller"].Media.Length));
            Assert.That(parent.Count, Is.GreaterThan(0));
            Assert.That(catalog.GetActionWaveform("Play_U4_A", out var waveform, out error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(waveform.OneShotDurationSeconds, Is.EqualTo(4).Within(0.001));
            Assert.That(waveform.Source.SourceDurationSeconds, Is.Not.EqualTo(4));
            string destination = Path.Combine(Application.temporaryCachePath, "HarkenCopy_" + Guid.NewGuid().ToString("N"));
            try
            {
                Directory.CreateDirectory(destination); File.WriteAllText(Path.Combine(destination, "keep.txt"), "keep");
                Assert.That(catalog.CopyResources(destination, new[] { "Play_U4_A" }, out var paths, out error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(paths, Does.Contain("Windows/Init.bytes")); Assert.That(paths, Does.Contain(fixture.WavePath));
                Assert.That(File.ReadAllText(Path.Combine(destination, "keep.txt")), Is.EqualTo("keep"));
                var copied = new HarkenEditorCatalog();
                Assert.That(copied.Refresh(Path.Combine(destination, "Windows")), Is.EqualTo(VnResult.VN_OK), copied.Diagnostic);
                Assert.That(copied.GetActionWaveform("Play_U4_A", out _, out error), Is.EqualTo(VnResult.VN_OK), error);
            }
            finally { if (Directory.Exists(destination)) Directory.Delete(destination, true); }
            fixture.Files.Remove(fixture.WavePath); fixture.Refresh();
            Assert.That(catalog.PreparedPacks, Is.SameAs(prepared));
            Assert.That(catalog.GetActionWaveform("Play_U4_A", out waveform, out _), Is.Not.EqualTo(VnResult.VN_OK));
            Assert.That(waveform, Is.Null);
            Assert.That(catalog.GetDuration("Play_U4_A", out var duration, out _), Is.EqualTo(VnResult.VN_OK));
            Assert.That(duration.ExactSeconds, Is.EqualTo(4).Within(0.001));
            fixture.Files[fixture.WavePath] = Vkd(); fixture.Refresh();
            Assert.That(catalog.GetActionWaveform("Play_U4_A", out _, out error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.Throws<IOException>(() => fixture.Resources.Resolve("Play_U4_A.json", "../../escape.vkd"));
        }

        [Test]
        public void TextAssetWaveformsKeepBinaryIdentityAndDisappearOnRefresh()
        {
            var fixture = new Fixture();
            string folder = "Assets/HarkenVkd_" + Guid.NewGuid().ToString("N");
            AssetDatabase.CreateFolder("Assets", folder.Substring(7));
            string path = folder + "/source.bytes";
            try
            {
                File.WriteAllBytes(path, Vkd());
                AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceSynchronousImport);
                var asset = AssetDatabase.LoadAssetAtPath<TextAsset>(path);
                Assert.That(asset.bytes, Is.EqualTo(Vkd()));
                var resources = HarkenEditorResources.FromAssets("Windows", new[] {
                    new HarkenPackSource.ContentAsset { relativePath = fixture.WavePath, asset = asset } });
                Assert.That(fixture.Catalog.Refresh(fixture.Source, new[] { "Play_U4_A", "Play_U4_Caller" }, resources), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Catalog.GetActionWaveform("Play_U4_A", out _, out var error), Is.EqualTo(VnResult.VN_OK), error);
                AssetDatabase.DeleteAsset(path);
                Assert.That(fixture.Catalog.Refresh(fixture.Source, new[] { "Play_U4_A", "Play_U4_Caller" }, resources), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Catalog.GetActionWaveform("Play_U4_A", out _, out _), Is.Not.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Catalog.GetDuration("Play_U4_A", out var duration, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(duration.ExactSeconds, Is.EqualTo(4).Within(0.001));
            }
            finally { AssetDatabase.DeleteAsset(folder); }
        }

        [UnityTest]
        public IEnumerator RealStartedSelectionClearsOnStopAndDisplayRefresh()
        {
            Assert.That(EditorApplication.isPlaying, Is.False);
            var fixture = new Fixture();
            using (var preview = new HarkenEditorPreview())
            using (var scope = preview.CreateScope())
            {
                Assert.That(scope.Prepare(fixture.Catalog.PreparedPacks, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), scope.Diagnostic);
                for (int play = 0; play != 2; ++play)
                {
                    Assert.That(scope.Play("Play_U4_A", out var firing), Is.EqualTo(VnResult.VN_OK));
                    double deadline = EditorApplication.timeSinceStartup + 2;
                    HarkenActionWaveform waveform = null; string error = "";
                    while (EditorApplication.timeSinceStartup < deadline)
                    {
                        preview.Tick();
                        if (scope.GetPlayingWaveform(firing, fixture.Catalog, out waveform, out error) == VnResult.VN_OK) break;
                        yield return null;
                    }
                    Assert.That(waveform, Is.Not.Null, error);
                    Assert.That(waveform.OneShotDurationSeconds, Is.EqualTo(4).Within(0.001));
                    if (play == 0) fixture.Refresh();
                    else Assert.That(scope.Stop(firing, 0), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.GetPlayingWaveform(firing, fixture.Catalog, out _, out _), Is.Not.EqualTo(VnResult.VN_OK));
                    scope.StopAll(0);
                    deadline = EditorApplication.timeSinceStartup + 2;
                    while (preview.FiringCount > 0 && EditorApplication.timeSinceStartup < deadline) { preview.Tick(); yield return null; }
                    Assert.That(preview.FiringCount, Is.Zero);
                }
                Assert.That(preview.Shutdown(), Is.EqualTo(VnResult.VN_OK));
            }
        }
    }
}
