using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenEmitterLifecycleTests
    {
        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpAndCopy(
            [Out] float[] samples, uint capacity, out uint frames, out uint channels);

        [UnityTest]
        public IEnumerator DisabledBeforeReadyDoesNotCreateEmitter() => ReadyCase("before");

        [UnityTest]
        public IEnumerator DisabledAfterItsReadyHandlerReleasesEmitter() => ReadyCase("after");

        [UnityTest]
        public IEnumerator DisabledByEarlierReadyHandlerMustNotCreateEmitter() => ReadyCase("during");

        [UnityTest]
        public IEnumerator DestroyedByEarlierReadyHandlerMustNotCreateEmitter() => ReadyCase("destroy");

        private static IEnumerator ReadyCase(string timing)
        {
            using (var f = new Fixture())
            {
                JsonUtility.FromJsonOverwrite("{\"maxEmitters\":1}", HarkenProjectSettings.ActiveForStartup());
                if (timing == "during") f.Runtime.BecameReady += _ => f.Emitter.enabled = false;
                if (timing == "destroy") f.Runtime.BecameReady += _ => Object.DestroyImmediate(f.Emitter);
                f.Actor.SetActive(true);
                if (timing == "before") f.Emitter.enabled = false;
                if (timing == "after") f.Runtime.BecameReady += _ => f.Emitter.enabled = false;
                f.Start();
                yield return null;
                yield return null;
                if (timing == "destroy") Assert.That(f.Emitter == null, Is.True);
                else
                {
                    Assert.That(f.Emitter.enabled, Is.False);
                    Assert.That(f.Emitter.IsCreated, Is.False);
                }
                // The only native slot must still be available, even if the component is gone.
                Assert.That(Emitter.Create(out Emitter probe), Is.EqualTo(VnResult.VN_OK));
                Assert.That(probe.Destroy(), Is.EqualTo(VnResult.VN_OK));
            }
        }

        #if HARKEN_SDK_TEST_NULL_PUMP
        [UnityTest]
        public IEnumerator OrdinaryEmitterMovementReachesNativeAudio() => EmitterPoseCase("ordinary");

        [UnityTest]
        public IEnumerator ReadOnlyResultObserverPreservesEmitterMovement() => EmitterPoseCase("read-only");

        [UnityTest]
        public IEnumerator EmitterMovedInsideResultCallbackMustReachNativeAudio() => EmitterPoseCase("move-in-callback");

        private static IEnumerator EmitterPoseCase(string mode)
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                Assert.That(UnitySpatial.SetListener(Vector3.zero, Vector3.forward, Vector3.up), Is.EqualTo(VnResult.VN_OK));
                Assert.That(f.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                f.RenderFresh();
                bool observed = false;
                f.Runtime.OperationCompleted += (_, operation, result) =>
                {
                    if (observed || operation != "UnitySpatial.SetEmitterTransform" || result != VnResult.VN_OK) return;
                    observed = true;
                    if (mode == "move-in-callback") f.Actor.transform.position = Vector3.right * 80;
                };
                f.Actor.transform.position = (mode == "move-in-callback" ? Vector3.left : Vector3.right) * 80;
                for (int i = 0; i < 4; ++i) yield return null;
                Vector2 energy = f.RenderFresh();
                Debug.Log($"EMITTER_AUDIT pose={mode} unityX={f.Actor.transform.position.x} left={energy.x:R} right={energy.y:R} observed={observed}");
                Assert.That(observed, Is.True);
                Assert.That(f.Actor.transform.position.x, Is.EqualTo(80));
                Assert.That(energy.y, Is.GreaterThan(0.001f));
                Assert.That(energy.y, Is.GreaterThan(energy.x * 3), "The final Transform is on the right; newly rendered audio must follow it.");
            }
        }

        [UnityTest]
        public IEnumerator OrdinaryListenerMovementReachesNativeAudio() => ListenerPoseCase(false);

        [UnityTest]
        public IEnumerator ListenerMovedInsideResultCallbackMustReachNativeAudio() => ListenerPoseCase(true);

        private static IEnumerator ListenerPoseCase(bool moveInCallback)
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                f.Camera.SetActive(true);
                Assert.That(f.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                yield return null;
                f.RenderFresh();
                bool observed = false;
                f.Runtime.OperationCompleted += (_, operation, result) =>
                {
                    if (observed || operation != "UnitySpatial.SetListener" || result != VnResult.VN_OK) return;
                    observed = true;
                    if (moveInCallback) f.Camera.transform.position = Vector3.right * 80;
                };
                f.Camera.transform.position = (moveInCallback ? Vector3.left : Vector3.right) * 80;
                for (int i = 0; i < 4; ++i) yield return null;
                Vector2 energy = f.RenderFresh();
                Debug.Log($"EMITTER_AUDIT listenerCallback={moveInCallback} unityX={f.Camera.transform.position.x} left={energy.x:R} right={energy.y:R} observed={observed}");
                Assert.That(observed, Is.True);
                Assert.That(f.Camera.transform.position.x, Is.EqualTo(80));
                Assert.That(energy.x, Is.GreaterThan(0.001f));
                Assert.That(energy.x, Is.GreaterThan(energy.y * 3), "The listener is right of the source; newly rendered audio must be on the left.");
            }
        }

        [UnityTest]
        public IEnumerator StopAuditNoStopPreservesAudio() => StopAuditCase("none");

        [UnityTest]
        public IEnumerator StopAuditActionStopSilencesAudio() => StopAuditCase("action");

        [UnityTest]
        public IEnumerator StopAuditEmitterStopSilencesActionAudio() => StopAuditCase("emitter");

        [UnityTest]
        public IEnumerator StopAuditEmitterDisableSilencesActionAudio() => StopAuditCase("disable");

        private static IEnumerator StopAuditCase(string mode)
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                typeof(HarkenEmitter).GetField("stopFadeMs", BindingFlags.Instance | BindingFlags.NonPublic)
                    .SetValue(f.Emitter, 0U);
                Assert.That(UnitySpatial.SetListener(Vector3.zero, Vector3.forward, Vector3.up), Is.EqualTo(VnResult.VN_OK));
                Assert.That(f.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                Vector2 before = f.RenderFresh();
                Assert.That(before.x + before.y, Is.GreaterThan(0.001f));
                Emitter originalHandle = f.Emitter.Handle;
                if (mode == "action") Assert.That(f.Action.Stop(), Is.EqualTo(VnResult.VN_OK));
                if (mode == "emitter") Assert.That(f.Emitter.Stop(), Is.EqualTo(VnResult.VN_OK));
                if (mode == "disable") f.Emitter.enabled = false;
                yield return null;
                Vector2 after = f.RenderFresh();
                VnResult originalHandleResult = originalHandle.SetPosition(default);
                Debug.Log($"EMITTER_STOP_AUDIT mode={mode} beforeL={before.x:R} beforeR={before.y:R} afterL={after.x:R} afterR={after.y:R} created={f.Emitter.IsCreated} originalHandle={originalHandleResult}");
                if (mode == "disable")
                {
                    Assert.That(f.Emitter.IsCreated, Is.False);
                    Assert.That(originalHandleResult, Is.EqualTo(VnResult.VN_STALE_HANDLE));
                }
                if (mode == "none") Assert.That(after.x + after.y, Is.GreaterThan(0.001f));
                else Assert.That(after.x + after.y, Is.LessThan(0.000001f), "Accepted stop or component cleanup must silence the already-playing Action.");
            }
        }

        [UnityTest]
        public IEnumerator EmitterDestroyedInsideResultCallbackReleasesItsHandle() => DestroyInPoseCallback(false);

        [UnityTest]
        public IEnumerator ListenerDestroyedInsideResultCallbackDoesNotAccessItsTransform() => DestroyInPoseCallback(true);

        private static IEnumerator DestroyInPoseCallback(bool listener)
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                f.Camera.SetActive(true);
                yield return null;
                Emitter original = f.Emitter.Handle;
                GameObject target = listener ? f.Camera : f.Actor;
                string expected = listener ? "UnitySpatial.SetListener" : "UnitySpatial.SetEmitterTransform";
                bool called = false;
                f.Runtime.OperationCompleted += (_, operation, result) =>
                {
                    if (called || operation != expected) return;
                    Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                    called = true;
                    Object.DestroyImmediate(target);
                };
                target.transform.position = Vector3.right * 80;
                yield return null;
                Assert.That(called, Is.True);
                Assert.That(target == null, Is.True);
                if (!listener) Assert.That(original.SetPosition(default), Is.EqualTo(VnResult.VN_STALE_HANDLE));
            }
        }
        #endif

        private sealed class Fixture : IDisposable
        {
            public readonly GameObject Owner = new GameObject("Emitter lifecycle Runtime");
            public readonly GameObject Actor = new GameObject("Emitter lifecycle source");
            public readonly GameObject Camera = new GameObject("Emitter lifecycle listener");
            public readonly HarkenAudioSystem Runtime;
            public readonly HarkenEmitter Emitter;
            public readonly HarkenAction Action;
            private readonly string root = Path.Combine(Application.streamingAssetsPath, "Audio/SpatialPacks");
            private HarkenProjectSettingsLease lease;
            private readonly float[] samples = new float[8192];

            public Fixture()
            {
                lease = HarkenProjectSettingsLease.Acquire();
                Owner.SetActive(false);
                Actor.SetActive(false);
                Camera.SetActive(false);
                Runtime = Owner.AddComponent<HarkenAudioSystem>();
                Set(Runtime, "autoInitialize", false);
                Set(Runtime, "persistAcrossScenes", false);
                Emitter = Actor.AddComponent<HarkenEmitter>();
                Action = Actor.AddComponent<HarkenAction>();
                Set(Emitter, "runtime", Runtime);
                Set(Action, "runtime", Runtime);
                Set(Action, "emitter", Emitter);
                Set(Action, "playOnEnable", false);
                Set(Action, "stopFadeMs", 0U);
                var listener = Camera.AddComponent<HarkenListener>();
                Set(listener, "runtime", Runtime);
                Assert.That(GameplayPublication.Read(root, null, out GameplayPublication publication, out string error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction("Play_U4_A", out GameplayAction selected), Is.True);
                Set(Action, "actionName", selected.Name);
                Set(Action, "projectId", publication.ProjectId);
                Assert.That(Runtime.ConfigureBeforeStartup("Audio/SpatialPacks", "Init.pack", new[] { selected.OwnerPackStableId }, true, false), Is.EqualTo(VnResult.VN_OK));
                Owner.SetActive(true);
            }

            public void Start()
            {
                Assert.That(Runtime.StartRuntime(root), Is.EqualTo(VnResult.VN_OK), Runtime.LastStartupError);
                Assert.That(Runtime.IsReady, Is.True);
            }

            public Vector2 RenderFresh()
            {
                PackBootstrapSession session = Runtime.Session;
                var before = HarkenEngine.DefaultSnapshot();
                Assert.That(HarkenEngine.Capture(ref before), Is.EqualTo(VnResult.VN_OK));
                Vector2 energy = default;
                for (int block = 0; block < 8; ++block)
                {
                    Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length, out uint frames, out uint channels), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(channels, Is.EqualTo(2));
                    Assert.That(frames, Is.GreaterThan(0));
                    if (block < 4) continue;
                    for (int frame = 0; frame < frames; ++frame)
                    {
                        energy.x += samples[frame * 2] * samples[frame * 2];
                        energy.y += samples[frame * 2 + 1] * samples[frame * 2 + 1];
                    }
                }
                var after = HarkenEngine.DefaultSnapshot();
                Assert.That(HarkenEngine.Capture(ref after), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Runtime.Session, Is.SameAs(session));
                Assert.That(after.blockIndex, Is.GreaterThan(before.blockIndex));
                return new Vector2(Mathf.Sqrt(energy.x), Mathf.Sqrt(energy.y));
            }

            public void Dispose()
            {
                Runtime.ShutdownRuntime();
                Object.DestroyImmediate(Actor);
                Object.DestroyImmediate(Camera);
                Object.DestroyImmediate(Owner);
                lease.Dispose();
            }

            private static void Set(object target, string name, object value)
                => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);
        }
    }
}
