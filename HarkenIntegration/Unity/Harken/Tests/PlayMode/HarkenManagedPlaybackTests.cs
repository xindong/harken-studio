using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Profiling;
using UnityEngine.TestTools;
using Harken.Generated;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    [DefaultExecutionOrder(10000)]
    public sealed class HarkenLateActionPostProbe : MonoBehaviour
    {
        public Action Trigger { get; set; }
        public int TriggerFrame { get; private set; } = -1;

        private void LateUpdate()
        {
            Action trigger = Trigger;
            if (trigger == null) return;
            Trigger = null;
            TriggerFrame = Time.frameCount;
            trigger();
        }
    }

    [DefaultExecutionOrder(30000)]
    public sealed class HarkenVeryLateActionPostProbe : MonoBehaviour
    {
        public Action Trigger { get; set; }
        public int TriggerFrame { get; private set; } = -1;

        private void LateUpdate()
        {
            Action trigger = Trigger;
            if (trigger == null) return;
            Trigger = null;
            TriggerFrame = Time.frameCount;
            trigger();
        }
    }

    public sealed class HarkenManagedPlaybackTests
    {
        private static VnObjectId Id(ulong value) => new VnObjectId(0x3da91a64b9474560UL, value);

        [UnityTest]
        public IEnumerator TypedVariableCatalogCallbackRestartCannotWriteAnOldRequest()
        {
            try
            {
                using (var fixture = new Fixture(2))
                {
                    fixture.Start();
                    PackBootstrapSession previous = fixture.Runtime.Session;
                    ulong previousVersion = VariableGameplay.SessionVersion;
                    InstallVariableDisplayMismatch();
                    bool restarted = false;
                    fixture.Runtime.RuntimeEnded += (owner, state, operation, result) =>
                    {
                        if (restarted) return;
                        restarted = true;
                        fixture.Start();
                    };
                    VariableCatalogGuard.DisplayMismatchHandler = (live, generated) =>
                        Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    LogAssert.Expect(LogType.Warning, "Harken Variable API catalog is stale; write still proceeds.");
                    FieldInfo rpm = typeof(HarkenVariables).GetField("VehicleRPM",
                        BindingFlags.Public | BindingFlags.Static);
                    VnObjectId rpmId = rpm != null && rpm.FieldType == typeof(VnObjectId)
                        ? (VnObjectId)rpm.GetValue(null) : Id(7);
                    HarkenVariableId variable = HarkenVariableId.FromGenerated("VehicleRPM", rpmId);
                    Assert.That(Values.SetValue(variable, 33), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                    Assert.That(restarted, Is.True);
                    Assert.That(previous.IsActive, Is.False);
                    Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                    Assert.That(VariableGameplay.SessionVersion, Is.GreaterThan(previousVersion));
                }
            }
            finally { VariableCatalogGuard.ResetTestState(); }
            yield return null;
        }

        private static void InstallVariableDisplayMismatch()
        {
            VariableCatalogGuard.Query = (ref VnVariableCatalogInfoV1 info) =>
            {
                info.executionCatalogFingerprint = HarkenVariables.ExecutionCatalogFingerprint;
                info.sdkCatalogFingerprint = HarkenVariables.CatalogFingerprint ^ 1UL;
                return VnResult.VN_OK;
            };
        }

        [UnityTest]
        public IEnumerator ManagedPostsOverlapAndRefusalDoesNotGrowOwnership()
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                ActionFiring first = fixture.Play();
                ActionFiring second = fixture.Play();
                Assert.That(first.Handle, Is.Not.EqualTo(second.Handle));
                Assert.That(Pending(fixture.Runtime), Is.EqualTo(2));
                ExpectPlayFailure(VnResult.VN_NO_ACTION);
                Assert.That(fixture.TryPlay(out ActionFiring refused), Is.EqualTo(VnResult.VN_NO_ACTION));
                Assert.That(refused.IsValid, Is.False);
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(Pending(fixture.Runtime), Is.EqualTo(2));
                Assert.That(first.Query(out VnActionFiringInfo firstInfo), Is.EqualTo(VnResult.VN_OK));
                Assert.That(second.Query(out VnActionFiringInfo secondInfo), Is.EqualTo(VnResult.VN_OK));
                Assert.That(firstInfo.state, Is.Not.EqualTo((uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED));
                Assert.That(secondInfo.state, Is.Not.EqualTo((uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED));
                Assert.That(Get<ICollection>(fixture.Runtime, "firingCleanup").Count, Is.Zero);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator NativeCapacityRefusalLeavesTheManagedSlotAvailable()
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                Assert.That(Actions.Resolve(fixture.Action.Name, fixture.Publication.ProjectId,
                    out ActionResolved resolved), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post(in resolved, Emitter.None, 44, out ActionFiring external),
                    Is.EqualTo(VnResult.VN_OK));
                fixture.Play();
                ExpectPlayFailure(VnResult.VN_NO_ACTION);
                Assert.That(fixture.TryPlay(out ActionFiring refused), Is.EqualTo(VnResult.VN_NO_ACTION));
                Assert.That(refused.IsValid, Is.False);
                Assert.That(Pending(fixture.Runtime), Is.EqualTo(1));
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(Pending(fixture.Runtime), Is.EqualTo(1));
                Assert.That(external.Query(out _), Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator InitOnlyManagedTriggerPreparesAndPostsImmediately()
        {
            using (var fixture = new Fixture(2, false))
            {
                fixture.Start();
                ActionFiring firing = fixture.Play();
                Assert.That(firing.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Pending(fixture.Runtime), Is.EqualTo(1));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DemandSourceRejectsInvalidTriggersWithoutReservingSlots()
        {
            using (var fixture = new Fixture(1, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                ExpectPlayFailure(VnResult.VN_CONTENT_NOT_LOADED);
                Assert.That(fixture.Runtime.PlayAction("Missing_Action", fixture.Publication.ProjectId, out _),
                    Is.EqualTo(VnResult.VN_CONTENT_NOT_LOADED));
                ExpectPlayFailure(VnResult.VN_GAMEPLAY_WRONG_PROJECT);
                Assert.That(fixture.Runtime.PlayAction(fixture.Action.Name,
                    "11111111-2222-3333-4444-555555555555", out _), Is.EqualTo(VnResult.VN_GAMEPLAY_WRONG_PROJECT));
                Assert.That(source.Calls, Is.Empty);
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                fixture.Play();
                Assert.That(source.Calls.Count, Is.EqualTo(1));
                ExpectPlayFailure(VnResult.VN_NO_ACTION);
                Assert.That(fixture.Runtime.PlayAction("Play_U4_B", fixture.Publication.ProjectId, out _),
                    Is.EqualTo(VnResult.VN_NO_ACTION));
                Assert.That(source.Calls.Count, Is.EqualTo(1));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DemandCallbackCannotReenterSamePackOrConsumeReservedSlot()
        {
            using (var fixture = new Fixture(1, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                source.BeforeRead = id =>
                {
                    Assert.That(fixture.Runtime.Session.AcquirePack(id, out _), Is.EqualTo(VnResult.VN_IN_USE));
                    Invoke(fixture.Runtime, "AdvanceFrame");
                    ExpectPlayFailure(VnResult.VN_NO_ACTION);
                    Assert.That(fixture.TryPlay(out _), Is.EqualTo(VnResult.VN_NO_ACTION));
                };
                fixture.Play();
                Assert.That(source.Calls.Count, Is.EqualTo(1));
                Assert.That(Pending(fixture.Runtime), Is.EqualTo(1));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DemandCallbackRestartCannotLoadIntoTheNewSession()
        {
            using (var fixture = new Fixture(2, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                PackBootstrapSession previous = fixture.Runtime.Session;
                source.BeforeRead = id =>
                {
                    Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    fixture.Start();
                };
                ExpectPlayFailure(VnResult.VN_NOT_INITIALIZED);
                Assert.That(fixture.TryPlay(out ActionFiring refused), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                Assert.That(refused.IsValid, Is.False);
                Assert.That(previous.IsActive, Is.False);
                Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                AssertUnprepared(fixture.Action.Name, fixture.Publication.ProjectId);
                fixture.Play();
                Assert.That(source.Calls.Count, Is.EqualTo(1));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DemandReplaceCallbackRestartCannotLoadIntoTheNewSession()
        {
            using (var fixture = new Fixture(2, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                PackBootstrapSession previous = fixture.Runtime.Session;
                Assert.That(previous.PrepareActions(new[] { fixture.Action.Name },
                    fixture.Publication.ProjectId, out ActionPreparation preparation), Is.EqualTo(VnResult.VN_OK));
                Assert.That(previous.AcquirePack(fixture.Action.OwnerPackStableId, out Pack pack),
                    Is.EqualTo(VnResult.VN_OK));
                source.BeforeRead = id =>
                {
                    Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    fixture.Start();
                };
                Assert.That(previous.ReplacePack(fixture.Action.OwnerPackStableId, pack),
                    Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                Assert.That(previous.IsActive, Is.False);
                Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                Assert.That(preparation.Release(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(preparation.IsReleased, Is.True);
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                AssertUnprepared(fixture.Action.Name, fixture.Publication.ProjectId);
                fixture.Play();
                Assert.That(source.Calls.Count, Is.EqualTo(2));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ComponentDemandCallbackDisableCannotPostOrRetainItsPreparedContent()
        {
            using (var fixture = new Fixture(2, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                HarkenAction action = AddAction(fixture);
                source.BeforeRead = id => action.enabled = false;
                // An invalidated component returns the failure without publishing an old operation result.
                Assert.That(action.Play(), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                Assert.That(action.Firing.IsValid, Is.False);
                Assert.That(Get<ulong>(action, "callerSeed"), Is.Zero);
                Assert.That(Get<ICollection>(fixture.Runtime, "firingCleanup").Count, Is.Zero);
                AssertUnprepared(fixture.Action.Name, fixture.Publication.ProjectId);
                source.BeforeRead = null;
                action.enabled = true;
                Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(source.Calls.Count, Is.EqualTo(2));
                Assert.That(Pending(fixture.Runtime), Is.Zero, "Component and managed firing ownership must stay distinct.");
            }
            yield return null;
        }

        private static HarkenAction AddAction(Fixture fixture)
        {
            var owner = new GameObject("Demand Action component");
            owner.SetActive(false);
            owner.transform.SetParent(fixture.Owner.transform);
            HarkenEmitter emitter = owner.AddComponent<HarkenEmitter>();
            Set(emitter, "runtime", fixture.Runtime);
            HarkenAction action = owner.AddComponent<HarkenAction>();
            Set(action, "runtime", fixture.Runtime);
            Set(action, "emitter", emitter);
            Set(action, "actionName", fixture.Action.Name);
            Set(action, "projectId", fixture.Publication.ProjectId);
            Set(action, "stopFadeMs", 0U);
            owner.SetActive(true);
            return action;
        }

        private sealed class PreparationSlot { public ActionPreparation Value; }

        private static void AssertUnprepared(string name, string projectId)
        {
            Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(Actions.Resolve(name, projectId, out _), Is.EqualTo(VnResult.VN_UNKNOWN_ID));
        }

        [UnityTest]
        public IEnumerator ShutdownCompletesOwnershipAndRestoresAudioBeforeAListenerRestarts()
        {
            bool originalPause = AudioListener.pause;
            float originalVolume = AudioListener.volume;
            try
            {
                AudioListener.pause = false;
                AudioListener.volume = 0.37f;
                using (var fixture = new Fixture(2))
                {
                    Assert.That(fixture.Runtime.ConfigureLifecycleBeforeStartup(true, true), Is.EqualTo(VnResult.VN_OK));
                    fixture.Start();
                    Assert.That(AudioListener.pause, Is.True);
                    Assert.That(AudioListener.volume, Is.Zero);
                    fixture.Play();
                    PackBootstrapSession previous = fixture.Runtime.Session;
                    int completed = 0;
                    fixture.Runtime.RuntimeEnded += (owner, state, operation, result) =>
                    {
                        ++completed;
                        Assert.That(owner.State, Is.EqualTo(state));
                        Assert.That(state, Is.EqualTo(HarkenRuntimeState.Stopped));
                        Assert.That(operation, Is.EqualTo("PackBootstrapSession.Shutdown"));
                        Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                        Assert.That(owner.Session, Is.Null);
                        Assert.That(Pending(owner), Is.Zero);
                        Assert.That(AudioListener.pause, Is.False);
                        Assert.That(AudioListener.volume, Is.EqualTo(0.37f));
                        if (completed == 1) fixture.Start();
                    };
                    Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(completed, Is.EqualTo(1));
                    Assert.That(previous.IsActive, Is.False);
                    Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                    Assert.That(fixture.Runtime.IsReady, Is.True);
                    Assert.That(AudioListener.pause, Is.True);
                    fixture.Play();
                    fixture.Runtime.enabled = false;
                    Assert.That(completed, Is.EqualTo(2));
                    Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
                    Assert.That(fixture.Runtime.Session, Is.Null);
                    Assert.That(AudioListener.volume, Is.EqualTo(0.37f));
                }
            }
            finally
            {
                AudioListener.pause = originalPause;
                AudioListener.volume = originalVolume;
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator LifecycleOptInsPreserveDefaultDisableAndUnityAudioBehavior()
        {
            bool originalPause = AudioListener.pause;
            float originalVolume = AudioListener.volume;
            using (var fixture = new Fixture(0))
            {
                fixture.Start();
                Assert.That(Get<Array>(fixture.Runtime, "managedFirings").Length, Is.EqualTo(256));
                fixture.Play();
                fixture.Runtime.enabled = false;
                Assert.That(fixture.Runtime.Session.IsActive, Is.True);
                Assert.That(AudioListener.pause, Is.EqualTo(originalPause));
                Assert.That(AudioListener.volume, Is.EqualTo(originalVolume));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator RuntimeFailureCompletionRetainsTheOriginalCause()
        {
            bool originalPause = AudioListener.pause;
            float originalVolume = AudioListener.volume;
            using (var fixture = new Fixture(2))
            {
                Assert.That(fixture.Runtime.ConfigureLifecycleBeforeStartup(true, true), Is.EqualTo(VnResult.VN_OK));
                fixture.Start();
                fixture.Play();
                ulong previousVersion = VariableGameplay.SessionVersion;
                int completed = 0;
                VnResult disabledRestart = VnResult.VN_OK;
                fixture.Runtime.RuntimeEnded += (owner, state, operation, result) =>
                {
                    ++completed;
                    if (completed != 1) return;
                    Assert.That(state, Is.EqualTo(HarkenRuntimeState.Failed));
                    Assert.That(owner.State, Is.EqualTo(state));
                    Assert.That(owner.Session, Is.Null);
                    Assert.That(operation, Is.EqualTo("Injected.Update"));
                    Assert.That(result, Is.EqualTo(VnResult.VN_INVALID_ARG));
                    Assert.That(owner.LastOperation, Is.EqualTo(operation));
                    Assert.That(owner.LastResult, Is.EqualTo(result));
                    Assert.That(owner.enabled, Is.False);
                    Assert.That(Pending(owner), Is.Zero);
                    Assert.That(VariableGameplay.SessionVersion, Is.GreaterThan(previousVersion));
                    Assert.That(AudioListener.pause, Is.EqualTo(originalPause));
                    Assert.That(AudioListener.volume, Is.EqualTo(originalVolume));
                    disabledRestart = owner.StartRuntime(fixture.Root);
                };
                LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: Injected.Update: VN_INVALID_ARG"));
                Invoke(fixture.Runtime, "FailRuntime", "Injected.Update", VnResult.VN_INVALID_ARG);
                Assert.That(completed, Is.EqualTo(1));
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Failed));
                Assert.That(fixture.Runtime.LastResult, Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(disabledRestart, Is.Not.EqualTo(VnResult.VN_OK));
                fixture.Runtime.enabled = true;
                fixture.Start();
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ResumeObserverShutdownCannotPublishReadyForTheEndedSession()
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                Assert.That(fixture.Runtime.SetSuspended(true), Is.EqualTo(VnResult.VN_OK));
                int ready = 0;
                fixture.Runtime.BecameReady += _ => ++ready;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (operation == "HarkenEngine.Resume") owner.ShutdownRuntime();
                };
                Assert.That(fixture.Runtime.SetSuspended(false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(ready, Is.Zero);
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
                Assert.That(fixture.Runtime.Session, Is.Null);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator InactiveHostAllowsConfigurationButRequiresActivationBeforeStartup()
        {
            using (var fixture = new Fixture(2, true, false))
            {
                Assert.That(fixture.Runtime.ConfigureLifecycleBeforeStartup(true, true), Is.EqualTo(VnResult.VN_OK));
                LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: PackBootstrap.Start: VN_ALREADY_INITIALIZED"));
                Assert.That(fixture.Runtime.StartRuntime(fixture.Root), Is.Not.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.Session, Is.Null);
                fixture.Owner.SetActive(true);
                fixture.Start();
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator DisabledHostRejectsRestartFromRuntimeEnded()
            => HostExitRejectsRestart("disable");

        [UnityTest]
        public IEnumerator DestroyedGameObjectRejectsRestartFromRuntimeEnded()
            => HostExitRejectsRestart("destroy");

        [UnityTest]
        public IEnumerator ApplicationQuitRejectsRestartFromRuntimeEnded()
            => HostExitRejectsRestart("quit");

        private static IEnumerator HostExitRejectsRestart(string exit)
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Runtime.ConfigureLifecycleBeforeStartup(true, true);
                fixture.Start();
                int ended = 0;
                VnResult restart = VnResult.VN_OK;
                fixture.Runtime.RuntimeEnded += (owner, state, operation, result) =>
                {
                    if (++ended == 1) restart = owner.StartRuntime(fixture.Root);
                };
                if (exit == "disable") fixture.Runtime.enabled = false;
                else if (exit == "destroy") Object.Destroy(fixture.Owner);
                else Invoke(fixture.Runtime, "OnApplicationQuit");
                yield return null;
                Assert.That(ended, Is.EqualTo(1));
                Assert.That(restart, Is.Not.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.Session, Is.Null);
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                if (exit == "disable")
                {
                    fixture.Runtime.enabled = true;
                    fixture.Start();
                }
            }
            yield return null;
        }

#if HARKEN_SDK_TEST_NULL_PUMP
        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void VnPreview_PumpNullMixer(uint blockCount);

        [UnityTest]
        public IEnumerator NaturalCompletionReclaimsSlotsWithoutStoppingOrReplayingRefusedPosts()
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                PackBootstrapSession owner = fixture.Runtime.Session;
                ulong baseline = CaptureBlock();
                for (int iteration = 0; iteration < 4; ++iteration)
                {
                    ActionFiring first = fixture.Play();
                    ActionFiring second = fixture.Play();
                    ExpectPlayFailure(VnResult.VN_NO_ACTION);
                    Assert.That(fixture.TryPlay(out _), Is.EqualTo(VnResult.VN_NO_ACTION));
                    PumpUntilTerminal(owner, first, second);
                    Assert.That(CaptureBlock(), Is.GreaterThan(baseline));
                    baseline = CaptureBlock();
                    Invoke(fixture.Runtime, "AdvanceFrame");
                    Assert.That(fixture.Runtime.Session, Is.SameAs(owner));
                    Assert.That(first.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                    Assert.That(second.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                    Assert.That(Pending(fixture.Runtime), Is.Zero);
                    Assert.That(Get<ICollection>(fixture.Runtime, "firingCleanup").Count, Is.Zero);
                    VnPreview_PumpNullMixer(4);
                    Invoke(fixture.Runtime, "AdvanceFrame");
                    Assert.That(Pending(fixture.Runtime), Is.Zero);
                }
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator CleanupObserverRestartCannotTransferAnOldTriggerToTheNewSession()
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                PackBootstrapSession previous = fixture.Runtime.Session;
                ActionFiring first = fixture.Play();
                ActionFiring second = fixture.Play();
                PumpUntilTerminal(previous, first, second);
                bool restarted = false;
                ActionFiring current = default;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (restarted || operation != "ActionFiring.Release(managed)") return;
                    restarted = true;
                    Assert.That(owner.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    fixture.Start();
                    current = fixture.Play();
                };
                ExpectPlayFailure(VnResult.VN_NOT_INITIALIZED);
                Assert.That(fixture.TryPlay(out ActionFiring refused), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                Assert.That(refused.IsValid, Is.False);
                Assert.That(restarted, Is.True);
                Assert.That(previous.IsActive, Is.False);
                Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                Assert.That(Pending(fixture.Runtime), Is.EqualTo(1));
                Assert.That(current.Query(out _), Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        private static void PumpUntilTerminal(PackBootstrapSession session, params ActionFiring[] firings)
        {
            for (int i = 0; i < 8192; ++i)
            {
                Assert.That(session.IsActive, Is.True);
                VnPreview_PumpNullMixer(1);
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                bool terminal = true;
                foreach (ActionFiring firing in firings)
                {
                    Assert.That(firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                    terminal &= info.state == (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED;
                    if (info.state == (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED)
                        Assert.That(info.terminalReason, Is.EqualTo((uint)VnActionTerminalReason.VN_ACTION_TERMINAL_NATURAL));
                }
                if (terminal) return;
            }
            Assert.Fail("The posted firings did not naturally finish in the same null-device session.");
        }

        [UnityTest]
        public IEnumerator NaturalCompletionReleasesDemandBytesAndColdPlaybackFetchesAgain()
        {
            using (var fixture = new Fixture(2, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                for (int iteration = 0; iteration < 3; ++iteration)
                {
                    ulong baseline = CaptureBlock();
                    ActionFiring first = fixture.Play();
                    ActionFiring second = fixture.Play();
                    Assert.That(source.Calls.Count, Is.EqualTo(iteration + 1));
                    PumpUntilTerminal(fixture.Runtime.Session, first, second);
                    Assert.That(CaptureBlock(), Is.GreaterThan(baseline));
                    Invoke(fixture.Runtime, "AdvanceFrame");
                    Assert.That(Pending(fixture.Runtime), Is.Zero);
                    Assert.That(first.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                    Assert.That(second.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                    AssertUnprepared(fixture.Action.Name, fixture.Publication.ProjectId);
                    Assert.That(PackByteProvider.OutstandingPinCount, Is.Zero);
                }
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator NaturalFiringCleanupPreservesPreloadUntilItsOwnRelease()
        {
            using (var fixture = new Fixture(2, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                Assert.That(fixture.Runtime.Session.PrepareActions(new[] { fixture.Action.Name },
                    fixture.Publication.ProjectId, out ActionPreparation preload), Is.EqualTo(VnResult.VN_OK));
                ActionFiring first = fixture.Play();
                ActionFiring second = fixture.Play();
                PumpUntilTerminal(fixture.Runtime.Session, first, second);
                Invoke(fixture.Runtime, "AdvanceFrame");
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                Assert.That(preload.IsPrepared, Is.True);
                ActionFiring third = fixture.Play();
                Assert.That(source.Calls.Count, Is.EqualTo(1));
                Assert.That(preload.Release(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(third.Query(out _), Is.EqualTo(VnResult.VN_OK));
                PumpUntilTerminal(fixture.Runtime.Session, third);
                Invoke(fixture.Runtime, "AdvanceFrame");
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                AssertUnprepared(fixture.Action.Name, fixture.Publication.ProjectId);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ComponentNaturalCompletionReleasesResourcesAndReusesItsPlan()
        {
            using (var fixture = new Fixture(2, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                HarkenAction action = AddAction(fixture);
                for (int iteration = 0; iteration < 3; ++iteration)
                {
                    Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                    ActionFiring firing = action.Firing;
                    PumpUntilTerminal(fixture.Runtime.Session, firing);
                    Invoke(action, "Update");
                    Assert.That(action.Firing.IsValid, Is.False);
                    Assert.That(firing.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                    Assert.That(source.Calls.Count, Is.EqualTo(iteration + 1));
                    AssertUnprepared(fixture.Action.Name, fixture.Publication.ProjectId);
                }
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator WarmComponentPlayKeepsZeroAllocationWithOwnedPreparationOnNullDevice()
        {
            using (var fixture = new Fixture(2))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                HarkenAction action = AddAction(fixture);
                Recorder recorder = Recorder.Get("GC.Alloc");
                recorder.enabled = false;
                recorder.FilterToCurrentThread();
                try
                {
                    AssertAllocationRecorder(recorder);
                    int allocations = 0;
                    for (int iteration = 0; iteration < 17; ++iteration)
                    {
                        recorder.enabled = true;
                        VnResult result = action.Play();
                        recorder.enabled = false;
                        int count = recorder.sampleBlockCount;
                        Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                        if (iteration != 0) allocations += count;
                        PumpUntilTerminal(fixture.Runtime.Session, action.Firing);
                        Invoke(action, "Update");
                        Assert.That(action.Firing.IsValid, Is.False);
                    }
                    AssertAllocationRecorder(recorder);
                    Assert.That(allocations, Is.Zero, "Sixteen warm Posts must keep the existing allocation contract.");
                    Assert.That(source.Calls.Count, Is.EqualTo(1));
                }
                finally
                {
                    recorder.enabled = false;
                    recorder.CollectFromAllThreads();
                }
            }
            yield return null;
        }

        private static void AssertAllocationRecorder(Recorder recorder)
        {
            recorder.enabled = true;
            var control = new byte[1024];
            recorder.enabled = false;
            int allocations = recorder.sampleBlockCount;
            GC.KeepAlive(control);
            Assert.That(allocations, Is.GreaterThan(0));
        }

        [UnityTest]
        public IEnumerator DisabledComponentTransfersFiringAndResourcesAsOneCleanupOwner()
        {
            using (var fixture = new Fixture(2, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                HarkenAction action = AddAction(fixture);
                Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                ActionFiring firing = action.Firing;
                action.enabled = false;
                Assert.That(action.Firing.IsValid, Is.False);
                Assert.That(Get<ICollection>(fixture.Runtime, "firingCleanup").Count, Is.EqualTo(1));
                for (int i = 0; i < 8192 && Get<ICollection>(fixture.Runtime, "firingCleanup").Count != 0; ++i)
                {
                    VnPreview_PumpNullMixer(1);
                    Invoke(fixture.Runtime, "AdvanceFrame");
                }
                Assert.That(Get<ICollection>(fixture.Runtime, "firingCleanup").Count, Is.Zero);
                Assert.That(firing.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                AssertUnprepared(fixture.Action.Name, fixture.Publication.ProjectId);
                Assert.That(source.Calls.Count, Is.EqualTo(1));
            }
            yield return null;
        }

        private static ulong CaptureBlock()
        {
            VnSnapshot snapshot = HarkenEngine.DefaultSnapshot();
            Assert.That(HarkenEngine.Capture(ref snapshot), Is.EqualTo(VnResult.VN_OK));
            return snapshot.blockIndex;
        }
#endif

        private static int Pending(HarkenAudioSystem runtime)
        {
            int count = 0;
            foreach (object slot in Get<Array>(runtime, "managedFirings"))
                if ((bool)slot.GetType().GetField("Reserved").GetValue(slot)) ++count;
            return count;
        }

        [UnityTest]
        public IEnumerator SpatialPlaybackRetainsTheSourceAcrossOverlapAndVisualRecycle()
        {
            var actor = new GameObject("Managed spatial actor");
            try
            {
                using (var fixture = new Fixture(4))
                {
                    fixture.Start();
                    Assert.That(fixture.Runtime.CreateSpatialSource(actor.transform, out var source), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.CreateSpatialSource(actor.transform, out var same), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(same, Is.SameAs(source));
                    Assert.That(fixture.Runtime.PlayAction(fixture.Action.Name, fixture.Publication.ProjectId, source, out var first),
                        Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.PlayAction(fixture.Action.Name, fixture.Publication.ProjectId, source, out var second),
                        Is.EqualTo(VnResult.VN_OK));
                    Assert.That(first.Handle, Is.Not.EqualTo(second.Handle));
                    Assert.That(source.EndFollow(), Is.EqualTo(VnResult.VN_OK));
                    actor.transform.position = Vector3.left * 20;
                    Invoke(fixture.Runtime, "AdvanceFrame");
                    Assert.That(first.Query(out _), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(second.Query(out _), Is.EqualTo(VnResult.VN_OK));
                    ExpectPlayFailure(VnResult.VN_INVALID_ARG);
                    Assert.That(fixture.Runtime.PlayAction(fixture.Action.Name, fixture.Publication.ProjectId, source, out _),
                        Is.EqualTo(VnResult.VN_INVALID_ARG));
                    Assert.That(fixture.Runtime.CreateSpatialSource(actor.transform, out var next), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(next, Is.Not.SameAs(source));
                    fixture.Runtime.ShutdownRuntime();
                    fixture.Start();
                    ExpectPlayFailure(VnResult.VN_STALE_HANDLE);
                    Assert.That(fixture.Runtime.PlayAction(fixture.Action.Name, fixture.Publication.ProjectId, next, out _),
                        Is.EqualTo(VnResult.VN_STALE_HANDLE));
                }
            }
            finally { Object.Destroy(actor); }
            yield return null;
        }

        private static void ExpectPlayFailure(VnResult result)
            => LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: HarkenAudioSystem.PlayAction: " + result));

        private static void Invoke(object target, string name, params object[] arguments)
            => target.GetType().GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic).Invoke(target, arguments);

        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);

        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);

        private sealed class Fixture : IDisposable
        {
            public readonly string Root = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
            public readonly GameObject Owner = new GameObject("Managed playback Runtime");
            public readonly HarkenAudioSystem Runtime;
            public readonly CompanionTestPublication Publication;
            public readonly CompanionTestAction Action;
            private HarkenProjectSettingsLease settingsLease;
            private readonly bool prepare;

            public Fixture(uint capacity, bool prepare = true, bool activate = true)
            {
                this.prepare = prepare;
                Owner.SetActive(false);
                Runtime = Owner.AddComponent<HarkenAudioSystem>();
                Set(Runtime, "autoInitialize", false);
                Set(Runtime, "persistAcrossScenes", false);
                settingsLease = HarkenProjectSettingsLease.Acquire();
                settingsLease.Settings.SetMaxActionFirings(capacity);
                Assert.That(CompanionTestPublication.Read(Root, null, out Publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(Publication.TryFindAction("Play_U4_A", out Action), Is.True);
                Assert.That(Runtime.ConfigureBeforeStartup("Audio/U4Original", Publication.InitPack.Path,
                    Array.Empty<ulong>(), true, false),
                    Is.EqualTo(VnResult.VN_OK));
                if (activate) Owner.SetActive(true);
            }

            public void Start()
            {
                Assert.That(Runtime.StartRuntime(Root), Is.EqualTo(VnResult.VN_OK), Runtime.LastStartupError);
                Assert.That(Runtime.IsReady, Is.True);
                if (prepare) Assert.That(Runtime.Session.PrepareActions(new[] { Action.Name }, Publication.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            }

            public void Start(PackSource prepared)
            {
                Assert.That(Runtime.StartRuntime(prepared), Is.EqualTo(VnResult.VN_OK), Runtime.LastStartupError);
                Assert.That(Runtime.IsReady, Is.True);
                if (prepare) Assert.That(Runtime.Session.PrepareActions(new[] { Action.Name }, Publication.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            }

            public VnResult TryPlay(out ActionFiring firing)
                => Runtime.PlayAction(Action.Name, Publication.ProjectId, out firing);

            public ActionFiring Play()
            {
                Assert.That(TryPlay(out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
                Assert.That(firing.IsValid, Is.True);
                return firing;
            }

            public void Dispose()
            {
                try
                {
                    Runtime.ShutdownRuntime();
                    Object.Destroy(Owner);
                }
                finally { settingsLease.Dispose(); }
            }
        }

        private sealed class DemandSource
        {
            private readonly Fixture fixture;
            public readonly PackSource Prepared;
            public readonly List<ulong> Calls = new List<ulong>();
            public ulong FailureId;
            public Action<ulong> BeforeRead;

            public DemandSource(Fixture fixture)
            {
                this.fixture = fixture;
                Prepared = PackSource.FromReader(Read);
            }

            private byte[] Read(string path)
            {
                if (Path.GetExtension(path) == ".bytes" && Path.GetFileName(path) != "Init.bytes" &&
                    fixture.Publication.TryFindAction(Path.GetFileNameWithoutExtension(path), out CompanionTestAction action))
                {
                    ulong id = action.OwnerPackStableId;
                    Calls.Add(id);
                    BeforeRead?.Invoke(id);
                    if (id == FailureId) throw new IOException("Fixture provider refusal.");
                }
                string absolute = Path.Combine(fixture.Root, path);
                return File.Exists(absolute) ? File.ReadAllBytes(absolute) : null;
            }
        }
    }
}
