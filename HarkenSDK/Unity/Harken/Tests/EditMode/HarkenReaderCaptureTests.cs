using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using NUnit.Framework;
using UnityEngine;
using Harken.Unity.Editor;

namespace Harken.Unity.Tests
{
    public sealed class HarkenReaderCaptureTests
    {
        private const string U4Logical = "Assets/Audio/U4Original";
        private const string NameFirstLogical = "Assets/Audio/NameFirstPacks";
        private const string AndroidLogical = "Assets/Audio/PlatformContractPacks/Android_Low";

        [Test]
        public void CapturePreservesTheConfiguredResourceAddressWithoutADiskDirectory()
        {
            using (var lease = HarkenProjectSettingsLease.Acquire())
            {
                string root = "Assets/Audio/GeneratedPacks_" + Guid.NewGuid().ToString("N");
                lease.Settings.SetPackDirectory(root);
                Dictionary<string, byte[]> files = LogicalFiles(lease.Settings.PackDirectory, "U4Original");
                var requested = new List<string>();
                Assert.That(Directory.Exists(root), Is.False);
                string manifest = Manifest(lease.Settings.PackDirectory);
                Assert.That(HarkenPreparedPacks.Capture(manifest, path =>
                {
                    requested.Add(path);
                    return files.TryGetValue(path, out byte[] bytes) ? (byte[])bytes.Clone() : null;
                }, out HarkenPreparedPacks prepared, out _, out string error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(prepared.SelectedManifestPath, Is.EqualTo(manifest));
                Assert.That(requested, Does.Contain(root + "/Init.pack"));
                Assert.That(requested, Has.All.StartsWith(root + "/"));
                Assert.That(lease.Settings.PackDirectory, Is.EqualTo(root));
            }
        }

        [Test]
        public void CaptureReadsInventorySelectedAndInitAndDemandLoadsHashedContent()
        {
            Dictionary<string, byte[]> files = LogicalFiles(U4Logical, "U4Original");
            string manifestPath = Manifest(U4Logical);
            Assert.That(HarkenPreparedPacks.Capture(manifestPath, MemoryReader(files),
                out HarkenPreparedPacks prepared, out byte[] manifestBytes, out string error),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(prepared, Is.Not.Null);
            Assert.That(prepared.SelectedManifestPath, Is.EqualTo(manifestPath));
            Assert.That(manifestBytes, Is.EqualTo(files[manifestPath]));
            Assert.That(prepared.Publication.TryFindAction("Play_U4_A", out GameplayAction action), Is.True);
            Assert.That(prepared.Publication.TryFindPack(action.OwnerPackStableId, out PackManifestEntry pack), Is.True);
            Assert.That(PackManifest.VerifyPackBytes(files[U4Logical + "/" + pack.Path], pack), Is.True);
            using (var preview = new HarkenEditorPreview())
            {
                Assert.That(preview.Prepare(prepared, new[] { pack.PackStableId }, true),
                    Is.EqualTo(VnResult.VN_OK), preview.Diagnostic);
                Assert.That(preview.Session.AcquirePack(pack.PackStableId, out Pack demanded), Is.EqualTo(VnResult.VN_OK));
                Assert.That(demanded.IsValid, Is.True);
            }

            Dictionary<string, byte[]> rootFiles = LogicalFiles(string.Empty, "U4Original");
            Assert.That(HarkenPreparedPacks.Capture(PackManifest.ManifestFileName, MemoryReader(rootFiles),
                out HarkenPreparedPacks rooted, out _, out error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(rooted.SelectedManifestPath, Is.EqualTo(PackManifest.ManifestFileName));
        }

        [Test]
        public void CaptureMissingBytesReturnsContentNotLoadedWithoutPartialPublication()
        {
            Dictionary<string, byte[]> files = LogicalFiles(U4Logical, "U4Original");
            files.Remove(U4Logical + "/Init.pack");
            AssertFailedCapture(Manifest(U4Logical), MemoryReader(files), VnResult.VN_CONTENT_NOT_LOADED);
            AssertFailedCapture(Manifest(U4Logical), path => null, VnResult.VN_CONTENT_NOT_LOADED);
        }

        [Test]
        public void CaptureRejectsInvalidUtf8WithoutSilentReplacement()
        {
            Dictionary<string, byte[]> files = LogicalFiles(U4Logical, "U4Original");
            files[Manifest(U4Logical)] = new byte[] { 0x61, 0xFF, 0x80, 0x62 };
            Assert.That(HarkenPreparedPacks.Capture(Manifest(U4Logical), MemoryReader(files),
                out HarkenPreparedPacks prepared, out byte[] manifestBytes, out string error),
                Is.EqualTo(VnResult.VN_BAD_PACK));
            Assert.That(prepared, Is.Null);
            Assert.That(manifestBytes, Is.Null);
            Assert.That(error, Does.Contain("UTF-8"));
        }

        [Test]
        public void CaptureReturnsTypedResultForInitAndContentHashMismatch()
        {
            Dictionary<string, byte[]> initFiles = LogicalFiles(U4Logical, "U4Original");
            initFiles[U4Logical + "/Init.pack"] = Mutate(initFiles[U4Logical + "/Init.pack"]);
            AssertFailedCapture(Manifest(U4Logical), MemoryReader(initFiles), VnResult.VN_PACK_HASH_MISMATCH);

            Dictionary<string, byte[]> contentFiles = LogicalFiles(U4Logical, "U4Original");
            Assert.That(HarkenPreparedPacks.Capture(Manifest(U4Logical), MemoryReader(contentFiles),
                out HarkenPreparedPacks prepared, out _, out string error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(prepared.Publication.TryFindAction("Play_U4_A", out GameplayAction action), Is.True);
            Assert.That(prepared.Publication.TryFindPack(action.OwnerPackStableId, out PackManifestEntry pack), Is.True);
            contentFiles[U4Logical + "/" + pack.Path] = Mutate(contentFiles[U4Logical + "/" + pack.Path]);
            using (var preview = new HarkenEditorPreview())
            {
                Assert.That(preview.Prepare(prepared, new[] { pack.PackStableId }, true),
                    Is.EqualTo(VnResult.VN_PACK_HASH_MISMATCH));
            }
        }

        [Test]
        public void CaptureSelectsExplicitLanguagePublication()
        {
            Dictionary<string, byte[]> files = LogicalFiles(NameFirstLogical, "NameFirstPacks");
            string manifestPath = Manifest(NameFirstLogical);
            Assert.That(HarkenPreparedPacks.Capture(manifestPath, MemoryReader(files),
                out HarkenPreparedPacks english, out _, out string error, "English"),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(english.Publication.Language, Is.EqualTo("English"));
            Assert.That(english.SelectedManifestPath,
                Is.EqualTo(NameFirstLogical + "/English/" + PackManifest.ManifestFileName));
            Assert.That(HarkenPreparedPacks.Capture(manifestPath, MemoryReader(files),
                out HarkenPreparedPacks japanese, out _, out error, "ja"),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(japanese.Publication.Language, Is.EqualTo("ja"));
            Assert.That(japanese.SelectedManifestPath,
                Is.EqualTo(NameFirstLogical + "/ja/" + PackManifest.ManifestFileName));
            Assert.That(japanese.Publication.Actions[0].OwnerPackStableId,
                Is.Not.EqualTo(english.Publication.Actions[0].OwnerPackStableId));
        }

        [Test]
        public void CaptureAllowsCrossPlatformPublicationAndCatalogReaderRefreshClearsOnEditorMismatch()
        {
            Dictionary<string, byte[]> android = LogicalFiles(AndroidLogical, "PlatformContractPacks/Android_Low");
            string androidManifest = Manifest(AndroidLogical);
            Assert.That(HarkenPreparedPacks.Capture(androidManifest, MemoryReader(android),
                out HarkenPreparedPacks prepared, out _, out string error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(prepared.Publication.Platform.BasePlatform, Is.EqualTo("Android"));

            Dictionary<string, byte[]> windows = LogicalFiles(U4Logical, "U4Original");
            var catalog = new HarkenEditorCatalog();
            Assert.That(catalog.Refresh(Manifest(U4Logical), MemoryReader(windows)), Is.EqualTo(VnResult.VN_OK),
                catalog.Diagnostic);
            uint revision = catalog.Revision;
            Assert.That(catalog.Refresh(androidManifest, MemoryReader(android)), Is.EqualTo(VnResult.VN_INVALID_ARG));
            Assert.That(catalog.Publication, Is.Null);
            Assert.That(catalog.PreparedPacks, Is.Null);
            Assert.That(catalog.Revision, Is.GreaterThan(revision));
        }

        [Test]
        public void CaptureDetectsInventoryOrSelectedChangeAsStaleRevision()
        {
            Dictionary<string, byte[]> files = LogicalFiles(U4Logical, "U4Original");
            string manifestPath = Manifest(U4Logical);
            int inventoryReads = 0;
            Func<string, byte[]> staleInventory = path =>
            {
                byte[] bytes = MemoryReader(files)(path);
                if (path == manifestPath)
                {
                    inventoryReads++;
                    if (inventoryReads >= 2) return Mutate(bytes);
                }
                return bytes;
            };
            AssertFailedCapture(manifestPath, staleInventory, VnResult.VN_PACK_STALE_REVISION);

            Dictionary<string, byte[]> languages = LogicalFiles(NameFirstLogical, "NameFirstPacks");
            string inventory = Manifest(NameFirstLogical);
            string selected = NameFirstLogical + "/English/" + PackManifest.ManifestFileName;
            int selectedReads = 0;
            Func<string, byte[]> staleSelected = path =>
            {
                byte[] bytes = MemoryReader(languages)(path);
                if (path == selected)
                {
                    selectedReads++;
                    if (selectedReads >= 2) return Mutate(bytes);
                }
                return bytes;
            };
            AssertFailedCapture(inventory, staleSelected, VnResult.VN_PACK_STALE_REVISION, "English");
        }

        [Test]
        public void CaptureMapsReaderExceptionsToPackProviderFailed()
        {
            Assert.That(HarkenPreparedPacks.Capture(Manifest(U4Logical),
                path => throw new InvalidOperationException("reader-boom"),
                out HarkenPreparedPacks prepared, out byte[] manifestBytes, out string error),
                Is.EqualTo(VnResult.VN_PACK_PROVIDER_FAILED));
            Assert.That(prepared, Is.Null);
            Assert.That(manifestBytes, Is.Null);
            Assert.That(error, Does.Contain("inventory"));
            Assert.That(error, Does.Contain(Manifest(U4Logical)));
            Assert.That(error, Does.Contain("InvalidOperationException"));
            Assert.That(error, Does.Contain("reader-boom"));
        }

        [Test]
        public void CaptureRejectsUnsafeManifestPaths()
        {
            Func<string, byte[]> reader = path => throw new AssertionException("reader must not run for unsafe paths: " + path);
            foreach (string path in new[]
            {
                "C:/Audio/HarkenPackManifest.json",
                "/HarkenPackManifest.json",
                "../HarkenPackManifest.json",
                "Audio\\HarkenPackManifest.json",
                "Assets/../HarkenPackManifest.json"
            })
                AssertFailedCapture(path, reader, VnResult.VN_INVALID_ARG);
            AssertFailedCapture(Manifest(U4Logical), null, VnResult.VN_INVALID_ARG);
        }

        [Test]
        public void CatalogReaderRefreshKeepsActionsWhenWaveformIsMissingOversizedOrCorrupt()
        {
            Dictionary<string, byte[]> files = LogicalFiles(U4Logical, "U4Original");
            string manifestPath = Manifest(U4Logical);
            string companionPath = U4Logical + "/" + HarkenWaveformPublication.FileName;
            var catalog = new HarkenEditorCatalog();
            Assert.That(catalog.Refresh(manifestPath, MemoryReader(files)), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
            Assert.That(catalog.Publication.TryFindAction("Play_U4_A", out _), Is.True);
            Assert.That(catalog.PreparedPacks, Is.Not.Null);
            Assert.That(catalog.WaveformResult, Is.EqualTo(VnResult.VN_CAPABILITY_UNAVAILABLE));
            uint revision = catalog.Revision;

            files[companionPath] = Encoding.UTF8.GetBytes("{}");
            Assert.That(catalog.Refresh(manifestPath, MemoryReader(files)), Is.EqualTo(VnResult.VN_OK));
            Assert.That(catalog.Publication.TryFindAction("Play_U4_A", out _), Is.True);
            Assert.That(catalog.WaveformResult, Is.EqualTo(VnResult.VN_BAD_PACK));
            Assert.That(catalog.Revision, Is.EqualTo(revision));

            files[companionPath] = new byte[HarkenWaveformPublication.MaxBytes + 1];
            Assert.That(catalog.Refresh(manifestPath, MemoryReader(files)), Is.EqualTo(VnResult.VN_OK));
            Assert.That(catalog.Publication.TryFindAction("Play_U4_A", out _), Is.True);
            Assert.That(catalog.WaveformResult, Is.EqualTo(VnResult.VN_BAD_PACK));
            Assert.That(catalog.WaveformDiagnostic, Does.Contain("byte budget"));

            string golden = Path.Combine(Application.streamingAssetsPath, "Audio", "EditorWaveformGolden");
            if (!Directory.Exists(golden)) return;
            const string goldenLogical = "Assets/Audio/EditorWaveformGolden";
            Dictionary<string, byte[]> goldenFiles = LogicalFiles(goldenLogical, "EditorWaveformGolden");
            Assert.That(catalog.Refresh(Manifest(goldenLogical), MemoryReader(goldenFiles)),
                Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
            Assert.That(catalog.GetActionWaveform("Play_Golden", out HarkenActionWaveform waveform, out string diagnostic),
                Is.EqualTo(VnResult.VN_OK), diagnostic);
            Assert.That(waveform, Is.Not.Null);
        }

        private static void AssertFailedCapture(string manifestPath, Func<string, byte[]> readBytes,
            VnResult expected, string language = null)
        {
            Assert.That(HarkenPreparedPacks.Capture(manifestPath, readBytes,
                out HarkenPreparedPacks prepared, out byte[] manifestBytes, out _),
                Is.EqualTo(expected));
            Assert.That(prepared, Is.Null);
            Assert.That(manifestBytes, Is.Null);
        }

        private static string Manifest(string logicalRoot) => logicalRoot + "/" + PackManifest.ManifestFileName;

        private static byte[] Mutate(byte[] bytes)
        {
            byte[] mutated = (byte[])bytes.Clone();
            mutated[0] ^= 0xFF;
            return mutated;
        }

        private static Dictionary<string, byte[]> LogicalFiles(string logicalRoot, string name)
        {
            string disk = Path.Combine(Application.streamingAssetsPath, "Audio", name);
            var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
            foreach (string path in Directory.GetFiles(disk, "*", SearchOption.AllDirectories))
            {
                string relative = path.Substring(disk.Length)
                    .TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                    .Replace('\\', '/');
                files[string.IsNullOrEmpty(logicalRoot) ? relative : logicalRoot + "/" + relative] =
                    File.ReadAllBytes(path);
            }
            return files;
        }

        private static Func<string, byte[]> MemoryReader(Dictionary<string, byte[]> files)
            => path => files.TryGetValue(path, out byte[] bytes) ? (byte[])bytes.Clone() : null;
    }
}
