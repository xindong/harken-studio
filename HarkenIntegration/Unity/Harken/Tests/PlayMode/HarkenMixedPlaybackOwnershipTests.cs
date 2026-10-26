#if !HARKEN_SDK_TEST_NULL_PUMP
using System;
using System.Collections;
using System.IO;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;
using static Harken.Unity.Tests.HarkenProductionPlayerTests;

namespace Harken.Unity.Tests
{
    [Category("ProductionDevice")]
    [UnityPlatform(RuntimePlatform.WindowsPlayer)]
    public sealed class HarkenMixedPlaybackOwnershipTests
    {
        private static IEnumerator Until(Func<bool> complete, string label)
        {
            double deadline = Time.realtimeSinceStartupAsDouble + 15;
            while (!complete() && Time.realtimeSinceStartupAsDouble < deadline) yield return null;
            Assert.That(complete(), Is.True, label);
        }

        private static ActionFiring PlayingLeaf(ActionFiring root)
        {
            for (int depth = 0; depth < 8; ++depth)
            {
                Assert.That(root.Query(out var info), Is.EqualTo(VnResult.VN_OK));
                if (info.livePlayableCount != 0) return root;
                Assert.That(root.QueryStep(0, out var step), Is.EqualTo(VnResult.VN_OK));
                Assert.That(step.childFiring, Is.Not.Zero);
                root = new ActionFiring(step.childFiring);
            }
            Assert.Fail("The fixture did not reach its playing child.");
            return default;
        }

        [UnityTest, Timeout(60000)]
        public IEnumerator NestedAndDirectPlaybackStopIndependentlyAndReuseOneSession()
        {
            string root = Path.Combine(Application.streamingAssetsPath, "Audio", "U4Original");
            Assert.That(File.Exists(Path.Combine(root, "Init.bytes")), Is.True);
            using (HarkenProjectSettingsLease.Acquire())
            {
                var owner = new GameObject("SDK mixed ownership");
                var nestedActor = new GameObject("SDK nested source");
                var directActor = new GameObject("SDK direct source");
                owner.SetActive(false);
                var runtime = owner.AddComponent<HarkenAudioSystem>();
                Set(runtime, "autoInitialize", false);
                Assert.That(runtime.ConfigureBeforeStartup("Audio/U4Original", "Init.bytes",
                    Array.Empty<ulong>(), false, false), Is.EqualTo(VnResult.VN_OK));
                owner.SetActive(true);
                try
                {
                    Assert.That(runtime.StartRuntime(root), Is.EqualTo(VnResult.VN_OK));
                    var session = runtime.Session;
                    var baseline = Capture();
                    for (int cycle = 0; cycle < 3; ++cycle)
                    {
                        Assert.That(runtime.CreateSpatialSource(nestedActor.transform, out var nestedSource), Is.EqualTo(VnResult.VN_OK));
                        Assert.That(runtime.CreateSpatialSource(directActor.transform, out var directSource), Is.EqualTo(VnResult.VN_OK));
                        var before = Capture();
                        Assert.That(runtime.PlayAction("Play_U4_Caller", session.ProjectId, nestedSource, out var nested), Is.EqualTo(VnResult.VN_OK));
                        Assert.That(runtime.PlayAction("Play_U4_A", session.ProjectId, directSource, out var direct), Is.EqualTo(VnResult.VN_OK));
                        yield return Until(() =>
                        {
                            var current = Capture();
                            return current.blockIndex > before.blockIndex && current.activeVoiceCount >= 2 && Peak(current) > 0.0001f;
                        }, "Nested and direct voices must overlap on a fresh rendered block.");
                        Assert.That(nested.Query(out var nestedInfo), Is.EqualTo(VnResult.VN_OK));
                        Assert.That(nestedInfo.childFiringCount, Is.GreaterThan(0));
                        var nestedLeaf = PlayingLeaf(nested);
                        Assert.That(nestedLeaf.Handle, Is.Not.EqualTo(nested.Handle));
                        Assert.That(direct.Query(out var directInfo), Is.EqualTo(VnResult.VN_OK));
                        Assert.That(directInfo.livePlayableCount, Is.GreaterThan(0));
                        Assert.That(nestedSource.EndFollow(), Is.EqualTo(VnResult.VN_OK));
                        Assert.That(nestedSource.IsReleased, Is.False);
                        var beforeNestedStop = Capture();
                        Assert.That(nested.Stop(0), Is.EqualTo(VnResult.VN_OK));
                        yield return Until(() => nestedSource.IsReleased, "Stopped nested source ownership must retire.");
                        Assert.That(direct.Query(out directInfo), Is.EqualTo(VnResult.VN_OK));
                        Assert.That(directInfo.livePlayableCount, Is.GreaterThan(0), "Stopping the nested root must preserve independent playback.");
                        yield return FreshOutput(beforeNestedStop, "direct-survives-nested-stop-" + cycle);
                        Assert.That(directSource.EndFollow(), Is.EqualTo(VnResult.VN_OK));
                        var beforeDirectStop = Capture();
                        Assert.That(direct.Stop(0), Is.EqualTo(VnResult.VN_OK));
                        yield return FreshSilence(beforeDirectStop);
                        yield return Until(() => directSource.IsReleased &&
                            nested.Query(out _) == VnResult.VN_STALE_HANDLE &&
                            direct.Query(out _) == VnResult.VN_STALE_HANDLE,
                            "Both managed firings and their spatial sources must release.");
                        yield return Until(() => Capture().pinnedGenerationCount == baseline.pinnedGenerationCount,
                            "Released playback must return pinned generations to the initialized baseline.");
                        var after = Capture();
                        Assert.That(after.blockIndex, Is.GreaterThan(beforeDirectStop.blockIndex));
                        Assert.That(after.pendingStartCount, Is.Zero);
                        Assert.That(after.notificationDropCount, Is.EqualTo(baseline.notificationDropCount));
                        Assert.That(after.logicNotificationDropCount, Is.EqualTo(baseline.logicNotificationDropCount));
                        Assert.That(runtime.Session, Is.SameAs(session));
                        Debug.Log($"SDK_MIXED_OWNERSHIP cycle={cycle} nested={nested.Handle} nestedLeaf={nestedLeaf.Handle} direct={direct.Handle} block={after.blockIndex} voices={after.activeVoiceCount} pinned={after.pinnedGenerationCount} sameSession=true");
                    }
                    Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(session.IsActive, Is.False);
                }
                finally
                {
                    if (runtime && runtime.Session != null && runtime.Session.IsActive) runtime.ShutdownRuntime();
                    Object.DestroyImmediate(nestedActor);
                    Object.DestroyImmediate(directActor);
                    Object.DestroyImmediate(owner);
                }
            }
        }
    }
}
#endif
