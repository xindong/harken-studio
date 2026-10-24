using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Profiling;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenActionOwnershipTests
    {
        [UnityTest]
        public IEnumerator SessionPackReplacementRefreshesWithoutAutomaticReplay()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                PackBootstrapSession session = fixture.Runtime.Session;
                Assert.That(CompanionTestPublication.Read(fixture.Root, null, out CompanionTestPublication publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction("Play_U4_B", out CompanionTestAction other), Is.True);
                Assert.That(session.PrepareActions(new[] { other.Name }, publication.ProjectId, out ActionPreparation otherPreparation), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.AcquirePack(other.OwnerPackStableId, out Pack extra), Is.EqualTo(VnResult.VN_OK));
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                ActionFiring first = fixture.Action.Firing;
                ulong baseline = CaptureBlock();
                Assert.That(fixture.Action.Stop(), Is.EqualTo(VnResult.VN_OK));
                yield return WaitForRelease(fixture, first, baseline);
                ulong seed = Get<ulong>(fixture.Action, "callerSeed");
                ulong version = Packs.ChangeVersion;
                ulong revision = Get<ulong>(session, "SetRevision");
                ulong beforeMutation = CaptureBlock();
                Assert.That(otherPreparation.Release(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.ReleasePack(other.OwnerPackStableId, extra), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Packs.ChangeVersion, Is.EqualTo(version + 1));
                Assert.That(Get<ulong>(session, "SetRevision"), Is.EqualTo(revision + 1));
                Assert.That(fixture.Runtime.Session, Is.SameAs(session));
                Assert.That(session.IsStartupComplete, Is.True);
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(Get<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(seed));
                Assert.That(fixture.Action.Firing.IsValid, Is.False);
                yield return WaitForNewBlock(fixture, beforeMutation);
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Get<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(seed + 1));
                Assert.That(Get<ulong>(fixture.Action, "cachedPackVersion"), Is.EqualTo(Packs.ChangeVersion));
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.IsActive, Is.False);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DisabledEmitterDuringSuspensionDoesNotDestroyItsReplacement()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start(true);
                var component = fixture.Source.GetComponent<HarkenEmitter>();
                Emitter retained = component.Handle;
                Assert.That(fixture.Runtime.SetSuspended(true), Is.EqualTo(VnResult.VN_OK));
                component.enabled = false;
                Assert.That(component.IsCreated, Is.False);
                Assert.That(HarkenNative.Vn_SetEmitterPosition(retained.Handle, default), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(fixture.Runtime.SetSuspended(false), Is.EqualTo(VnResult.VN_OK));
                component.enabled = true;
                Emitter replacement = component.Handle;
                Assert.That(replacement.IsValid, Is.True);
                Assert.That(replacement.Handle, Is.Not.EqualTo(retained.Handle));
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(Get<ICollection>(fixture.Runtime, "emitterCleanup").Count, Is.Zero);
                Assert.That(HarkenNative.Vn_SetEmitterPosition(retained.Handle, default), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(HarkenNative.Vn_SetEmitterPosition(replacement.Handle, default), Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DestroyedEmitterDuringSuspensionDoesNotRetainANativeSlot()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start(true);
                var component = fixture.Source.GetComponent<HarkenEmitter>();
                Emitter retained = component.Handle;
                PackBootstrapSession session = fixture.Runtime.Session;
                Assert.That(fixture.Runtime.SetSuspended(true), Is.EqualTo(VnResult.VN_OK));
                Object.Destroy(component);
                yield return null;
                Assert.That(fixture.Runtime.Session, Is.SameAs(session));
                Assert.That(HarkenNative.Vn_SetEmitterPosition(retained.Handle, default),
                    Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(fixture.Runtime.SetSuspended(false), Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DestroyedActionReleasesAfterANewBlockInTheSameSession()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                ActionFiring retained = fixture.Action.Firing;
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Assert.That(retained.Query(out VnActionFiringInfo active), Is.EqualTo(VnResult.VN_OK));
                Assert.That(active.livePlayableCount, Is.GreaterThan(0));
                ulong baseline = CaptureBlock();
                Object.Destroy(fixture.Action);
                yield return WaitForRelease(fixture, retained, baseline);
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DisableThenEnableSeparatesOldCleanupFromTheNewFiring()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                ActionFiring first = fixture.Action.Firing;
                LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: Action .*VN_INVALID_ARG"));
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_INVALID_ARG));
                ulong baseline = CaptureBlock();
                fixture.Action.enabled = false;
                Assert.That(fixture.Action.Firing.IsValid, Is.False);
                Assert.That(fixture.Action.LastResult, Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(PendingCleanup(fixture.Runtime), Is.EqualTo(1));
                fixture.Action.enabled = true;
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                ActionFiring second = fixture.Action.Firing;
                Assert.That(second.Handle, Is.Not.EqualTo(first.Handle));
                yield return WaitForRelease(fixture, first, baseline);
                Assert.That(second.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Action.Firing.Handle, Is.EqualTo(second.Handle));
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator StoppedSessionRetiresCleanupBeforeANewSessionCanReuseSlots()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start(true);
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                fixture.Action.enabled = false;
                Assert.That(PendingCleanup(fixture.Runtime), Is.EqualTo(1));
                PackBootstrapSession previous = fixture.Runtime.Session;
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(previous.IsActive, Is.False);
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                Assert.That(fixture.Runtime.StartRuntime(fixture.Root), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                fixture.Action.enabled = true;
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                ActionFiring current = fixture.Action.Firing;
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                Assert.That(current.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Action.Firing.Handle, Is.EqualTo(current.Handle));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator CleanupResultListenerCanRestartTheSessionWithoutProcessingOldIndices()
        {
            using (var fixture = new Fixture())
            {
                HarkenProjectSettings.ActiveForStartup().SetMaxActionFirings(2);
                Set(fixture.Action, "stopFadeMs", 5000U);
                fixture.Start();
                var other = fixture.Source.AddComponent<HarkenAction>();
                Set(other, "runtime", fixture.Runtime);
                Set(other, "emitter", fixture.Source.GetComponent<HarkenEmitter>());
                Set(other, "actionName", fixture.Action.ActionName);
                Set(other, "projectId", fixture.Action.ProjectId);
                Set(other, "stopFadeMs", 5000U);
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                VnResult firstPost = fixture.Action.Play();
                VnResult secondPost = other.Play();
                Assert.That(firstPost, Is.EqualTo(VnResult.VN_OK));
                Assert.That(secondPost, Is.EqualTo(VnResult.VN_OK));
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Assert.That(fixture.Action.Firing.Query(out VnActionFiringInfo first), Is.EqualTo(VnResult.VN_OK));
                Assert.That(other.Firing.Query(out VnActionFiringInfo second), Is.EqualTo(VnResult.VN_OK));
                Assert.That(first.livePlayableCount, Is.GreaterThan(0));
                Assert.That(second.livePlayableCount, Is.GreaterThan(0));
                Assert.That(fixture.Runtime.SetSuspended(true), Is.EqualTo(VnResult.VN_OK));
                PackBootstrapSession previous = fixture.Runtime.Session;
                bool restarted = false;
                int transferredCount = 0;
                VnResult shutdown = VnResult.VN_NOT_INITIALIZED, start = VnResult.VN_NOT_INITIALIZED;
                VnResult muted = VnResult.VN_NOT_INITIALIZED;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (restarted || operation != "ActionFiring.Stop(cleanup)" || PendingCleanup(owner) != 2) return;
                    restarted = true;
                    transferredCount = PendingCleanup(owner);
                    shutdown = owner.ShutdownRuntime();
                    start = owner.StartRuntime(fixture.Root);
                    muted = HarkenNative.Vn_SetBusMute(VnConstants.VN_BUS_MASTER, 1, 0);
                };
                fixture.Action.enabled = false;
                Assert.That(PendingCleanup(fixture.Runtime), Is.EqualTo(1));
                Assert.That(restarted, Is.False);
                other.enabled = false;
                Assert.That(restarted, Is.True);
                Assert.That(transferredCount, Is.EqualTo(2));
                Assert.That(shutdown, Is.EqualTo(VnResult.VN_OK));
                Assert.That(start, Is.EqualTo(VnResult.VN_OK));
                Assert.That(muted, Is.EqualTo(VnResult.VN_IN_USE));
                Assert.That(previous.IsActive, Is.False);
                Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                other.enabled = true;
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(other.Play(), Is.EqualTo(VnResult.VN_OK));
                ActionFiring current = other.Firing;
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(current.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(other.Firing.Handle, Is.EqualTo(current.Handle));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DeferredCleanupRemainsBoundedByNativeFiringSlots()
        {
            using (var fixture = new Fixture())
            {
                HarkenProjectSettings.ActiveForStartup().SetMaxActionFirings(2);
                fixture.Start(true);
                var other = fixture.Source.AddComponent<HarkenAction>();
                Set(other, "runtime", fixture.Runtime);
                Set(other, "emitter", fixture.Source.GetComponent<HarkenEmitter>());
                Set(other, "actionName", fixture.Action.ActionName);
                Set(other, "projectId", fixture.Action.ProjectId);
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                VnResult firstPost = fixture.Action.Play();
                VnResult secondPost = other.Play();
                Assert.That(firstPost, Is.EqualTo(VnResult.VN_OK));
                Assert.That(secondPost, Is.EqualTo(VnResult.VN_OK));
                ActionFiring first = fixture.Action.Firing;
                ActionFiring second = other.Firing;
                Assert.That(first.IsValid, Is.True);
                Assert.That(second.IsValid, Is.True);
                Assert.That(second.Handle, Is.Not.EqualTo(first.Handle));
                fixture.Action.enabled = false;
                other.enabled = false;
                Assert.That(PendingCleanup(fixture.Runtime), Is.EqualTo(2));
                Assert.That(first.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(second.Query(out _), Is.EqualTo(VnResult.VN_OK));
                fixture.Action.enabled = true;
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: Action .*VN_NO_ACTION"));
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_NO_ACTION));
                fixture.Action.enabled = false;
                Assert.That(PendingCleanup(fixture.Runtime), Is.EqualTo(2));
                Assert.That(first.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(second.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Action.LastResult, Is.EqualTo(VnResult.VN_NO_ACTION));
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator RejectedStopRetainsCleanupUntilTheDeviceConsumesCommands()
        {
            // The controlled clock guarantees rejection; a real device may already own a deferred stop.
#if HARKEN_SDK_TEST_NULL_PUMP
            yield return AssertSuspendedStopCleanup(true);
#endif
            yield return AssertSuspendedStopCleanup(false);
        }

        private static IEnumerator AssertSuspendedStopCleanup(bool nullDevice)
        {
            using (var fixture = new Fixture())
            {
                fixture.Start(nullDevice);
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                ActionFiring retained = fixture.Action.Firing;
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Assert.That(retained.Query(out VnActionFiringInfo active), Is.EqualTo(VnResult.VN_OK));
                Assert.That(active.livePlayableCount, Is.GreaterThan(0));
                Assert.That(fixture.Runtime.SetSuspended(true), Is.EqualTo(VnResult.VN_OK));
                VnResult saturated = VnResult.VN_OK;
                for (int i = 0; i < 65536 && saturated == VnResult.VN_OK; ++i)
                    saturated = HarkenNative.Vn_SetBusOutputVolume(VnConstants.VN_BUS_MASTER, 0.0f, 0);
                Assert.That(saturated, Is.EqualTo(VnResult.VN_BACKPRESSURE), "The real native command ring must be full.");
                ulong baseline = CaptureBlock();
                VnResult? stopResult = null;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (operation != "ActionFiring.Stop(cleanup)") return;
                    stopResult = result;
                    if (result == VnResult.VN_BACKPRESSURE)
                        LogAssert.Expect(LogType.Error,
                            "Harken operation failed: ActionFiring.Stop(cleanup): VN_BACKPRESSURE");
                };
                fixture.Action.enabled = false;
                Assert.That(fixture.Action.LastResult, Is.EqualTo(VnResult.VN_OK));
                Assert.That(stopResult.HasValue, Is.True);
                if (nullDevice) Assert.That(stopResult.Value, Is.EqualTo(VnResult.VN_BACKPRESSURE));
                else Assert.That(stopResult.Value,
                    Is.EqualTo(VnResult.VN_OK).Or.EqualTo(VnResult.VN_BACKPRESSURE));
                Assert.That(fixture.Runtime.LastResult, Is.EqualTo(stopResult.Value));
                Assert.That(PendingCleanup(fixture.Runtime), Is.EqualTo(1));
                Assert.That(retained.Query(out VnActionFiringInfo refused), Is.EqualTo(VnResult.VN_OK));
                Assert.That(refused.state, Is.Not.EqualTo((uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED));
                Assert.That(fixture.Runtime.SetSuspended(false), Is.EqualTo(VnResult.VN_OK));
                yield return WaitForRelease(fixture, retained, baseline);
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                Assert.That(fixture.Action.LastResult, Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ReentrantResultListenerPreservesEachCompletedOperation()
        {
            using (var fixture = new Fixture())
            {
                var completed = new List<string>();
                bool argumentsCorrect = true;
                Get<HarkenRuntimeResultEvent>(fixture.Runtime, "resultObserved").AddListener(message =>
                {
                    if (message == "Outer: VN_OK") fixture.Runtime.ObserveResult("Inner", VnResult.VN_OK);
                });
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    completed.Add(operation);
                    argumentsCorrect &= ReferenceEquals(owner, fixture.Runtime) && result == VnResult.VN_OK;
                };
                Assert.That(fixture.Runtime.ObserveResult("Outer", VnResult.VN_OK), Is.EqualTo(VnResult.VN_OK));
                Assert.That(completed, Is.EqualTo(new[] { "Inner", "Outer" }));
                Assert.That(argumentsCorrect, Is.True);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator WarmSuccessfulPlayAllocatesNothingAndStillDeliversBothEvents()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                yield return WaitForNewBlock(fixture, CaptureBlock());
                Set(fixture.Action, "stopFadeMs", 0U);
                var messages = Get<HarkenRuntimeResultEvent>(fixture.Runtime, "resultObserved");
                int observed = 0, completed = 0;
                bool eventArgumentsCorrect = true;
                messages.AddListener(message =>
                {
                    if (message == "Actions.PostResolved: VN_OK") ++observed;
                });
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (operation != "Actions.PostResolved") return;
                    ++completed;
                    eventArgumentsCorrect &= ReferenceEquals(owner, fixture.Runtime) && result == VnResult.VN_OK;
                };
                for (int i = 0; i < 100; ++i)
                    fixture.Runtime.ObserveResult("Cache pressure " + i, VnResult.VN_OK);
                Assert.That(Get<Dictionary<string, string>>(fixture.Runtime, "successMessages").Count,
                    Is.LessThanOrEqualTo(64));
                Recorder recorder = Recorder.Get("GC.Alloc");
                recorder.enabled = false;
                recorder.FilterToCurrentThread();
                try
                {
                    AssertAllocationControl(recorder);
                    int allocations = 0;
                    for (int i = 0; i < 17; ++i)
                    {
                        Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK), "Prepare iteration " + i);
                        recorder.enabled = true;
                        VnResult result = fixture.Action.Play();
                        recorder.enabled = false;
                        int count = recorder.sampleBlockCount;
                        Assert.That(result, Is.EqualTo(VnResult.VN_OK), "Play iteration " + i);
                        if (i != 0) allocations += count;
                        ActionFiring firing = fixture.Action.Firing;
                        ulong baseline = CaptureBlock();
                        Assert.That(fixture.Action.Stop(), Is.EqualTo(VnResult.VN_OK));
                        yield return WaitForRelease(fixture, firing, baseline);
                        Assert.That(fixture.Action.IsPlaying, Is.False);
                    }
                    AssertAllocationControl(recorder);
                    Assert.That(allocations, Is.Zero, "Warm Play allocations across 16 successful native posts.");
                    Assert.That(observed, Is.EqualTo(17));
                    Assert.That(completed, Is.EqualTo(17));
                    Assert.That(eventArgumentsCorrect, Is.True);
                }
                finally
                {
                    recorder.enabled = false;
                    recorder.CollectFromAllThreads();
                }
            }
            yield return null;
        }

        private static void AssertAllocationControl(Recorder recorder)
        {
            recorder.enabled = true;
            var control = new byte[1024];
            recorder.enabled = false;
            int allocations = recorder.sampleBlockCount;
            GC.KeepAlive(control);
            Assert.That(allocations, Is.GreaterThan(0), "GC.Alloc must detect the positive control.");
        }

        private static IEnumerator WaitForRelease(Fixture fixture, ActionFiring firing, ulong baseline)
        {
            PackBootstrapSession session = fixture.Runtime.Session;
            float deadline = Time.realtimeSinceStartup + 3;
            while (Time.realtimeSinceStartup < deadline)
            {
                PumpControlledDevice(fixture);
                yield return null;
                Assert.That(fixture.Runtime.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                ulong block = CaptureBlock();
                VnResult result = firing.Query(out _);
                Assert.That(result == VnResult.VN_OK || result == VnResult.VN_STALE_HANDLE, Is.True);
                if (block > baseline && result == VnResult.VN_STALE_HANDLE) yield break;
            }
            Assert.Fail("The same session must publish a newer block and release the firing before the deadline.");
        }

        private static IEnumerator WaitForNewBlock(Fixture fixture, ulong baseline)
        {
            PackBootstrapSession session = fixture.Runtime.Session;
            float deadline = Time.realtimeSinceStartup + 3;
            while (Time.realtimeSinceStartup < deadline)
            {
                PumpControlledDevice(fixture);
                yield return null;
                Assert.That(fixture.Runtime.Session, Is.SameAs(session));
                if (CaptureBlock() > baseline) yield break;
            }
            Assert.Fail("The real device did not publish a new block in the same session.");
        }

        private static ulong CaptureBlock()
        {
            var snapshot = HarkenEngine.DefaultSnapshot();
            Assert.That(HarkenEngine.Capture(ref snapshot), Is.EqualTo(VnResult.VN_OK));
            return snapshot.blockIndex;
        }

#if HARKEN_SDK_TEST_NULL_PUMP
        [System.Runtime.InteropServices.DllImport(HarkenNative.LibraryName,
            CallingConvention = System.Runtime.InteropServices.CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpNull(uint blocks);
#endif

        private static void PumpControlledDevice(Fixture fixture)
        {
#if HARKEN_SDK_TEST_NULL_PUMP
            if (fixture.NullDevice)
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
#endif
        }

        private static int PendingCleanup(HarkenAudioSystem runtime)
            => Get<ICollection>(runtime, "firingCleanup").Count;

        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);

        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);

        private sealed class Fixture : IDisposable
        {
            public readonly string Root = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
            public readonly GameObject Owner = new GameObject("Action ownership Runtime");
            public readonly GameObject Source = new GameObject("Action ownership source");
            public readonly HarkenAudioSystem Runtime;
            public readonly HarkenAction Action;
            public bool NullDevice { get; private set; }

            private readonly HarkenProjectSettingsLease settingsLease = HarkenProjectSettingsLease.Acquire();

            public Fixture()
            {
                Owner.SetActive(false);
                Source.SetActive(false);
                Runtime = Owner.AddComponent<HarkenAudioSystem>();
                HarkenEmitter emitter = Source.AddComponent<HarkenEmitter>();
                Action = Source.AddComponent<HarkenAction>();
                Set(emitter, "runtime", Runtime);
                Set(Action, "runtime", Runtime);
                Set(Action, "emitter", emitter);
            }

            public void Start(bool nullDevice = false)
            {
                NullDevice = nullDevice;
                Assert.That(CompanionTestPublication.Read(Root, null, out CompanionTestPublication publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction("Play_U4_A", out CompanionTestAction selected), Is.True);
                Set(Action, "actionName", selected.Name);
                Set(Action, "projectId", publication.ProjectId);
                settingsLease.Settings.SetPackDirectory(Root);
                Assert.That(Runtime.ConfigureBeforeStartup("Audio/U4Original", publication.InitPack.Path,
                    Array.Empty<ulong>(), nullDevice, false), Is.EqualTo(VnResult.VN_OK));
                Owner.SetActive(true);
                Assert.That(Runtime.IsReady, Is.True);
                Assert.That(Runtime.Session.PrepareActions(new[] { selected.Name }, publication.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenNative.Vn_SetBusMute(VnConstants.VN_BUS_MASTER, 1, 0), Is.EqualTo(VnResult.VN_IN_USE));
                Source.SetActive(true);
            }

            public void Dispose()
            {
                Runtime.ShutdownRuntime();
                Object.Destroy(Source);
                Object.Destroy(Owner);
                settingsLease.Dispose();
            }
        }
    }
}
