using System.Collections;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace Harken.Unity.Tests
{
    [DefaultExecutionOrder(10000)]
    public sealed class HarkenActionPlayerLoopProbe : MonoBehaviour
    {
        public HarkenAction Action { get; set; }
        public bool Trigger { get; set; }
        public VnResult Result { get; private set; } = VnResult.VN_INVALID_ARG;
        public int TriggerFrame { get; private set; } = -1;

        private void Update()
        {
            if (!Trigger || Action == null)
            {
                return;
            }
            Trigger = false;
            TriggerFrame = Time.frameCount;
            Result = Action.Play();
        }
    }

    public sealed class HarkenLifecycleTests
    {
        [UnityTest]
        public IEnumerator RuntimeReachesReadyAndShutsDownOnNullDevice()
        {
            InstallTestPacks();
            var settingsLease = HarkenProjectSettingsLease.Acquire();
            settingsLease.Settings.SetPackDirectory(Path.Combine(Application.streamingAssetsPath, "Audio", "GeneratedPacks"));
            GameObject owner = new GameObject("Harken Runtime Test");
            owner.SetActive(false);
            HarkenAudioSystem runtime = owner.AddComponent<HarkenAudioSystem>();
            try
            {
                Assert.That(runtime.ConfigureBeforeStartup(
                    "Audio/GeneratedPacks", "Init.bytes",
                    System.Array.Empty<ulong>(), true, false),
                    Is.EqualTo(VnResult.VN_OK));

                owner.SetActive(true);
                yield return null;
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Ready));
                Assert.That(runtime.LastResult, Is.EqualTo(VnResult.VN_OK));

                Assert.That(runtime.SetSuspended(true), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Suspended));
                Assert.That(runtime.LastOperation, Is.EqualTo("HarkenEngine.Suspend"));
                Assert.That(runtime.SetSuspended(false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Ready));
                Assert.That(runtime.LastOperation, Is.EqualTo("HarkenEngine.Resume"));

                VnResult shutdown = runtime.ShutdownRuntime();
                Assert.That(shutdown, Is.EqualTo(VnResult.VN_OK));
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
            }
            finally
            {
                try
                {
                    if (runtime.Session != null && runtime.Session.IsActive)
                        Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                }
                finally
                {
                    Object.DestroyImmediate(owner);
                    settingsLease.Dispose();
                }
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator SuspendedRuntimePreservesQueuedPackUntilResume()
        {
            InstallTestPacks();
            var settingsLease = HarkenProjectSettingsLease.Acquire();
            settingsLease.Settings.SetPackDirectory(Path.Combine(Application.streamingAssetsPath, "Audio", "GeneratedPacks"));
            GameObject owner = new GameObject("Harken Runtime Suspend Test");
            owner.SetActive(false);

            HarkenAudioSystem runtime = owner.AddComponent<HarkenAudioSystem>();
            HarkenPack pack = owner.AddComponent<HarkenPack>();
            try
            {
                Assert.That(runtime.ConfigureBeforeStartup(
                    "Audio/GeneratedPacks", "Init.bytes",
                    System.Array.Empty<ulong>(), true, false),
                    Is.EqualTo(VnResult.VN_OK));
                SetPrivateField(pack, "runtime", runtime);
                SetPrivateField(pack, "packStableId", ReadSampleRandomOwner());

                owner.SetActive(true);
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Ready));
                Assert.That(runtime.PendingPackLoadCount, Is.EqualTo(1));
                Assert.That(runtime.SetSuspended(true), Is.EqualTo(VnResult.VN_OK));

                yield return null;

                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Suspended));
                Assert.That(runtime.PendingPackLoadCount, Is.EqualTo(1));
                Assert.That(pack.IsLoaded, Is.False);
                Assert.That(runtime.SetSuspended(false), Is.EqualTo(VnResult.VN_OK));

                yield return null;

                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Ready));
                Assert.That(runtime.PendingPackLoadCount, Is.Zero);
                Assert.That(pack.IsLoaded, Is.True);
                Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
            }
            finally
            {
                try
                {
                    if (runtime.Session != null && runtime.Session.IsActive)
                        Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                }
                finally
                {
                    Object.DestroyImmediate(owner);
                    settingsLease.Dispose();
                }
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ComponentActionPostsFromPlayerLoopAfterRuntimePump()
        {
            using (var fixture = new NameFirstFixture())
            {
                fixture.Start(true);
                HarkenActionPlayerLoopProbe probe =
                    fixture.Source.AddComponent<HarkenActionPlayerLoopProbe>();
                probe.Action = fixture.Action;
                yield return null;
                Assert.That(fixture.Runtime.IsPreparationComplete, Is.True);
                Assert.That(fixture.Pack.IsLoaded, Is.True);
                probe.Trigger = true;
                yield return null;
                Assert.That(probe.TriggerFrame, Is.GreaterThanOrEqualTo(0));
                Assert.That(probe.Result, Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Action.Firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                Assert.That(info.actionId, Is.EqualTo(fixture.ActionId));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ComponentValidatesIdentityThenPreparesColdContent()
        {
            using (var fixture = new NameFirstFixture())
            {
                fixture.Start(false);
                yield return null;
                Assert.That(fixture.Pack.IsLoaded, Is.False);
                Assert.That(fixture.Runtime.PendingPackLoadCount, Is.Zero);
                Assert.That(Actions.Resolve(fixture.Name, fixture.ProjectId, out ActionResolved initial),
                    Is.EqualTo(VnResult.VN_OK));
                ulong initialGeneration = initial.PublicationGeneration;

                SetPrivateField(fixture.Action, "actionName", "Missing_Action_For_Component_Test");
                ExpectActionFailure(VnResult.VN_UNKNOWN_ID);
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_UNKNOWN_ID));
                SetPrivateField(fixture.Action, "actionName", fixture.Name);
                SetPrivateField(fixture.Action, "projectId", "01234567-89ab-cdef-8123-456789abcdef");
                ExpectActionFailure(VnResult.VN_GAMEPLAY_WRONG_PROJECT);
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_GAMEPLAY_WRONG_PROJECT));
                SetPrivateField(fixture.Action, "projectId", fixture.ProjectId);
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Pack.IsLoaded, Is.False, "The Action owns its own preparation request.");
                Assert.That(GetPrivateField<ActionResolved>(fixture.Action, "cachedResolved").PublicationGeneration,
                    Is.Not.EqualTo(initialGeneration));
                Assert.That(fixture.Action.Firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                Assert.That(info.actionId, Is.EqualTo(fixture.ActionId));
                Assert.That(fixture.Action.ActionName, Is.EqualTo(fixture.Name));
                Assert.That(fixture.Action.ProjectId, Is.EqualTo(fixture.ProjectId));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator AutomaticColdPlayPreparesOnceAndDoesNotReplayWhenAnotherOwnerPreloads()
        {
            using (var fixture = new NameFirstFixture())
            {
                fixture.Start(false, true);
                int attempts = 0;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (operation == "Actions.PostResolved" || operation.StartsWith("Action '")) ++attempts;
                };
                yield return null;
                Assert.That(attempts, Is.EqualTo(1));
                Assert.That(fixture.Action.LastResult, Is.EqualTo(VnResult.VN_OK));
                ActionFiring first = fixture.Action.Firing;
                Assert.That(first.IsValid, Is.True);
                Assert.That(GetPrivateField<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(1UL));
                Assert.That(fixture.Pack.Load(), Is.EqualTo(VnResult.VN_OK));
                for (int i = 0; i < 4; ++i) yield return null;
                Assert.That(attempts, Is.EqualTo(1));
                Assert.That(fixture.Action.LastResult, Is.EqualTo(VnResult.VN_OK));
                Assert.That(GetPrivateField<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(1UL));
                Assert.That(fixture.Action.Firing.Handle, Is.EqualTo(first.Handle));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator AutomaticPlayWaitsForExplicitPreparation()
        {
            using (var fixture = new NameFirstFixture())
            {
                fixture.Start(true, true);
                Assert.That(fixture.Runtime.PendingPackLoadCount, Is.EqualTo(1));
                Assert.That(fixture.Runtime.IsPreparationComplete, Is.False);
                Assert.That(GetPrivateField<ulong>(fixture.Action, "callerSeed"), Is.Zero);
                yield return null;
                Assert.That(fixture.Runtime.IsPreparationComplete, Is.True);
                Assert.That(fixture.Action.LastResult, Is.EqualTo(VnResult.VN_OK));
                Assert.That(GetPrivateField<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(1UL));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ExplicitRefusalSurvivesExistingFiringObservation()
        {
            using (var fixture = new NameFirstFixture())
            {
                fixture.Start(true);
                yield return null;
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                ExpectActionFailure(VnResult.VN_INVALID_ARG);
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_INVALID_ARG));
                yield return null;
                yield return null;
                Assert.That(fixture.Action.LastResult, Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(GetPrivateField<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(1UL));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator AutomaticAdmissionRefusalDoesNotRetryOrAdvanceSeed()
        {
            using (var fixture = new NameFirstFixture())
            {
                HarkenProjectSettings.ActiveForStartup().SetMaxActionFirings(1);
                fixture.Start(true);
                yield return null;
                Assert.That(Actions.Resolve(fixture.Name, fixture.ProjectId, out ActionResolved resolved),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post(in resolved, fixture.Emitter.Handle, 0, out ActionFiring retained),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(retained.IsValid, Is.True);
                fixture.Action.enabled = false;
                SetPrivateField(fixture.Action, "playOnEnable", true);
                fixture.Action.enabled = true;
                int attempts = 0;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (operation == "Actions.PostResolved" || operation.StartsWith("Action '")) ++attempts;
                };
                ExpectActionFailure(VnResult.VN_NO_ACTION);
                for (int i = 0; i < 5; ++i) yield return null;
                Assert.That(attempts, Is.EqualTo(1));
                Assert.That(fixture.Action.LastResult, Is.EqualTo(VnResult.VN_NO_ACTION));
                Assert.That(fixture.Action.IsPlaying, Is.False);
                Assert.That(GetPrivateField<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(1UL));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ExplicitPackRequestsShareUntilTheirLastRelease()
        {
            using (var fixture = new NameFirstFixture())
            {
                fixture.Start(false);
                GameObject otherOwner = new GameObject("Second explicit request");
                otherOwner.SetActive(false);
                otherOwner.transform.SetParent(fixture.Owner.transform);
                HarkenPack other = otherOwner.AddComponent<HarkenPack>();
                SetPrivateField(other, "runtime", fixture.Runtime);
                SetPrivateField(other, "packStableId", fixture.Pack.PackStableId);
                SetPrivateField(other, "loadOnReady", false);
                otherOwner.SetActive(true);
                Assert.That(fixture.Pack.Load(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(other.Load(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(other.Handle.Handle, Is.EqualTo(fixture.Pack.Handle.Handle));
                fixture.Pack.enabled = false;
                Assert.That(fixture.Pack.IsLoaded, Is.False);
                Assert.That(other.IsLoaded, Is.True);
                yield return null;
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(other.Unload(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(other.IsLoaded, Is.False);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator UnknownStartupPackRejectsTheWholeSessionAndAllowsRetry()
        {
            using (var fixture = new NameFirstFixture())
            {
                const string relativeRoot = "Audio/NameFirstPacks";
                string root = Path.Combine(Application.streamingAssetsPath, relativeRoot);
                Assert.That(GameplayPublication.Read(root, "English", out GameplayPublication publication,
                    out string error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindPack(ulong.MaxValue, out _), Is.False);
                ulong ownerId = publication.Actions[0].OwnerPackStableId;
                HarkenProjectSettings.ActiveForStartup().SetPackDirectory(root);
                Assert.That(fixture.Runtime.ConfigureBeforeStartup(relativeRoot, publication.InitPack.Path,
                    new[] { ownerId, ulong.MaxValue }, true, false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.ConfigureLanguageBeforeStartup("English"), Is.EqualTo(VnResult.VN_OK));
                LogAssert.Expect(LogType.Error, new Regex("PackBootstrap.Start: VN_UNKNOWN_ID"));
                VnResult startupResult = VnResult.VN_OK;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (operation == "PackBootstrap.Start") startupResult = result;
                };
                fixture.Owner.SetActive(true);
                Assert.That(startupResult, Is.EqualTo(VnResult.VN_UNKNOWN_ID));
                Assert.That(fixture.Runtime.Session, Is.Null);
                Assert.That(fixture.Runtime.IsReady, Is.False);
                Assert.That(GetPrivateField<object>(fixture.Runtime, "notificationRegistration"), Is.Null);
                Assert.That(GetPrivateField<object>(fixture.Runtime, "logRegistration"), Is.Null);
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(fixture.Runtime.Session, Is.Null);
                Assert.That(fixture.Runtime.IsReady, Is.False);
                Assert.That(fixture.Runtime.ConfigureBeforeStartup(relativeRoot, publication.InitPack.Path,
                    new[] { ownerId }, true, false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.StartRuntime(root), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
                Assert.That(fixture.Runtime.Session.IsStartupComplete, Is.True);
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator RuntimeLanguageSelectsInitAndEveryLaterExplicitLoad()
        {
            foreach (string language in new[] { "English", "ja" })
            {
                using (var fixture = new NameFirstFixture())
                {
                    fixture.Start(false, false, language);
                    Assert.That(fixture.Runtime.Language, Is.EqualTo(language));
                    Assert.That(fixture.Runtime.Session.Language, Is.EqualTo(language));
                    Assert.That(fixture.Pack.Load(), Is.EqualTo(VnResult.VN_OK));
                    yield return null;
                    LogAssert.Expect(LogType.Error, new Regex("ConfigureLanguageBeforeStartup: VN_INVALID_ARG"));
                    Assert.That(fixture.Runtime.ConfigureLanguageBeforeStartup(language == "ja" ? "English" : "ja"),
                        Is.EqualTo(VnResult.VN_INVALID_ARG));
                    Assert.That(fixture.Runtime.Language, Is.EqualTo(language));
                    Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Action.Firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(info.actionId, Is.EqualTo(fixture.ActionId));
                }
                yield return null;
            }
        }

        [UnityTest]
        public IEnumerator MissingSelectedContentRetainsTheSessionAndAllowsExplicitRecovery()
        {
            string source = Path.Combine(Application.streamingAssetsPath, "Audio/NameFirstPacks");
            string target = Path.Combine(Application.temporaryCachePath,
                "HarkenPartialPack_" + System.Guid.NewGuid().ToString("N"));
            PackBootstrapSession session = null;
            Assert.That(Directory.Exists(target), Is.False);
            try
            {
                foreach (string file in Directory.GetFiles(source, "*", SearchOption.AllDirectories))
                {
                    string destination = Path.Combine(target, Path.GetRelativePath(source, file));
                    Directory.CreateDirectory(Path.GetDirectoryName(destination));
                    File.Copy(file, destination);
                }
                Assert.That(PackManifest.TryReadManifest(target, "English", out _,
                    out PackManifestEntry[] packs, out _, out ulong initId), Is.True);
                PackManifestEntry init = System.Array.Find(packs, entry => entry.PackStableId == initId);
                PackManifestEntry content = System.Array.Find(packs, entry => entry.PackStableId != initId);
                string contentPath = Path.Combine(target, content.Path);
                File.Delete(contentPath);
                Assert.That(PackManifest.TryReadManifest(target, "English", out _, out _, out _), Is.True);
                Assert.That(PackManifest.TryLoadManifest(target, "English", out _, out _), Is.False);
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                Assert.That(PackBootstrap.Start(Path.Combine(target, init.Path), target,
                    new[] { content.PackStableId }, "English", ref config, out session),
                    Is.EqualTo(VnResult.VN_CONTENT_NOT_LOADED));
                Assert.That(session, Is.Not.Null);
                Assert.That(session.IsActive, Is.True);
                Assert.That(session.IsStartupComplete, Is.False);
                Assert.That(session.FailedStartupPackStableId, Is.EqualTo(content.PackStableId));
                File.Copy(Path.Combine(source, content.Path), contentPath);
                Assert.That(session.PrepareStartupPacks(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(session.IsActive, Is.True);
                Assert.That(session.IsStartupComplete, Is.True);
                Assert.That(session.StartupPacks[0].IsValid, Is.True);
                Assert.That(GameplayPublication.Read(target, "English", out GameplayPublication publication,
                    out string error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(Actions.Resolve(publication.Actions[0].Name,
                    publication.ProjectId, out ActionResolved resolved), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post(in resolved, Emitter.None, 0, out ActionFiring firing), Is.EqualTo(VnResult.VN_OK));
                Assert.That(firing.IsValid, Is.True);
            }
            finally
            {
                session?.Dispose();
                if (Directory.Exists(target)) Directory.Delete(target, true);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ExplicitRestartRecreatesParticipantsWithoutAutomaticReplay()
        {
            using (var fixture = new NameFirstFixture())
            {
                fixture.Start(true, true);
                yield return null;
                PackBootstrapSession previous = fixture.Runtime.Session;
                Assert.That(GetPrivateField<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(1UL));
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(previous.IsActive, Is.False);
                Assert.That(fixture.Pack.IsLoaded, Is.False);
                Assert.That(fixture.Emitter.IsCreated, Is.False);
                Assert.That(fixture.Runtime.StartRuntime(Path.Combine(Application.streamingAssetsPath,
                    "Audio/NameFirstPacks")), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                yield return null;
                Assert.That(fixture.Pack.IsLoaded, Is.True);
                Assert.That(fixture.Emitter.IsCreated, Is.True);
                Assert.That(GetPrivateField<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(1UL));
                Assert.That(fixture.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(GetPrivateField<ulong>(fixture.Action, "callerSeed"), Is.EqualTo(2UL));
            }
            yield return null;
        }

        private sealed class NameFirstFixture : System.IDisposable
        {
            public readonly GameObject Owner = new GameObject("Name-first Runtime");
            public readonly GameObject Source = new GameObject("Name-first Action");
            public readonly HarkenAudioSystem Runtime;
            public readonly HarkenPack Pack;
            public readonly HarkenEmitter Emitter;
            public readonly HarkenAction Action;
            public string Name { get; private set; }
            public string ProjectId { get; private set; }
            public VnObjectId ActionId { get; private set; }

            private readonly HarkenProjectSettingsLease settingsLease = HarkenProjectSettingsLease.Acquire();

            public NameFirstFixture()
            {
                Owner.SetActive(false);
                Source.SetActive(false);
                Runtime = Owner.AddComponent<HarkenAudioSystem>();
                Pack = Owner.AddComponent<HarkenPack>();
                Emitter = Source.AddComponent<HarkenEmitter>();
                Action = Source.AddComponent<HarkenAction>();
            }

            public void Start(bool loadOnReady, bool playOnEnable = false, string language = null,
                bool loadAtStartup = false)
            {
                const string relativeRoot = "Audio/NameFirstPacks";
                string root = Path.Combine(Application.streamingAssetsPath, relativeRoot);
                Assert.That(PackManifest.TryResolvePlatformDirectory(root, null, null, "Windows", language,
                    out string platformRoot, out string error), Is.True, error);
                Assert.That(GameplayPublication.Read(platformRoot, language,
                    out GameplayPublication publication, out error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.ValidateInit(File.ReadAllBytes(
                    Path.Combine(platformRoot, publication.InitPack.Path)), out error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.Actions, Is.Not.Empty);
                global::Harken.GameplayAction selected = publication.Actions[0];
                Name = selected.Name;
                ProjectId = publication.ProjectId;
                ActionId = selected.ActionStableId;
                ulong ownerId = selected.OwnerPackStableId;
                ulong[] startup = loadAtStartup ? new[] { ownerId } : System.Array.Empty<ulong>();
                settingsLease.Settings.SetPackDirectory(root);
                Assert.That(Runtime.ConfigureBeforeStartup(relativeRoot, publication.InitPack.Path, startup, true, false),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(Runtime.ConfigureLanguageBeforeStartup(language), Is.EqualTo(VnResult.VN_OK));
                SetPrivateField(Pack, "runtime", Runtime);
                SetPrivateField(Pack, "packStableId", ownerId);
                SetPrivateField(Pack, "loadOnReady", loadOnReady);
                SetPrivateField(Emitter, "runtime", Runtime);
                SetPrivateField(Action, "runtime", Runtime);
                SetPrivateField(Action, "emitter", Emitter);
                SetPrivateField(Action, "actionName", Name);
                SetPrivateField(Action, "projectId", ProjectId);
                SetPrivateField(Action, "playOnEnable", playOnEnable);
                Owner.SetActive(true);
                Source.SetActive(true);
                Assert.That(Runtime.IsReady, Is.True);
            }

            public void Dispose()
            {
                if (Runtime.Session != null && Runtime.Session.IsActive)
                    Assert.That(Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Object.Destroy(Source);
                Object.Destroy(Owner);
                settingsLease.Dispose();
            }
        }

        private static void ExpectActionFailure(VnResult result)
            => LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: Action .*" + result));

        private static T GetPrivateField<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);

        private static void InstallTestPacks()
        {
            string target = Path.Combine(
                Application.streamingAssetsPath, "Audio", "GeneratedPacks");
            Assert.That(
                File.Exists(Path.Combine(target, "HarkenPackIndex.json")) &&
                File.Exists(Path.Combine(target, "Windows", "Init.bytes")),
                "Playable Demo Packs must be installed before PlayMode tests.");
        }

        private static ulong ReadSampleRandomOwner()
        {
            string root = Path.Combine(Application.streamingAssetsPath, "Audio", "GeneratedPacks");
            Assert.That(PackManifest.TryReadPlatformDirectory(root, null, null,
                HarkenAudioSystem.CurrentPackBasePlatform(), null, out string directory, out string error), Is.True, error);
            Assert.That(GameplayPublication.Read(directory, null, out GameplayPublication publication, out error),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(publication.ValidateInit(File.ReadAllBytes(Path.Combine(directory, publication.InitPack.Path)), out error),
                Is.EqualTo(VnResult.VN_OK), error);
            Assert.That(publication.TryFindAction("Play_Random_Draw", out global::Harken.GameplayAction action), Is.True);
            return action.OwnerPackStableId;
        }

        private static void SetPrivateField<T>(
            object target, string name, T value)
        {
            FieldInfo field = target.GetType().GetField(
                name, BindingFlags.Instance | BindingFlags.NonPublic);
            Assert.That(field, Is.Not.Null, "Missing field " + name);
            field.SetValue(target, value);
        }
    }
}
