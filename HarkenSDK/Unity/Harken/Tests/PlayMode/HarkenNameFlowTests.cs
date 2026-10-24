using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenNameFlowTests
    {
        [UnityTest]
        public IEnumerator CompanionDirectoryPreparesBothPlaybackEntrypointsOnFirstUse()
            => CompanionFirstUse(false);

        [UnityTest]
        public IEnumerator ProjectCompanionDirectoryPreparesBothPlaybackEntrypointsOnFirstUse()
            => CompanionFirstUse(true);

        [UnityTest]
        public IEnumerator CompanionDirectoryHonorsExplicitPlatformSelection()
        {
            using (var fixture = new Fixture())
            {
                string root = fixture.StartCompanion(false);
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.ConfigurePlatformBeforeStartup(null, "DifferentPlatform"), Is.EqualTo(VnResult.VN_OK));
                LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: PackBootstrap.Start: VN_INVALID_ARG"));
                Assert.That(fixture.Runtime.StartRuntime(root), Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(fixture.Runtime.IsReady, Is.False);
                LogAssert.Expect(LogType.Error, new Regex("HarkenAudioSystem.ConfigurePlatformBeforeStartup: VN_INVALID_ARG"));
                Assert.That(fixture.Runtime.ConfigurePlatformBeforeStartup("11111111-2222-3333-4444-555555555555"), Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(fixture.Runtime.ConfigurePlatformBeforeStartup(null, Path.GetFileName(root)), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.StartRuntime(Path.GetDirectoryName(root)), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.IsReady, Is.True);
            }
            yield return null;
        }

        private static IEnumerator CompanionFirstUse(bool projectStartup)
        {
            using (var fixture = new Fixture())
            {
                string root = fixture.StartCompanion(projectStartup);
                Assert.That(fixture.Runtime.Session.Publication, Is.Null);
                Assert.That(fixture.Runtime.Session.ProjectId, Is.EqualTo(fixture.Publication.ProjectId));
                Assert.That(fixture.Runtime.Session.StartupPacks, Is.Empty);
                Assert.That(Actions.Resolve("Play_U4_A", out _), Is.Not.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Resolve("Play_U4_B", out _), Is.Not.EqualTo(VnResult.VN_OK));
                AssertAction(Post(fixture.Action), fixture.Row("Play_U4_A").ActionStableId);
                Assert.That(fixture.Runtime.PlayAction("Play_U4_B", fixture.Publication.ProjectId, out ActionFiring managed),
                    Is.EqualTo(VnResult.VN_OK));
                AssertAction(managed, fixture.Row("Play_U4_B").ActionStableId);

                // Both owners remain usable while their source files are unavailable.
                foreach (string file in Directory.GetFiles(root)) File.Delete(file);
                Assert.That(fixture.Runtime.PlayAction("Play_U4_A", fixture.Publication.ProjectId, out ActionFiring warm),
                    Is.EqualTo(VnResult.VN_OK));
                AssertAction(warm, fixture.Row("Play_U4_A").ActionStableId);
                PackBootstrapSession previous = fixture.Runtime.Session;
                Assert.That(fixture.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(previous.IsActive, Is.False);
                Assert.That(warm.Query(out _), Is.Not.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator WarmNameEditUsesTheNewActionWithoutDisablingOrRestarting()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                PackBootstrapSession session = fixture.Runtime.Session;
                yield return NewBlock(fixture, CaptureBlock());
                ActionFiring first = Post(fixture.Action);
                AssertAction(first, fixture.Row("Play_U4_A").ActionStableId);
                yield return NewBlock(fixture, CaptureBlock());
                Assert.That(first.Query(out VnActionFiringInfo active), Is.EqualTo(VnResult.VN_OK));
                Assert.That(active.livePlayableCount, Is.GreaterThan(0));
                yield return StopAndRelease(fixture, fixture.Action);
                Assert.That(Get<bool>(fixture.Action, "cacheValid"), Is.True);
                ActionResolved cached = Get<ActionResolved>(fixture.Action, "cachedResolved");
                Assert.That(cached.ActionId, Is.EqualTo(fixture.Row("Play_U4_A").ActionStableId));
                Set(fixture.Action, "actionName", "Play_U4_B");
                Assert.That(Get<bool>(fixture.Action, "cacheValid"), Is.True);
                Assert.That(Get<ActionResolved>(fixture.Action, "cachedResolved").ActionId, Is.EqualTo(cached.ActionId));
                ActionFiring next = Post(fixture.Action);
                AssertAction(next, fixture.Row("Play_U4_B").ActionStableId);
                Assert.That(fixture.Action.isActiveAndEnabled, Is.True);
                Assert.That(fixture.Runtime.Session, Is.SameAs(session));
                fixture.AssertPreparationUnchanged();
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator MultiplePacksRemainPreparedAcrossIndependentABAPlaybacks()
        {
            using (var fixture = new Fixture())
            {
                HarkenAction actionB = fixture.AddAction("Play_U4_B");
                HarkenAction anotherA = fixture.AddAction("Play_U4_A");
                fixture.Start();
                yield return NewBlock(fixture, CaptureBlock());
                ActionFiring first = Post(fixture.Action);
                fixture.AssertPreparationUnchanged();
                ActionFiring second = Post(actionB);
                fixture.AssertPreparationUnchanged();
                ActionFiring third = Post(anotherA);
                fixture.AssertPreparationUnchanged();
                yield return NewBlock(fixture, CaptureBlock());
                Assert.That(new[] { first.Handle, second.Handle, third.Handle }.Distinct().Count(), Is.EqualTo(3));
                AssertAction(first, fixture.Row("Play_U4_A").ActionStableId);
                AssertAction(second, fixture.Row("Play_U4_B").ActionStableId);
                AssertAction(third, fixture.Row("Play_U4_A").ActionStableId);
                yield return StopAndRelease(fixture, fixture.Action);
                AssertAction(second, fixture.Row("Play_U4_B").ActionStableId);
                AssertAction(third, fixture.Row("Play_U4_A").ActionStableId);
                Assert.That(actionB.Firing.Handle, Is.EqualTo(second.Handle));
                Assert.That(anotherA.Firing.Handle, Is.EqualTo(third.Handle));
                fixture.AssertPreparationUnchanged();
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator AnotherProjectRejectsPersistedConfigurationUntilExplicitRebind()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                yield return NewBlock(fixture, CaptureBlock());
                Post(fixture.Action);
                string persistedProject = fixture.Action.ProjectId;
                fixture.Restart("U4OtherProject");
                yield return NewBlock(fixture, CaptureBlock());
                Assert.That(fixture.Action.ProjectId, Is.EqualTo(persistedProject));
                ExpectFailure(fixture.Action, VnResult.VN_GAMEPLAY_WRONG_PROJECT);
                Assert.That(fixture.Action.ActionName, Is.EqualTo("Play_U4_A"));
                Assert.That(fixture.Action.ProjectId, Is.EqualTo(persistedProject));
                Set(fixture.Action, "projectId", fixture.Publication.ProjectId);
                AssertAction(Post(fixture.Action), fixture.Row("Play_U4_A").ActionStableId);
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));

            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator RestartWithRenameRefusesThePersistedOldNameUntilExplicitlyEdited()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                yield return NewBlock(fixture, CaptureBlock());
                AssertAction(Post(fixture.Action), fixture.Row("Play_U4_A").ActionStableId);
                Assert.That(Get<bool>(fixture.Action, "cacheValid"), Is.True);
                string project = fixture.Action.ProjectId;
                fixture.Restart("U4Renamed");
                yield return NewBlock(fixture, CaptureBlock());
                Assert.That(fixture.Action.ActionName, Is.EqualTo("Play_U4_A"));
                Assert.That(fixture.Action.ProjectId, Is.EqualTo(project));
                ExpectFailure(fixture.Action, VnResult.VN_CONTENT_NOT_LOADED);
                Assert.That(fixture.Action.ActionName, Is.EqualTo("Play_U4_A"));
                Assert.That(fixture.Action.Firing.IsValid, Is.False);
                Set(fixture.Action, "actionName", "Play_U4_Renamed");
                AssertAction(Post(fixture.Action), fixture.Row("Play_U4_Renamed").ActionStableId);
                fixture.AssertPreparationUnchanged();
            }
            yield return null;
        }

        private static ActionFiring Post(HarkenAction action)
        {
            Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(action.Play(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(action.Firing.IsValid, Is.True);
            return action.Firing;
        }

        private static void ExpectFailure(HarkenAction action, VnResult expected)
        {
            Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
            LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: Action .*" + expected));
            Assert.That(action.Play(), Is.EqualTo(expected));
            Assert.That(action.Firing.IsValid, Is.False);
        }

        private static void AssertAction(ActionFiring firing, VnObjectId expectedId)
        {
            Assert.That(firing.Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
            Assert.That(info.actionId, Is.EqualTo(expectedId));
        }

        private static IEnumerator StopAndRelease(Fixture fixture, HarkenAction action)
        {
            PackBootstrapSession session = fixture.Runtime.Session;
            ActionFiring firing = action.Firing;
            ulong baseline = CaptureBlock();
            Assert.That(action.Stop(), Is.EqualTo(VnResult.VN_OK));
            float deadline = Time.realtimeSinceStartup + 3;
            while (Time.realtimeSinceStartup < deadline)
            {
                yield return null;
                Assert.That(fixture.Runtime.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                if (CaptureBlock() > baseline && !action.Firing.IsValid)
                {
                    Assert.That(firing.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                    yield break;
                }
            }
            Assert.Fail("The same session did not publish a newer block and release the stopped firing.");
        }

        private static IEnumerator NewBlock(Fixture fixture, ulong baseline)
        {
            PackBootstrapSession session = fixture.Runtime.Session;
            float deadline = Time.realtimeSinceStartup + 3;
            while (Time.realtimeSinceStartup < deadline)
            {
                yield return null;
                Assert.That(fixture.Runtime.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
                if (CaptureBlock() > baseline) yield break;
            }
            Assert.Fail("No fresh block was published by the same real-device session.");
        }

        private static ulong CaptureBlock()
        {
            VnSnapshot snapshot = HarkenEngine.DefaultSnapshot();
            Assert.That(HarkenEngine.Capture(ref snapshot), Is.EqualTo(VnResult.VN_OK));
            return snapshot.blockIndex;
        }

        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(target);

        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);

        private sealed class Fixture : IDisposable
        {
            public readonly GameObject Owner = new GameObject("Name flow Runtime");
            public readonly GameObject Source = new GameObject("Name flow source");
            public readonly HarkenAudioSystem Runtime;
            public readonly HarkenEmitter Emitter;
            public readonly HarkenAction Action;
            private readonly List<HarkenAction> actions = new List<HarkenAction>();
            private ulong[] preparedHandles;
            private ulong generation;
            private string companionRoot;
            public CompanionTestPublication Publication { get; private set; }

            private readonly HarkenProjectSettingsLease settingsLease = HarkenProjectSettingsLease.Acquire();

            public Fixture()
            {
                Owner.SetActive(false);
                Source.SetActive(false);
                Runtime = Owner.AddComponent<HarkenAudioSystem>();
                Emitter = Source.AddComponent<HarkenEmitter>();
                Set(Emitter, "runtime", Runtime);
                Action = AddAction("Play_U4_A");
            }

            public HarkenAction AddAction(string name)
            {
                var action = Source.AddComponent<HarkenAction>();
                Set(action, "runtime", Runtime);
                Set(action, "emitter", Emitter);
                Set(action, "actionName", name);
                actions.Add(action);
                return action;
            }

            public void Start()
            {
                Configure("U4Original");
                foreach (HarkenAction action in actions) Set(action, "projectId", Publication.ProjectId);
                Owner.SetActive(true);
                AfterStartup();
                Source.SetActive(true);
            }

            public string StartCompanion(bool projectStartup)
            {
                string original = Path.Combine(Application.streamingAssetsPath, "Audio", "U4Original");
                Assert.That(CompanionTestPublication.Read(original, null, out CompanionTestPublication publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Publication = publication;
                companionRoot = Path.Combine(Application.streamingAssetsPath, "harken-companion-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(companionRoot);
                foreach (string file in Directory.GetFiles(original))
                    File.Copy(file, Path.Combine(companionRoot, Path.GetFileName(file)));
                File.WriteAllText(Path.Combine(companionRoot, "Unused.json"), "invalid");
                settingsLease.Settings.SetPackDirectory(companionRoot);
                settingsLease.Settings.SetPlayerExports(new[] { Path.GetFileName(companionRoot) });
                Set(Runtime, "autoInitialize", projectStartup);
                Assert.That(Runtime.ConfigureBeforeStartup("Audio/Companion", "Init.bytes", Array.Empty<ulong>(), true, false),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(Runtime.ConfigurePlatformBeforeStartup(null), Is.EqualTo(VnResult.VN_OK));
                Set(Action, "projectId", publication.ProjectId);
                Owner.SetActive(true);
                if (!projectStartup)
                    Assert.That(Runtime.StartRuntime(companionRoot), Is.EqualTo(VnResult.VN_OK), Runtime.LastStartupError);
                Assert.That(Runtime.IsReady, Is.True, Runtime.LastStartupError);
                Source.SetActive(true);
                return companionRoot;
            }

            public void Restart(string directory)
            {
                PackBootstrapSession previous = Runtime.Session;
                Assert.That(Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(previous.IsActive, Is.False);
                Configure(directory);
                Assert.That(Runtime.StartRuntime(Path.Combine(Application.streamingAssetsPath, "Audio", directory)),
                    Is.EqualTo(VnResult.VN_OK), Runtime.LastStartupError);
                Assert.That(Runtime.Session, Is.Not.SameAs(previous));
                AfterStartup();
                Assert.That(Emitter.IsCreated, Is.True);
            }

            private void Configure(string directory)
            {
                string root = Path.Combine(Application.streamingAssetsPath, "Audio", directory);
                Assert.That(CompanionTestPublication.Read(root, null, out CompanionTestPublication publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Publication = publication;
                settingsLease.Settings.SetPackDirectory(root);
                Assert.That(Runtime.ConfigureBeforeStartup("Audio/" + directory, Publication.InitPack.Path,
                    Array.Empty<ulong>(), false, false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Runtime.ConfigurePlatformBeforeStartup(null), Is.EqualTo(VnResult.VN_OK));
            }

            private void AfterStartup()
            {
                Assert.That(Runtime.IsPreparationComplete, Is.True, Runtime.LastStartupError);
                Assert.That(HarkenNative.Vn_SetBusMute(VnConstants.VN_BUS_MASTER, 1, 0), Is.EqualTo(VnResult.VN_IN_USE));
                Assert.That(Runtime.Session.PrepareActions(Publication.Actions.Where(row => row.Name != "Play_U4_Caller")
                    .Select(row => row.Name).ToArray(), Publication.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                preparedHandles = Runtime.Session.StartupPacks.Select(pack => pack.Handle).ToArray();
                Assert.That(Actions.Resolve("Play_U4_B", Publication.ProjectId, out ActionResolved resolved), Is.EqualTo(VnResult.VN_OK));
                generation = resolved.PublicationGeneration;
            }

            public CompanionTestAction Row(string name)
            {
                Assert.That(Publication.TryFindAction(name, out CompanionTestAction row), Is.True, name);
                return row;
            }

            public void AssertPreparationUnchanged()
            {
                Assert.That(Runtime.IsPreparationComplete, Is.True);
                Assert.That(Runtime.PendingPackLoadCount, Is.Zero);
                Assert.That(Runtime.Session.StartupPacks.Select(pack => pack.Handle).ToArray(), Is.EqualTo(preparedHandles));
                Assert.That(Actions.Resolve("Play_U4_B", Publication.ProjectId, out ActionResolved resolved), Is.EqualTo(VnResult.VN_OK));
                Assert.That(resolved.PublicationGeneration, Is.EqualTo(generation));
            }

            public void Dispose()
            {
                Runtime.ShutdownRuntime();
                Object.Destroy(Source);
                Object.Destroy(Owner);
                settingsLease.Dispose();
                if (companionRoot != null) Directory.Delete(companionRoot, true);
            }
        }
    }

    internal sealed class CompanionTestAction
    {
        internal string Name;
        internal VnObjectId ActionStableId;
        internal ulong OwnerPackStableId;
    }

    internal sealed class CompanionTestPack
    {
        internal string Name;
        internal string Path;
        internal ulong PackStableId;
        internal ulong ContentHash;
    }

    internal sealed class CompanionTestPublication
    {
        internal string ProjectId;
        internal string Language;
        internal CompanionTestPack InitPack;
        internal ulong InitPackStableId => InitPack.PackStableId;
        internal readonly List<CompanionTestAction> Actions = new List<CompanionTestAction>();
        internal readonly List<CompanionTestPack> Packs = new List<CompanionTestPack>();

        internal bool TryFindAction(string name, out CompanionTestAction action)
        {
            action = Actions.Find(row => row.Name == name);
            return action != null;
        }

        internal bool TryFindPack(ulong id, out CompanionTestPack pack)
        {
            pack = Packs.Find(row => row.PackStableId == id);
            return pack != null;
        }

        internal static VnResult Read(string root, string language,
            out CompanionTestPublication publication, out string error)
        {
            publication = null;
            var source = new PackDirectorySource(root, language);
            VnResult result = source.Capture(null, default, null, out CompanionInput init);
            error = result.ToString();
            if (result != VnResult.VN_OK) return result;
            publication = new CompanionTestPublication
            {
                ProjectId = init.Facts.projectId.ToString(),
                Language = language ?? string.Empty,
                InitPack = new CompanionTestPack { Name = "Init", Path = "Init.bytes",
                    PackStableId = init.Facts.packStableId, ContentHash = init.Facts.contentHash }
            };
            publication.Packs.Add(publication.InitPack);
            IEnumerable<string> files = Directory.GetFiles(root, "*.bytes");
            if (!string.IsNullOrEmpty(language) && Directory.Exists(Path.Combine(root, language)))
                files = files.Concat(Directory.GetFiles(Path.Combine(root, language), "*.bytes"));
            foreach (string name in files.Select(Path.GetFileNameWithoutExtension)
                .Where(name => name != "Init").Distinct().OrderBy(name => name, StringComparer.Ordinal))
            {
                result = source.Capture(name, init.Facts.projectId, init.Bytes, out CompanionInput action);
                error = result.ToString();
                if (result != VnResult.VN_OK) return result;
                publication.Actions.Add(new CompanionTestAction { Name = name,
                    ActionStableId = action.Facts.rootActionId, OwnerPackStableId = action.Facts.packStableId });
                publication.Packs.Add(new CompanionTestPack { Name = name,
                    Path = (string.IsNullOrEmpty(action.Names.Language) ? "" : action.Names.Language + "/") + name + ".bytes",
                    PackStableId = action.Facts.packStableId, ContentHash = action.Facts.contentHash });
            }
            error = string.Empty;
            return VnResult.VN_OK;
        }

        internal static PackSource Snapshot(string root)
        {
            var files = Directory.GetFiles(root, "*", SearchOption.AllDirectories)
                .ToDictionary(path => Path.GetRelativePath(root, path).Replace('\\', '/'), File.ReadAllBytes);
            return PackSource.FromReader(path => files.TryGetValue(path, out byte[] bytes) ? bytes : null);
        }
    }

}
