#if !HARKEN_SDK_TEST_NULL_PUMP
using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    [Category("ProductionDevice")]
    [UnityPlatform(RuntimePlatform.WindowsPlayer)]
    public sealed class HarkenProductionPlayerTests
    {
        private static string Root => Path.Combine(Application.streamingAssetsPath, "Audio", "CompanionPacks");

        internal static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);

        internal static VnSnapshot Capture()
        {
            var snapshot = HarkenEngine.DefaultSnapshot();
            Assert.That(HarkenEngine.Capture(ref snapshot), Is.EqualTo(VnResult.VN_OK));
            return snapshot;
        }

        internal static unsafe float Peak(VnSnapshot snapshot)
        {
            float peak = 0;
            for (int i = 0; i < Math.Min(32, snapshot.busCount); ++i)
                peak = Math.Max(peak, snapshot.busPeaks[i]);
            return peak;
        }

        internal static IEnumerator FreshOutput(VnSnapshot before, string phase)
        {
            float deadline = Time.realtimeSinceStartup + 15;
            while (Time.realtimeSinceStartup < deadline)
            {
                yield return null;
                var now = Capture();
                if (now.notificationDropCount != before.notificationDropCount ||
                    now.logicNotificationDropCount != before.logicNotificationDropCount)
                    Assert.Inconclusive("Notification capture was incomplete during " + phase);
                if (now.blockIndex > before.blockIndex && Peak(now) > 0.0001f)
                {
                    Debug.Log($"SDK_REAL_OUTPUT phase={phase} before={before.blockIndex} block={now.blockIndex} peak={Peak(now):R} voices={now.activeVoiceCount}");
                    yield break;
                }
            }
            Assert.Fail("No fresh nonzero production-device block was captured during " + phase);
        }

        internal static IEnumerator FreshSilence(VnSnapshot before)
        {
            float deadline = Time.realtimeSinceStartup + 15;
            while (Time.realtimeSinceStartup < deadline)
            {
                yield return null;
                var now = Capture();
                if (now.blockIndex > before.blockIndex && now.activeVoiceCount == 0 && Peak(now) < 0.000001f)
                {
                    Debug.Log($"SDK_REAL_STOP before={before.blockIndex} block={now.blockIndex} voices={now.activeVoiceCount}");
                    yield break;
                }
            }
            Assert.Fail("Production-device playback did not return to silence after stopping.");
        }

        private static IEnumerator Completed(Task task)
        {
            double deadline = Time.realtimeSinceStartupAsDouble + 15;
            while (!task.IsCompleted && Time.realtimeSinceStartupAsDouble < deadline) yield return null;
            Assert.That(task.IsCompleted, Is.True, "The deferred operation did not reach a terminal state.");
        }

        [UnityTest, Timeout(60000)]
        public IEnumerator DeferredReadsCancelWithoutAffectingTheNewSessionOrOtherCaller()
        {
            using var settingsLease = HarkenProjectSettingsLease.Acquire();
            byte[] Read(string path)
            {
                string file = Path.Combine(Root, path);
                return File.Exists(file) ? File.ReadAllBytes(file) : null;
            }
            var owner = new GameObject("SDK deferred production reads");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            Set(runtime, "autoInitialize", false);
            Assert.That(runtime.ConfigureBeforeStartup("Audio/CompanionPacks", "Init.bytes",
                Array.Empty<ulong>(), false, false), Is.EqualTo(VnResult.VN_OK));
            owner.SetActive(true);
            try
            {
                var oldInit = new TaskCompletionSource<byte[]>();
                var newInit = new TaskCompletionSource<byte[]>();
                var content = new TaskCompletionSource<byte[]>();
                var old = runtime.StartRuntimeAsync(PackSource.FromAsyncReader((path, token) =>
                    path == "Init.bytes" ? oldInit.Task : Task.FromResult(Read(path))));
                owner.SetActive(false);
                owner.SetActive(true);
                int reads = 0;
                var current = runtime.StartRuntimeAsync(PackSource.FromAsyncReader((path, token) =>
                {
                    if (path == "Init.bytes") return newInit.Task;
                    if (path == "Play_Head.bytes") { ++reads; return content.Task; }
                    return Task.FromResult(Read(path));
                }));
                oldInit.SetResult(Read("Init.bytes"));
                yield return Completed(old);
                Assert.That(old.IsCanceled, Is.True);
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Starting));
                Assert.That(current.IsCompleted, Is.False);
                newInit.SetResult(Read("Init.bytes"));
                yield return Completed(current);
                Assert.That(current.GetAwaiter().GetResult(), Is.EqualTo(VnResult.VN_OK));
                var session = runtime.Session;
                using var cancellation = new CancellationTokenSource();
                var cancelled = runtime.PlayActionAsync("Play_Head", session.ProjectId, cancellation.Token);
                var retained = runtime.PlayActionAsync("Play_Head", session.ProjectId);
                Assert.That(reads, Is.EqualTo(1));
                Assert.That(cancelled.IsCompleted || retained.IsCompleted, Is.False);
                cancellation.Cancel();
                yield return Completed(cancelled);
                Assert.That(cancelled.IsCanceled, Is.True);
                Assert.That(retained.IsCompleted, Is.False);
                var before = Capture();
                Assert.That(before.activeVoiceCount, Is.Zero);
                content.SetResult(Read("Play_Head.bytes"));
                yield return Completed(retained);
                var result = retained.GetAwaiter().GetResult();
                Assert.That(result.Result, Is.EqualTo(VnResult.VN_OK));
                yield return FreshOutput(before, "retained-deferred-caller");
                var stopped = result.Firing.Stop(0);
                Assert.That(stopped == VnResult.VN_OK || stopped == VnResult.VN_STALE_HANDLE, Is.True);
                yield return FreshSilence(Capture());
                Assert.That(runtime.Session, Is.SameAs(session));
                Assert.That(reads, Is.EqualTo(1));
                Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.IsActive, Is.False);
            }
            finally
            {
                if (runtime && runtime.Session != null && runtime.Session.IsActive) runtime.ShutdownRuntime();
                Object.Destroy(owner);
            }
            yield return null;
        }

        [UnityTest, Timeout(60000)]
        public IEnumerator ProductionDeviceColdWarmStopAndExplicitRestart()
        {
            using var settingsLease = HarkenProjectSettingsLease.Acquire();
            Assert.That(File.Exists(Path.Combine(Root, "Init.bytes")), Is.True);
            var owner = new GameObject("SDK production Player");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            Set(runtime, "autoInitialize", false);
            Assert.That(runtime.ConfigureBeforeStartup("Audio/CompanionPacks", "Init.bytes",
                Array.Empty<ulong>(), false, false), Is.EqualTo(VnResult.VN_OK));
            owner.SetActive(true);
            try
            {
                for (int cycle = 0; cycle < 2; ++cycle)
                {
                    int reads = 0;
                    var source = PackSource.FromReader(path =>
                    {
                        ++reads;
                        string file = Path.Combine(Root, path);
                        return File.Exists(file) ? File.ReadAllBytes(file) : null;
                    });
                    Assert.That(runtime.StartRuntime(source), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(runtime.IsReady, Is.True);
                    var session = runtime.Session;
                    Assert.That(Actions.Resolve("Play_Head", out _), Is.Not.EqualTo(VnResult.VN_OK));
                    for (int i = 0; i < 3; ++i) yield return null;
                    var beforeCold = Capture();
                    Assert.That(runtime.PlayAction("Play_Head", session.ProjectId, out ActionFiring cold), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(cold.Query(out VnActionFiringInfo coldInfo), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(coldInfo.acceptedLogicSequence, Is.GreaterThan(0));
                    int coldReads = reads;
                    Assert.That(session.PrepareActions(new[] { "Play_Head" }, session.ProjectId, out ActionPreparation retained),
                        Is.EqualTo(VnResult.VN_OK));
                    Assert.That(reads, Is.EqualTo(coldReads));
                    yield return FreshOutput(beforeCold, "cold-" + cycle);
                    var coldStop = cold.Stop(0);
                    Assert.That(coldStop == VnResult.VN_OK || coldStop == VnResult.VN_STALE_HANDLE, Is.True);
                    yield return FreshSilence(Capture());
                    int preparedReads = reads;
                    var beforeWarm = Capture();
                    Assert.That(runtime.PlayAction("Play_Head", session.ProjectId, out ActionFiring warm), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(reads, Is.EqualTo(preparedReads), "Warm playback must not call the source reader again.");
                    yield return FreshOutput(beforeWarm, "warm-" + cycle);
                    var beforeStop = Capture();
                    var stopped = warm.Stop(0);
                    if (stopped == VnResult.VN_STALE_HANDLE)
                        Assert.Inconclusive("The short fixture ended before the explicit stop could be exercised.");
                    Assert.That(stopped, Is.EqualTo(VnResult.VN_OK));
                    yield return FreshSilence(beforeStop);
                    Assert.That(retained.Release(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(session.IsActive, Is.False);
                    Assert.That(runtime.Session, Is.Null);
                    Debug.Log($"SDK_REAL_SESSION cycle={cycle} reads={reads} shutdown=ok");
                    yield return null;
                }
            }
            finally
            {
                if (runtime && runtime.Session != null && runtime.Session.IsActive) runtime.ShutdownRuntime();
                Object.Destroy(owner);
            }
            yield return null;
        }
    }
}
#endif
