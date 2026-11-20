using System;
using System.IO;
using Harken;
using Harken.Unity.Editor;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace HarkenCrossProduct
{
    public sealed class HarkenCrossProductEditModeTests
    {
        [Serializable]
        private sealed class Inputs
        {
            public string projectId;
            public string main;
            public string toneAction;
        }

        private const int TickLimit = 400;

        [Test]
        public void RefusedPreviewLeavesExternalPlaybackActive()
        {
            LogAssert.ignoreFailingMessages = true;
            var inputs = JsonUtility.FromJson<Inputs>(File.ReadAllText(
                Path.Combine(Application.streamingAssetsPath, "HarkenCrossProduct", "inputs.json")));
            string directory = Path.Combine(Application.streamingAssetsPath, inputs.main);
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            Assert.That(PackBootstrap.StartDirectory(directory, null, ref config, out PackBootstrapSession external),
                Is.EqualTo(VnResult.VN_OK));
            using (external)
            {
                Assert.That(external.PrepareActions(new[] { inputs.toneAction }, external.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Resolve(inputs.toneAction, external.ProjectId, out ActionResolved action), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post(in action, Emitter.None, 0, out ActionFiring retained), Is.EqualTo(VnResult.VN_OK));
                using (var preview = new HarkenEditorPreview())
                {
                    Assert.That(preview.Prepare(directory, null, Array.Empty<ulong>(), true), Is.EqualTo(VnResult.VN_ALREADY_INITIALIZED));
                    Assert.That(preview.Shutdown(), Is.EqualTo(VnResult.VN_OK));
                }
                Assert.That(external.IsActive, Is.True);
                Assert.That(retained.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            }
        }

        [Test]
        public void EditorPreviewPlaysAndStops()
        {
            LogAssert.ignoreFailingMessages = true;
            var inputs = JsonUtility.FromJson<Inputs>(File.ReadAllText(
                Path.Combine(Application.streamingAssetsPath, "HarkenCrossProduct", "inputs.json")));
            using (var preview = new HarkenEditorPreview())
            {
                Assert.That(preview.Prepare(Path.Combine(Application.streamingAssetsPath, inputs.main), null,
                    Array.Empty<ulong>(), true), Is.EqualTo(VnResult.VN_OK), preview.Diagnostic);
                Assert.That(preview.Session.PrepareActions(new[] { inputs.toneAction }, inputs.projectId,
                    out ActionPreparation preparation), Is.EqualTo(VnResult.VN_OK));
                try
                {
                    Assert.That(preview.Play(inputs.toneAction, inputs.projectId, out ActionFiring firing),
                        Is.EqualTo(VnResult.VN_OK), preview.Diagnostic);
                    Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(preview.FiringCount, Is.GreaterThan(0));
                    Assert.That(preview.Stop(firing, 0), Is.EqualTo(VnResult.VN_OK));
                    for (int i = 0; i < TickLimit && preview.FiringCount > 0; ++i)
                    {
                        DevelopmentMeter.Advance(1);
                        preview.Tick();
                    }
                    Assert.That(preview.FiringCount, Is.Zero, "stopped preview firings are released");
                }
                finally
                {
                    preparation.Release();
                }
                Assert.That(preview.Shutdown(), Is.EqualTo(VnResult.VN_OK));
            }
        }
    }
}
