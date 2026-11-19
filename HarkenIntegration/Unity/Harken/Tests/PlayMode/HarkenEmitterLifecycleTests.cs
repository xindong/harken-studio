using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenEmitterLifecycleTests
    {
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

        [UnityTest]
        public IEnumerator AttenuationHighpassTailReachesNativeValidation()
        {
            using (var f = new Fixture())
            {
                f.Start();
                Assert.That(HarkenAbi.CheckLayout(), Is.Empty);
                var id = new VnObjectId(0, 0xe30003);
                var desc = new VnAttenuationDesc
                {
                    structSize = (uint)Marshal.SizeOf<VnAttenuationDesc>(),
                    minDistance = 2,
                    maxDistance = 102,
                    airAbsorptionNearHz = 1.0e9f,
                    airAbsorptionFarHz = 1.0e9f
                };
                Assert.That(HarkenNative.Vn_RegisterAttenuation(id, in desc), Is.EqualTo(VnResult.VN_OK));
                desc.highpassCurve.enabled = 1;
                desc.highpassCurve.pointCount = 1;
                Assert.That(HarkenNative.Vn_ReplaceAttenuation(id, in desc), Is.EqualTo(VnResult.VN_OK),
                    "An enabled single zero-Hz point must bypass the distance high-pass contribution.");
                desc.highpassCurve.enabled = 2;
                Assert.That(HarkenNative.Vn_ReplaceAttenuation(id, in desc), Is.EqualTo(VnResult.VN_INVALID_ARG),
                    "The native library must read and validate the generated descriptor tail.");
                Debug.Log($"ATTENUATION_HPF_ABI size={desc.structSize} offset={Marshal.OffsetOf<VnAttenuationDesc>(nameof(VnAttenuationDesc.highpassCurve))} zeroHz=accepted invalidEnabled=rejected");
            }
            yield break;
        }

        [UnityTest]
        public IEnumerator DistanceScaleRejectsInvalidAndRetiredIdentities()
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                f.Camera.SetActive(true);
                Assert.That(f.Runtime.CreateSpatialSource(f.Actor.transform, out HarkenSpatialSource source),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(f.Emitter.SetDistanceScale(2), Is.EqualTo(VnResult.VN_OK));
                Assert.That(source.SetDistanceScale(2), Is.EqualTo(VnResult.VN_OK));
                foreach (float scale in new[] { 0, -1, float.NaN, float.PositiveInfinity, float.NegativeInfinity })
                {
                    LogAssert.Expect(LogType.Error, "Harken operation failed: Emitter.SetDistanceScale: VN_INVALID_ARG");
                    Assert.That(f.Emitter.SetDistanceScale(scale), Is.EqualTo(VnResult.VN_INVALID_ARG));
                    Assert.That(source.SetDistanceScale(scale), Is.EqualTo(VnResult.VN_INVALID_ARG));
                }
                int notifications = 0;
                f.Runtime.OperationCompleted += (_, operation, result) => ++notifications;
                Assert.That(f.Runtime.BeginSpatialPost(f.Runtime.Session, f.Emitter, null, out var publication),
                    Is.EqualTo(VnResult.VN_OK));
                try
                {
                    Assert.That(f.Emitter.SetDistanceScale(3), Is.EqualTo(VnResult.VN_IN_USE));
                    Assert.That(source.SetDistanceScale(3), Is.EqualTo(VnResult.VN_IN_USE));
                    Assert.That(notifications, Is.Zero, "Publication refusal must not reenter result callbacks.");
                }
                finally { f.Runtime.EndSpatialPost(in publication); }
                Assert.That(source.EndFollow(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(source.SetDistanceScale(1), Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(source.Maintain(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(source.SetDistanceScale(1), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(f.Runtime.CreateSpatialSource(f.Actor.transform, out HarkenSpatialSource next),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(next.Validate(f.Runtime, f.Runtime.Session, out Emitter rawSource), Is.EqualTo(VnResult.VN_OK));
                Assert.That(rawSource.Destroy(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(next.SetDistanceScale(1), Is.EqualTo(VnResult.VN_STALE_HANDLE),
                    "A cached managed handle must still pass native generation validation.");
                Assert.That(f.Runtime.CreateSpatialSource(f.Camera.transform, out HarkenSpatialSource sessionSource),
                    Is.EqualTo(VnResult.VN_OK));
                PackBootstrapSession oldSession = f.Runtime.Session;
                Emitter oldComponent = f.Emitter.Handle;
                Assert.That(f.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                f.Start();
                Assert.That(oldSession.IsActive, Is.False);
                Assert.That(f.Runtime.Session, Is.Not.SameAs(oldSession));
                Assert.That(sessionSource.SetDistanceScale(1), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(f.Emitter.Matches(f.Runtime, oldSession, oldComponent), Is.False);
                Assert.That(f.Emitter.SetDistanceScale(1), Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        #if HARKEN_SDK_TEST_NULL_PUMP
        [UnityTest]
        public IEnumerator PostNotificationMayDisableItsSourceWithoutReturningStaleSuccess()
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                f.Camera.SetActive(true);
                f.Actor.transform.position = Vector3.right * 80;
                bool called = false;
                f.Runtime.OperationCompleted += (_, operation, result) =>
                {
                    if (called || operation != "UnitySpatial.SetEmitterTransform") return;
                    called = true;
                    Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                    f.Actor.SetActive(false);
                };
                Assert.That(f.Action.Play(), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                Assert.That(called, Is.True);
                Assert.That(f.Action.Firing.IsValid, Is.False);
                Assert.That(f.Emitter.IsCreated, Is.False);
                Assert.That(f.Runtime.IsReady, Is.True);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator SpatialPostNotificationRestartCannotReturnAnOldFiring()
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                Assert.That(f.Runtime.CreateSpatialSource(f.Actor.transform, out HarkenSpatialSource source),
                    Is.EqualTo(VnResult.VN_OK));
                PackBootstrapSession original = f.Runtime.Session;
                bool called = false;
                f.Runtime.OperationCompleted += (_, operation, result) =>
                {
                    if (called || operation != "SpatialSource.Update") return;
                    called = true;
                    Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(f.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    f.Start();
                };
                Assert.That(f.Runtime.PlayAction(f.Action.ActionName, f.Action.ProjectId, source, out ActionFiring firing),
                    Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                Assert.That(called, Is.True);
                Assert.That(original.IsActive, Is.False);
                Assert.That(firing.IsValid, Is.False);
                Assert.That(f.Runtime.Session, Is.Not.SameAs(original));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ListenerCandidatesRequireExplicitSelectionAfterOwnerDisables()
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Camera.SetActive(true);
                HarkenListener first = f.Camera.GetComponent<HarkenListener>();
                var other = new GameObject("Listener candidate");
                try
                {
                    HarkenListener second = other.AddComponent<HarkenListener>();
                    Assert.That(f.Runtime.SelectedListener, Is.SameAs(first));
                    Assert.That(second.LastResult, Is.EqualTo(VnResult.VN_IN_USE));
                    second.enabled = false;
                    LogAssert.Expect(LogType.Error, "Harken operation failed: HarkenAudioSystem.SelectListener: VN_INVALID_ARG");
                    Assert.That(f.Runtime.SelectListener(second), Is.EqualTo(VnResult.VN_INVALID_ARG));
                    Assert.That(f.Runtime.SelectedListener, Is.SameAs(first));
                    second.enabled = true;
                    Assert.That(f.Runtime.SelectListener(second), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(f.Runtime.SelectedListener, Is.SameAs(second));
                    second.enabled = false;
                    LogAssert.Expect(LogType.Error, "Harken operation failed: HarkenAudioSystem.SelectListener: VN_INVALID_ARG");
                    Assert.That(f.Runtime.SelectListener(second), Is.EqualTo(VnResult.VN_INVALID_ARG));
                    yield return null;
                    Assert.That(f.Runtime.SelectedListener, Is.Null);
                    Assert.That(f.Runtime.SelectListener(first), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(f.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(f.Runtime.SelectedListener, Is.Null);
                    f.Start();
                    Assert.That(f.Runtime.SelectedListener, Is.SameAs(first));
                }
                finally { Object.DestroyImmediate(other); }
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
                Assert.That(CompanionTestPublication.Read(root, null, out CompanionTestPublication publication, out string error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction("Play_U4_A", out CompanionTestAction selected), Is.True);
                Set(Action, "actionName", selected.Name);
                Set(Action, "projectId", publication.ProjectId);
                Assert.That(Runtime.ConfigureBeforeStartup("Audio/SpatialPacks", publication.InitPack.Path, Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Runtime.ConfigurePlatformBeforeStartup(null), Is.EqualTo(VnResult.VN_OK));
                Owner.SetActive(true);
            }

            public Dictionary<string, byte[]> CompanionBytes()
            {
                byte[] init = File.ReadAllBytes(Path.Combine(root, "Init.bytes"));
                string version = PackCompanion.SupportedPackMetadataVersion.ToString(CultureInfo.InvariantCulture);
                Assert.That(CompanionTestPublication.Read(root, null, out CompanionTestPublication publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction(Action.ActionName, out CompanionTestAction action), Is.True);
                Assert.That(publication.TryFindPack(action.OwnerPackStableId, out CompanionTestPack pack), Is.True);
                return new Dictionary<string, byte[]>
                {
                    ["Init.bytes"] = init,
                    ["Init.json"] = Encoding.UTF8.GetBytes("{\"packMetadataVersion\":" + version + ",\"variables\":[]}"),
                    ["Play_U4_A.bytes"] = File.ReadAllBytes(Path.Combine(root, pack.Path)),
                    ["Play_U4_A.json"] = Encoding.UTF8.GetBytes("{\"packMetadataVersion\":" + version +
                        ",\"name\":\"" + action.Name + "\",\"guid\":\"" + action.ActionStableId + "\"}")
                };
            }

            public void Start()
            {
                Assert.That(Runtime.StartRuntime(root), Is.EqualTo(VnResult.VN_OK), Runtime.LastStartupError);
                Assert.That(Runtime.IsReady, Is.True);
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
