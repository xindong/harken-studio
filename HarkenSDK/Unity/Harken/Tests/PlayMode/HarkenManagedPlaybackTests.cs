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
                    LogAssert.Expect(LogType.Warning, "Harken Variable SDK catalog is stale; write still proceeds.");
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

#if HARKEN_SDK_TEST_NULL_PUMP
        [UnityTest]
        public IEnumerator ReusableActionScopesDeduplicateNamesAndShareReferences()
        {
            using (var fixture = new Fixture(2, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                fixture.Publication.TryFindAction("Play_U4_B", out CompanionTestAction dependency);
                fixture.Publication.TryFindAction("Play_U4_Caller", out CompanionTestAction caller);
                PackBootstrapSession owner = fixture.Runtime.Session;
                Assert.That(owner.PrepareActions(new[] { caller.Name, dependency.Name, caller.Name },
                    fixture.Publication.ProjectId, out ActionPreparation first), Is.EqualTo(VnResult.VN_OK));
                Assert.That(source.Calls, Is.EqualTo(new[] { caller.OwnerPackStableId, dependency.OwnerPackStableId }));
                Assert.That(first.IsPrepared, Is.True);
                Assert.That(owner.PrepareActions(new[] { dependency.Name }, fixture.Publication.ProjectId,
                    out ActionPreparation second), Is.EqualTo(VnResult.VN_OK));
                Assert.That(first.Release(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(first.IsReleased, Is.True);
                Assert.That(second.IsPrepared, Is.True);
                Assert.That(owner.AcquirePack(dependency.OwnerPackStableId, out Pack shared), Is.EqualTo(VnResult.VN_OK));
                Assert.That(source.Calls.Count, Is.EqualTo(2), "The second scope must retain the dependency.");
                Assert.That(owner.ReleasePack(dependency.OwnerPackStableId, shared), Is.EqualTo(VnResult.VN_OK));
                AdvanceNullAudioAndNormalService(fixture);
                Assert.That(second.Release(), Is.EqualTo(VnResult.VN_OK));
                AssertUnprepared(dependency.Name, fixture.Publication.ProjectId);
                AssertUnprepared(caller.Name, fixture.Publication.ProjectId);
            }
            yield return null;
        }
#endif

#if HARKEN_SDK_TEST_NULL_PUMP
        [UnityTest]
        public IEnumerator DemandFailureDoesNotPublishOrReplay()
        {
            using (var fixture = new Fixture(2, false))
            {
                fixture.Publication.TryFindAction("Play_U4_B", out CompanionTestAction dependency);
                fixture.Publication.TryFindAction("Play_U4_Caller", out CompanionTestAction caller);
                var source = new DemandSource(fixture) { FailureId = caller.OwnerPackStableId };
                fixture.Start(source.Prepared);
                ExpectPlayFailure(VnResult.VN_PACK_PROVIDER_FAILED);
                Assert.That(fixture.Runtime.PlayAction(caller.Name, fixture.Publication.ProjectId, out ActionFiring refused),
                    Is.EqualTo(VnResult.VN_PACK_PROVIDER_FAILED));
                Assert.That(refused.IsValid, Is.False);
                Assert.That(source.Calls, Is.EqualTo(new[] { caller.OwnerPackStableId }));
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                AssertUnprepared(dependency.Name, fixture.Publication.ProjectId);
                source.FailureId = 0;
                for (int i = 0; i < 3; ++i)
                {
                    AdvanceNullAudioAndNormalService(fixture);
                    yield return null;
                }
                Assert.That(source.Calls.Count, Is.EqualTo(1));
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                Assert.That(fixture.Runtime.PlayAction(caller.Name, fixture.Publication.ProjectId, out _),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(source.Calls.Count, Is.EqualTo(2));
            }
            yield return null;
        }
#endif

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

#if HARKEN_SDK_TEST_NULL_PUMP
        [UnityTest]
        public IEnumerator PreparationPublishesOnlyAfterCompletionAndRejectsReentryDuringReuse()
        {
            using (var fixture = new Fixture(2, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                var holder = new PreparationSlot();
                source.BeforeRead = id => Assert.That(holder.Value, Is.Null);
                Assert.That(fixture.Runtime.Session.PrepareActions(new[] { "Play_U4_A", "Play_U4_B" },
                    fixture.Publication.ProjectId, out holder.Value), Is.EqualTo(VnResult.VN_OK));
                Assert.That(holder.Value.IsPrepared, Is.True);
                AdvanceNullAudioAndNormalService(fixture);
                Assert.That(holder.Value.Release(), Is.EqualTo(VnResult.VN_OK));
                AdvanceNullAudioAndNormalService(fixture);
                source.BeforeRead = id =>
                {
                    Assert.That(holder.Value.Release(), Is.EqualTo(VnResult.VN_IN_USE));
                    Assert.That(holder.Value.Prepare(), Is.EqualTo(VnResult.VN_IN_USE));
                };
                Assert.That(holder.Value.Prepare(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(source.Calls.Count, Is.EqualTo(4));
                AdvanceNullAudioAndNormalService(fixture);
                Assert.That(holder.Value.Release(), Is.EqualTo(VnResult.VN_OK));
                AssertUnprepared("Play_U4_A", fixture.Publication.ProjectId);
                AssertUnprepared("Play_U4_B", fixture.Publication.ProjectId);
            }
            yield return null;
        }
#endif

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

#if HARKEN_SDK_TEST_NULL_PUMP
        private static void AdvanceNullAudioAndNormalService(Fixture fixture)
        {
            float[] samples = new float[4096];
            Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length, out uint frames, out uint channels),
                Is.EqualTo(VnResult.VN_OK));
            Assert.That(frames, Is.GreaterThan(0U));
            Assert.That(channels, Is.GreaterThan(0U));
            Assert.That((ulong)frames * channels, Is.LessThanOrEqualTo((ulong)samples.Length));
            Invoke(fixture.Runtime, "AdvanceFrame");
        }

#endif


        private static void AssertUnprepared(string name, string projectId)
        {
            Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(Actions.Resolve(name, projectId, out _), Is.EqualTo(VnResult.VN_UNKNOWN_ID));
        }

#if HARKEN_SDK_TEST_NULL_PUMP
        [UnityTest]
        public IEnumerator NativeCapacityRefusalReleasesNewPreparationWithoutReplay()
        {
            using (var fixture = new Fixture(1, false))
            {
                var source = new DemandSource(fixture);
                fixture.Start(source.Prepared);
                Assert.That(fixture.Runtime.Session.PrepareActions(new[] { fixture.Action.Name },
                    fixture.Publication.ProjectId, out ActionPreparation externalScope), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Resolve(fixture.Action.Name, fixture.Publication.ProjectId, out ActionResolved resolved),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post(in resolved, Emitter.None, 0, out ActionFiring external), Is.EqualTo(VnResult.VN_OK));
                ExpectPlayFailure(VnResult.VN_NO_ACTION);
                Assert.That(fixture.Runtime.PlayAction("Play_U4_B", fixture.Publication.ProjectId, out ActionFiring refused),
                    Is.EqualTo(VnResult.VN_NO_ACTION));
                Assert.That(refused.IsValid, Is.False);
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                // Capacity is occupied externally; a fresh preparation proves the refused call released B.
                Assert.That(source.Calls.Count, Is.EqualTo(2));
                AdvanceNullAudioAndNormalService(fixture);
                Assert.That(external.Query(out VnActionFiringInfo externalInfo), Is.EqualTo(VnResult.VN_OK));
                Assert.That(externalInfo.state, Is.EqualTo((uint)VnActionFiringState.VN_ACTION_FIRING_ACTIVE));
                Assert.That(fixture.Runtime.Session.PrepareActions(new[] { "Play_U4_B" }, fixture.Publication.ProjectId,
                    out ActionPreparation probe), Is.EqualTo(VnResult.VN_OK));
                Assert.That(source.Calls.Count, Is.EqualTo(3));
                Assert.That(probe.Release(), Is.EqualTo(VnResult.VN_OK));
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(Pending(fixture.Runtime), Is.Zero);
                Assert.That(source.Calls.Count, Is.EqualTo(3));
                Assert.That(externalScope.IsPrepared, Is.True);
            }
            yield return null;
        }
#endif



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
        // This export exists only in the isolated SDK fixture DLL, never in a shipped plugin.
        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpNull(uint blocks);

        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpAndCopy(
            [In, Out] float[] samples, uint capacity, out uint frames, out uint channels);

        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_QueueNotification();

        [UnityTest]
        public IEnumerator NativePcmCaptureRefusesSuspendedLastBlockAndResumesFresh()
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                var samples = new float[4096];
                PrimeFreshPcmSnapshot(samples);
                fixture.Play();
                AssertSilentFreshBlocks(samples, 1);
                Invoke(fixture.Runtime, "AdvanceFrame");
                AssertImmediatePcm(samples);
                ulong before = CaptureBlock();
                Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length,
                    out uint frames, out uint channels), Is.EqualTo(VnResult.VN_OK));
                Assert.That(frames * channels, Is.GreaterThan(0));
                Assert.That(CaptureBlock(), Is.EqualTo(before + 1));
                Assert.That(HarkenEngine.Suspend(), Is.EqualTo(VnResult.VN_OK));
                try
                {
                    before = CaptureBlock();
                    for (int i = 0; i < samples.Length; ++i) samples[i] = 0.375f;
                    Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length,
                        out frames, out channels), Is.EqualTo(VnResult.VN_IN_USE));
                    Assert.That(frames, Is.Zero);
                    Assert.That(channels, Is.Zero);
                    Assert.That(CaptureBlock(), Is.EqualTo(before));
                    foreach (float sample in samples) Assert.That(sample, Is.EqualTo(0.375f));
                }
                finally { Assert.That(HarkenEngine.Resume(), Is.EqualTo(VnResult.VN_OK)); }
                before = CaptureBlock();
                Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length,
                    out frames, out channels), Is.EqualTo(VnResult.VN_OK));
                Assert.That(frames * channels, Is.GreaterThan(0));
                Assert.That(CaptureBlock(), Is.EqualTo(before + 1));
                Assert.That(FirstPcmSample(samples, frames, channels), Is.GreaterThanOrEqualTo(0));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ManagedImmediatePostSurvivesAudioAheadUntilNextNormalUpdate()
            => ImmediatePostAcrossNormalUpdate(false);

        [UnityTest]
        public IEnumerator ComponentImmediatePostSurvivesAudioAheadUntilNextNormalUpdate()
            => ImmediatePostAcrossNormalUpdate(true);

        [UnityTest]
        public IEnumerator ManagedFirstServiceBeforeSourceApplyWaitsForNextNormalService()
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                var samples = new float[4096];
                PrimeFreshPcmSnapshot(samples);
                ActionFiring firing = fixture.Play();
                Invoke(fixture.Runtime, "AdvanceFrame");
                AssertSilentFreshBlocks(samples, 6);
                Assert.That(firing.Query(out VnActionFiringInfo pending), Is.EqualTo(VnResult.VN_OK));
                Assert.That(pending.state, Is.EqualTo((uint)VnActionFiringState.VN_ACTION_FIRING_ACTIVE));
                Invoke(fixture.Runtime, "AdvanceFrame");
                AssertImmediatePcm(samples);
                AssertNaturalTailAfterPureAudio(fixture, firing, samples);
            }
            yield return null;
        }

        private static IEnumerator ImmediatePostAcrossNormalUpdate(bool component)
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                var samples = new float[4096];
                PrimeFreshPcmSnapshot(samples);
                HarkenAction action = component ? AddAction(fixture) : null;
                Invoke(fixture.Runtime, "AdvanceFrame");
                ActionFiring firing;
                if (component)
                {
                    Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                    firing = action.Firing;
                }
                else firing = fixture.Play();
                AssertSilentFreshBlocks(samples, 8);
                Assert.That(firing.IsValid, Is.True);
                Invoke(fixture.Runtime, "AdvanceFrame");
                AssertImmediatePcm(samples);
                AssertNaturalTailAfterPureAudio(fixture, firing, samples);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ManagedVeryLatePostStartsOnNextNormalUnityFrame()
            => ImmediateVeryLatePost(false);

        [UnityTest]
        public IEnumerator ComponentVeryLatePostStartsOnNextNormalUnityFrame()
            => ImmediateVeryLatePost(true);

        private static IEnumerator ImmediateVeryLatePost(bool component)
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                var samples = new float[4096];
                PrimeFreshPcmSnapshot(samples);
                HarkenAction action = component ? AddAction(fixture) : null;
                var probe = fixture.Owner.AddComponent<HarkenVeryLateActionPostProbe>();
                ActionFiring firing = default;
                int postFrame = -1;
                int observedFrame = -1;
                probe.Trigger = () =>
                {
                    postFrame = Time.frameCount;
                    if (component)
                    {
                        Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                        firing = action.Firing;
                    }
                    else firing = fixture.Play();
                    AssertSilentFreshBlocks(samples, 8);
                    probe.Trigger = () =>
                    {
                        observedFrame = Time.frameCount;
                        AssertImmediatePcm(samples);
                    };
                };
                for (int i = 0; i < 4 && observedFrame < 0; ++i) yield return null;
                Assert.That(postFrame, Is.GreaterThanOrEqualTo(0));
                Assert.That(observedFrame, Is.EqualTo(postFrame + 1));
                Assert.That(probe.TriggerFrame, Is.EqualTo(observedFrame));
                Assert.That(firing.IsValid, Is.True);
                AssertNaturalTailAfterPureAudio(fixture, firing, samples);
            }
            yield return null;
        }

        private static int FirstPcmSample(float[] samples, uint frames, uint channels)
        {
            Assert.That(frames, Is.GreaterThan(0));
            Assert.That(channels, Is.GreaterThan(0));
            Assert.That((ulong)frames * channels, Is.LessThanOrEqualTo((ulong)samples.Length));
            for (int i = 0; i < frames * channels; ++i)
            {
                Assert.That(float.IsNaN(samples[i]) || float.IsInfinity(samples[i]), Is.False);
                if (Math.Abs(samples[i]) > 0.0000001f) return i / (int)channels;
            }
            return -1;
        }

        private static int PumpFreshPcm(float[] samples, out uint frames)
        {
            ulong before = CaptureBlock();
            Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length,
                out frames, out uint channels), Is.EqualTo(VnResult.VN_OK));
            Assert.That(CaptureBlock(), Is.EqualTo(before + 1));
            return FirstPcmSample(samples, frames, channels);
        }

        private static void PrimeFreshPcmSnapshot(float[] samples)
        {
            // The initial snapshot and the first published block both have zero-based index zero.
            Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length,
                out uint frames, out uint channels), Is.EqualTo(VnResult.VN_OK));
            Assert.That(FirstPcmSample(samples, frames, channels), Is.EqualTo(-1));
            Assert.That(CaptureBlock(), Is.Zero);
        }

        [UnityTest]
        public IEnumerator NativePcmCaptureRejectsInvalidBufferWithoutRendering()
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                var samples = new float[4096];
                PrimeFreshPcmSnapshot(samples);
                ulong before = CaptureBlock();
                for (int i = 0; i < samples.Length; ++i) samples[i] = 0.375f;
                Assert.That(HarkenSdkTest_PumpAndCopy(samples, 0,
                    out uint frames, out uint channels), Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(frames, Is.Zero);
                Assert.That(channels, Is.Zero);
                Assert.That(CaptureBlock(), Is.EqualTo(before));
                foreach (float sample in samples) Assert.That(sample, Is.EqualTo(0.375f));
                Assert.That(HarkenSdkTest_PumpAndCopy(null, (uint)samples.Length,
                    out frames, out channels), Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(frames, Is.Zero);
                Assert.That(channels, Is.Zero);
                Assert.That(CaptureBlock(), Is.EqualTo(before));
                Assert.That(PumpFreshPcm(samples, out _), Is.EqualTo(-1));
            }
            yield return null;
        }

        private static void AssertSilentFreshBlocks(float[] samples, int blocks)
        {
            for (int i = 0; i < blocks; ++i)
                Assert.That(PumpFreshPcm(samples, out _), Is.EqualTo(-1),
                    "An unserviced Post must not render before its actual publication.");
        }

        private static void AssertImmediatePcm(float[] samples)
        {
            // This fixture leaves EngineConfig's four-quantum publication lead at its default.
            const int leadQuanta = 4;
            int firstBlock = -1;
            for (int i = 0; i < leadQuanta + 2; ++i)
            {
                int firstSample = PumpFreshPcm(samples, out _);
                if (firstBlock < 0 && firstSample >= 0) firstBlock = i;
            }
            Assert.That(firstBlock, Is.EqualTo(leadQuanta),
                "The first real PCM block must use the service-time lead without an extra update.");
        }

        private static void AssertNaturalTailAfterPureAudio(Fixture fixture, ActionFiring firing, float[] samples)
        {
            ulong before = CaptureBlock();
            Assert.That(HarkenSdkTest_PumpNull(800), Is.EqualTo(VnResult.VN_OK));
            Assert.That(CaptureBlock(), Is.EqualTo(before + 800));
            Assert.That(PumpFreshPcm(samples, out _), Is.EqualTo(-1));
            bool observed = false;
            Action<HarkenAudioSystem, string, VnResult> observer = (owner, operation, result) =>
            {
                if (operation != "HarkenEngine.Update") return;
                Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                Assert.That(firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                if (info.state != (uint)VnActionFiringState.VN_ACTION_FIRING_TERMINAL_RETAINED) return;
                Assert.That(info.terminalReason, Is.EqualTo((uint)VnActionTerminalReason.VN_ACTION_TERMINAL_NATURAL));
                observed = true;
            };
            fixture.Runtime.OperationCompleted += observer;
            try
            {
                // Publication was already proven before EOF; these cycles retire completion/read fences.
                for (int i = 0; i < 16 && !observed; ++i)
                {
                    Invoke(fixture.Runtime, "AdvanceFrame");
                    if (!observed) Assert.That(PumpFreshPcm(samples, out _), Is.EqualTo(-1));
                }
            }
            finally { fixture.Runtime.OperationCompleted -= observer; }
            Assert.That(observed, Is.True);
            // Frame maintenance precedes the native update that first reports terminal completion.
            if (Pending(fixture.Runtime) != 0)
                Invoke(fixture.Runtime, "AdvanceFrame");
            Assert.That(Pending(fixture.Runtime), Is.Zero);
            Assert.That(PumpFreshPcm(samples, out _), Is.EqualTo(-1));
        }

        [UnityTest]
        public IEnumerator ManagedPostBeforeNormalUpdateStartsWithActualSourceAck()
            => ImmediatePostBeforeNormalUpdate(false);

        [UnityTest]
        public IEnumerator ComponentPostBeforeNormalUpdateStartsWithActualSourceAck()
            => ImmediatePostBeforeNormalUpdate(true);

        private static IEnumerator ImmediatePostBeforeNormalUpdate(bool component)
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                Assert.That(fixture.Owner.GetComponent<HarkenLateActionService>(), Is.Null);
                var samples = new float[4096];
                PrimeFreshPcmSnapshot(samples);
                HarkenAction action = component ? AddAction(fixture) : null;
                ActionFiring firing;
                if (component)
                {
                    Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                    firing = action.Firing;
                }
                else firing = fixture.Play();
                AssertSilentFreshBlocks(samples, 1);
                Invoke(fixture.Runtime, "AdvanceFrame");
                AssertImmediatePcm(samples);
                AssertNaturalTailAfterPureAudio(fixture, firing, samples);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ManagedLateUpdatePostWaitsForNextNormalUnityFrame()
            => ImmediatePostDuringLateUpdate(false);

        [UnityTest]
        public IEnumerator ComponentLateUpdatePostWaitsForNextNormalUnityFrame()
            => ImmediatePostDuringLateUpdate(true);

        private static IEnumerator ImmediatePostDuringLateUpdate(bool component)
        {
            using (var fixture = new Fixture(2))
            {
                fixture.Start();
                Assert.That(fixture.Owner.GetComponent<HarkenLateActionService>(), Is.Null);
                // An old serialized component remains harmless when its script identity is retained.
                fixture.Owner.AddComponent<HarkenLateActionService>();
                var samples = new float[4096];
                PrimeFreshPcmSnapshot(samples);
                HarkenAction action = component ? AddAction(fixture) : null;
                var probe = fixture.Owner.AddComponent<HarkenLateActionPostProbe>();
                var after = fixture.Owner.AddComponent<HarkenVeryLateActionPostProbe>();
                ActionFiring firing = default;
                int postFrame = -1;
                int observedFrame = -1;
                int updates = 0;
                int updatesAtPost = -1;
                Action<HarkenAudioSystem, string, VnResult> observer = (owner, operation, result) =>
                {
                    if (operation != "HarkenEngine.Update") return;
                    Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                    ++updates;
                };
                fixture.Runtime.OperationCompleted += observer;
                try
                {
                    probe.Trigger = () =>
                    {
                        postFrame = Time.frameCount;
                        updatesAtPost = updates;
                        if (component)
                        {
                            Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
                            firing = action.Firing;
                        }
                        else firing = fixture.Play();
                        AssertSilentFreshBlocks(samples, 1);
                    };
                    after.Trigger = () =>
                    {
                        Assert.That(Time.frameCount, Is.EqualTo(postFrame));
                        Assert.That(updates, Is.EqualTo(updatesAtPost));
                        AssertSilentFreshBlocks(samples, 7);
                        after.Trigger = () =>
                        {
                            observedFrame = Time.frameCount;
                            Assert.That(updates, Is.EqualTo(updatesAtPost + 1));
                            AssertImmediatePcm(samples);
                        };
                    };
                    for (int i = 0; i < 4 && observedFrame < 0; ++i) yield return null;
                    Assert.That(postFrame, Is.GreaterThanOrEqualTo(0));
                    Assert.That(probe.TriggerFrame, Is.EqualTo(postFrame));
                    Assert.That(observedFrame, Is.EqualTo(postFrame + 1));
                    Assert.That(after.TriggerFrame, Is.EqualTo(observedFrame));
                    Assert.That(firing.IsValid, Is.True);
                }
                finally { fixture.Runtime.OperationCompleted -= observer; }
                AssertNaturalTailAfterPureAudio(fixture, firing, samples);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator NativeNotificationShutdownRestartsAfterSdkUpdateReturns()
            => NotificationShutdownBoundary(true, false, "shutdown");

        [UnityTest]
        public IEnumerator NativeNotificationShutdownRestartsAfterExternalUpdateReturns()
            => NotificationShutdownBoundary(false, false, "shutdown");

        [UnityTest]
        public IEnumerator NestedUpdateCannotCompleteNotificationShutdownEarly()
            => NotificationShutdownBoundary(false, true, "shutdown");

        [UnityTest]
        public IEnumerator NativeNotificationDisableCompletesWithoutAnotherUnityUpdate()
            => NotificationShutdownBoundary(false, false, "disable");

        [UnityTest]
        public IEnumerator NativeNotificationDestroyCompletesWithoutAnotherUnityUpdate()
            => NotificationShutdownBoundary(false, false, "destroy");

        private static IEnumerator NotificationShutdownBoundary(bool sdkUpdate, bool nested, string exit)
        {
            bool originalPause = AudioListener.pause;
            float originalVolume = AudioListener.volume;
            using (var fixture = new Fixture(2))
            {
                fixture.Runtime.ConfigureLifecycleBeforeStartup(true, true);
                fixture.Start();
                PackBootstrapSession previous = fixture.Runtime.Session;
                fixture.Play();
                bool notified = false;
                bool inNotification = false;
                bool endedInNotification = false;
                bool retainedDuringNotification = false;
                bool audioRestoredAtEnd = false;
                bool cleanAtEnd = false;
                int ended = 0;
                int endedAfterNested = -1;
                VnResult shutdown = VnResult.VN_NOT_INITIALIZED;
                VnResult nestedResult = VnResult.VN_NOT_INITIALIZED;
                VnResult restart = VnResult.VN_NOT_INITIALIZED;
                VnResult prematureRestart = VnResult.VN_OK;
                VnResult repeatedShutdown = VnResult.VN_NOT_INITIALIZED;
                fixture.Runtime.RuntimeEnded += (owner, state, operation, result) =>
                {
                    if (++ended != 1) return;
                    endedInNotification = inNotification;
                    cleanAtEnd = owner.Session == null && Pending(owner) == 0 &&
                        state == HarkenRuntimeState.Stopped && result == VnResult.VN_OK;
                    audioRestoredAtEnd = AudioListener.pause == originalPause &&
                        AudioListener.volume == originalVolume;
                    if (exit == "shutdown") restart = owner.StartRuntime(fixture.Root);
                };
                fixture.Runtime.NotificationReceived += (in VnNotification notification, object state) =>
                {
                    if (notified || notification.type != (uint)VnNotificationType.VN_NOTIFY_DEGRADATION_OCCURRED) return;
                    notified = true;
                    inNotification = true;
                    try
                    {
                        if (exit == "disable") fixture.Runtime.enabled = false;
                        else if (exit == "destroy") Object.DestroyImmediate(fixture.Owner);
                        else shutdown = fixture.Runtime.ShutdownRuntime();
                        prematureRestart = fixture.Runtime.StartRuntime(fixture.Root);
                        repeatedShutdown = fixture.Runtime.ShutdownRuntime();
                        retainedDuringNotification = ReferenceEquals(fixture.Runtime.Session, previous) &&
                            Pending(fixture.Runtime) == 1 && AudioListener.pause && AudioListener.volume == 0;
                        if (nested)
                        {
                            nestedResult = HarkenEngine.Update();
                            endedAfterNested = ended;
                        }
                    }
                    finally { inNotification = false; }
                };
                // Inject only the notification into the real native queue; Update and Term remain production code.
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
                Assert.That(HarkenSdkTest_QueueNotification(), Is.EqualTo(VnResult.VN_OK));
                if (sdkUpdate) Invoke(fixture.Runtime, "AdvanceFrame");
                else Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                // Assertions stay outside native callbacks so failures cannot unwind through P/Invoke.
                Assert.That(notified, Is.True, "The injected notification must pass through native dispatch.");
                Assert.That(retainedDuringNotification, Is.True);
                Assert.That(endedInNotification, Is.False);
                Assert.That(ended, Is.EqualTo(1));
                Assert.That(prematureRestart, Is.Not.EqualTo(VnResult.VN_OK));
                Assert.That(repeatedShutdown, Is.EqualTo(VnResult.VN_OK));
                Assert.That(cleanAtEnd, Is.True);
                Assert.That(audioRestoredAtEnd, Is.True);
                Assert.That(previous.IsActive, Is.False);
                if (nested)
                {
                    // Term has detached the engine, but the outer Update still owns its teardown.
                    Assert.That(nestedResult, Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                    Assert.That(endedAfterNested, Is.Zero);
                }
                if (exit == "shutdown")
                {
                    Assert.That(shutdown, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(restart, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                    Assert.That(fixture.Runtime.Session.IsActive, Is.True);
                    Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                    fixture.Play();
                }
                else
                {
                    Assert.That(fixture.Runtime.Session, Is.Null);
                    Assert.That(Pending(fixture.Runtime), Is.Zero);
                    Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_NOT_INITIALIZED));
                }
            }
            Assert.That(AudioListener.pause, Is.EqualTo(originalPause));
            Assert.That(AudioListener.volume, Is.EqualTo(originalVolume));
            yield return null;
        }

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
                    Assert.That(HarkenSdkTest_PumpNull(4), Is.EqualTo(VnResult.VN_OK));
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
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
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
                    Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
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
