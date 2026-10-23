using System;
using System.Collections.Generic;
using System.IO;
using NUnit.Framework;
using UnityEngine;

namespace Harken.Unity.Tests
{
    public sealed class HarkenReaderCaptureTests
    {
        [Test]
        public void ReaderCapturesInitThenDemandsOnlyTheRequestedActionPair()
        {
            string root = Path.Combine(Application.streamingAssetsPath, "Audio", "U4Original");
            var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
            foreach (string path in Directory.GetFiles(root))
                if (path.EndsWith(".bytes", StringComparison.Ordinal) || path.EndsWith(".json", StringComparison.Ordinal))
                    files.Add(Path.GetFileName(path), File.ReadAllBytes(path));
            var requested = new List<string>();
            PackSource source = PackSource.FromReader(path =>
            {
                requested.Add(path);
                return files.TryGetValue(path, out byte[] bytes) ? bytes : null;
            });
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            PackBootstrapSession session = null;
            try
            {
                Assert.That(PackBootstrap.Start(source, ref config, out session), Is.EqualTo(VnResult.VN_OK));
                Assert.That(requested, Is.EquivalentTo(new[] { "Init.bytes", "Init.json" }));
                requested.Clear();
                Assert.That(session.PrepareActions(new[] { "Play_U4_A" }, session.ProjectId, out var prepared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(requested, Is.EquivalentTo(new[] { "Play_U4_A.bytes", "Play_U4_A.json" }));
                Array.Clear(files["Init.bytes"], 0, files["Init.bytes"].Length);
                Array.Clear(files["Play_U4_A.bytes"], 0, files["Play_U4_A.bytes"].Length);
                Assert.That(Actions.Resolve("Play_U4_A", session.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(prepared.Release(), Is.EqualTo(VnResult.VN_OK));
            }
            finally { session?.Dispose(); }
        }

        [Test]
        public void RetiredPublicationCaptureDoesNotInvokeTheReaderOrReturnPartialState()
        {
            int reads = 0;
            Assert.That(HarkenPreparedPacks.Capture("HarkenPackManifest.json", path =>
            {
                ++reads;
                throw new IOException(path);
            }, out var prepared, out var manifest, out _), Is.EqualTo(VnResult.VN_PACK_INCOMPATIBLE));
            Assert.That(reads, Is.Zero);
            Assert.That(prepared, Is.Null);
            Assert.That(manifest, Is.Null);
        }
    }
}
