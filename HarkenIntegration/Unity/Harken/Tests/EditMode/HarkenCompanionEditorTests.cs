using System;
using System.Collections;
using System.IO;
using System.Linq;
using Harken.Unity.Editor;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using UnityEngine.TestTools;

namespace Harken.Unity.Tests
{
    public sealed class HarkenCompanionEditorTests
    {
        private static string Root => Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
        private static HarkenEditorCatalog Catalog()
        {
            var catalog = new HarkenEditorCatalog();
            Assert.That(catalog.Refresh(Root), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
            return catalog;
        }

        [Test]
        public void DirectoryAndReaderExposeNativeDurationWithoutOptionalData()
        {
            var catalog = Catalog();
            Assert.That(catalog.Publication.Actions.Select(a => a.Name), Does.Contain("Play_U4_A"));
            Assert.That(catalog.GetDuration("Play_U4_A", out var duration, out string error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(duration.Kind, Is.EqualTo(HarkenActionDurationKind.Exact));
            Assert.That(duration.ExactSeconds, Is.EqualTo(4).Within(0.001));
            Assert.That(catalog.GetPack("Play_U4_A", out var pack, out error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(pack.ByteCount, Is.EqualTo(new FileInfo(Path.Combine(Root, pack.BytesPath)).Length));
            var source = PackSource.FromReader(path => File.Exists(Path.Combine(Root, path)) ? File.ReadAllBytes(Path.Combine(Root, path)) : null);
            Assert.That(catalog.Refresh(source, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
            Assert.That(catalog.GetDuration("Play_U4_A", out duration, out error), Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(duration.ExactSeconds, Is.EqualTo(4).Within(0.001));
            Assert.That(catalog.Refresh(source, new[] { "Missing_Action" }), Is.Not.EqualTo(VnResult.VN_OK));
            Assert.That(catalog.GetDuration("Play_U4_A", out duration, out _), Is.Not.EqualTo(VnResult.VN_OK));
            Assert.That(duration, Is.Null);
            Assert.That(catalog.Publication, Is.Null);
            Assert.That(catalog.Refresh(Root), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
        }

        [Test]
        public void FirstRawLeaseUsesCatalogNameLocatorAndSharesPreparationOwner()
        {
            var catalog = Catalog();
            Assert.That(catalog.GetPreparationOrder("Play_U4_A", out var ids, out _), Is.EqualTo(VnResult.VN_OK));
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_REAL;
            PackBootstrapSession session = null;
            try
            {
                Assert.That(catalog.PreparedPacks.Start(Array.Empty<ulong>(), ref config, out session), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.AcquirePack(ids[0], out Pack first), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.PrepareActions(new[] { "Play_U4_A" }, session.ProjectId, out var preparation), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.AcquirePack(ids[0], out Pack second), Is.EqualTo(VnResult.VN_OK));
                Assert.That(second.Handle, Is.EqualTo(first.Handle));
                Assert.That(session.ReleasePack(ids[0], first), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preparation.Release(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.ReleasePack(ids[0], second), Is.EqualTo(VnResult.VN_OK));
            }
            finally { session?.Shutdown(); }
        }

        [UnityTest]
        public IEnumerator RealDeviceOutsidePlayModeStopsReplaysAndNaturallyEnds()
        {
            Assert.That(EditorApplication.isPlaying, Is.False);
            var catalog = Catalog();
            using (var preview = new HarkenEditorPreview())
            using (var first = preview.CreateScope())
            using (var second = preview.CreateScope())
            {
                Assert.That(first.Prepare(catalog.PreparedPacks, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK), first.Diagnostic);
                Assert.That(second.Prepare(catalog.PreparedPacks, new[] { "Play_U4_B" }), Is.EqualTo(VnResult.VN_OK), second.Diagnostic);
                var session = preview.Session;
                ulong before = Block();
                Assert.That(first.Play("Play_U4_A", out var firing), Is.EqualTo(VnResult.VN_OK), first.Diagnostic);
                yield return FreshBlock(preview, session, before);
                Assert.That(first.Stop(firing, 0), Is.EqualTo(VnResult.VN_OK));
                yield return Idle(preview, 3);
                Assert.That(preview.IsPrepared, Is.True);
                before = Block();
                Assert.That(first.Play("Play_U4_A", out _), Is.EqualTo(VnResult.VN_OK), first.Diagnostic);
                yield return FreshBlock(preview, session, before);
                yield return Idle(preview, 7);
                Assert.That(second.Play("Play_U4_B", out _), Is.EqualTo(VnResult.VN_OK), second.Diagnostic);
                Assert.That(second.StopAll(0), Is.EqualTo(VnResult.VN_OK));
                yield return Idle(preview, 3);
                Assert.That(preview.Shutdown(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.IsActive, Is.False);
                Assert.That(first.IsPrepared, Is.False);
                Assert.That(second.IsPrepared, Is.False);
            }
        }

        [UnityTest]
        public IEnumerator PlayModeAndDomainReloadReleaseEditorOwnership()
        {
            var catalog = Catalog();
            var preview = new HarkenEditorPreview();
            var scope = preview.CreateScope();
            try
            {
                Assert.That(scope.Prepare(catalog.PreparedPacks, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK));
                Assert.That(scope.Play("Play_U4_A", out _), Is.EqualTo(VnResult.VN_OK));
                yield return new EnterPlayMode();
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
            }
            finally { scope?.Dispose(); preview?.Dispose(); }
            yield return new ExitPlayMode();
            catalog = Catalog();
            using (preview = new HarkenEditorPreview())
            using (scope = preview.CreateScope())
            {
                Assert.That(scope.Prepare(catalog.PreparedPacks, new[] { "Play_U4_A" }), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preview.Shutdown(), Is.EqualTo(VnResult.VN_OK));
            }
        }

        private static ulong Block()
        {
            VnSnapshot snapshot = HarkenEngine.DefaultSnapshot();
            Assert.That(HarkenEngine.Capture(ref snapshot), Is.EqualTo(VnResult.VN_OK));
            return snapshot.blockIndex;
        }
        private static IEnumerator FreshBlock(HarkenEditorPreview preview, PackBootstrapSession session, ulong baseline)
        {
            double end = EditorApplication.timeSinceStartup + 3;
            while (EditorApplication.timeSinceStartup < end)
            {
                Assert.That(preview.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                preview.Tick();
                if (Block() > baseline) yield break;
                yield return null;
            }
            Assert.Fail("No fresh real-device block from the same preview session.");
        }
        private static IEnumerator Idle(HarkenEditorPreview preview, double seconds)
        {
            double end = EditorApplication.timeSinceStartup + seconds;
            while (preview.FiringCount != 0 && EditorApplication.timeSinceStartup < end)
            { preview.Tick(); yield return null; }
            Assert.That(preview.FiringCount, Is.Zero, preview.Diagnostic);
        }
    }
}
