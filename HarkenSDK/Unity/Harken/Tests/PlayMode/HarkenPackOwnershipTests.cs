using System;
using System.Collections;
using System.IO;
using System.Reflection;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenPackOwnershipTests
    {
#if HARKEN_SDK_TEST_NULL_PUMP
        [System.Runtime.InteropServices.DllImport(HarkenNative.LibraryName,
            CallingConvention = System.Runtime.InteropServices.CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpNull(uint blocks);
#endif

        [UnityTest]
        public IEnumerator DestroyedComponentReleasesOnlyItsSharedRequest()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                Object.DestroyImmediate(fixture.Source);
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                for (int i = 0; i < 3; ++i) yield return null;
                fixture.AssertDependency(VnResult.VN_OK);
                fixture.ReleaseCaller();
                for (int i = 0; i < 3; ++i) yield return null;
                fixture.AssertDependency(VnResult.VN_CONTENT_NOT_LOADED);
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator ExplicitUnloadReleasesOnlyTheComponentRequest()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                Assert.That(fixture.Pack.Unload(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Pack.IsLoaded, Is.False);
                fixture.AssertDependency(VnResult.VN_OK);
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                fixture.ReleaseCaller();
                for (int i = 0; i < 3; ++i) yield return null;
                fixture.AssertDependency(VnResult.VN_CONTENT_NOT_LOADED);
                Assert.That(fixture.Pack.Unload(), Is.EqualTo(VnResult.VN_OK));
                fixture.AssertDependency(VnResult.VN_CONTENT_NOT_LOADED);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator UnloadListenerCanReenableWithoutLosingTheNewSharedRequest()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                bool reenabled = false;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (reenabled || !IsUnload(operation, result)) return;
                    reenabled = true;
                    Assert.That(fixture.Pack.IsLoaded, Is.False);
                    Assert.That(PendingCleanup(owner), Is.Zero);
                    Assert.That(owner.Session.PrepareActions(new[] { "Play_U4_B" }, owner.Session.ProjectId,
                        out ActionPreparation ready), Is.EqualTo(VnResult.VN_OK));
                    fixture.Pack.enabled = true;
                    Assert.That(fixture.Pack.Load(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(ready.Release(), Is.EqualTo(VnResult.VN_OK));
                };
                fixture.Pack.enabled = false;
                Assert.That(reenabled, Is.True);
                Assert.That(Get<HarkenAudioSystem>(fixture.Pack, "boundRuntime"), Is.SameAs(fixture.Runtime));
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                Assert.That(fixture.Pack.IsLoaded, Is.True);
                fixture.AssertDependency(VnResult.VN_OK);
                fixture.ReleaseCaller();
                Assert.That(fixture.Pack.Unload(), Is.EqualTo(VnResult.VN_OK));
                fixture.AssertDependency(VnResult.VN_CONTENT_NOT_LOADED);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator UnloadListenerCanRestartWithoutTransferringOldOwnershipToTheNewSession()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                PackBootstrapSession previous = fixture.Runtime.Session;
                bool restarted = false;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (restarted || !IsUnload(operation, result)) return;
                    restarted = true;
                    Assert.That(PendingCleanup(owner), Is.Zero);
                    Assert.That(owner.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(PendingCleanup(owner), Is.Zero);
                    Assert.That(owner.StartRuntime(fixture.Root), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(owner.Session.PrepareActions(new[] { "Play_U4_B" }, owner.Session.ProjectId,
                        out ActionPreparation ready), Is.EqualTo(VnResult.VN_OK));
                    fixture.Pack.enabled = true;
                    Assert.That(fixture.Pack.Load(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(ready.Release(), Is.EqualTo(VnResult.VN_OK));
                };
                fixture.Pack.enabled = false;
                Assert.That(restarted, Is.True);
                Assert.That(previous.IsActive, Is.False);
                Assert.That(fixture.Runtime.Session, Is.Not.SameAs(previous));
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                Assert.That(fixture.Pack.IsLoaded, Is.True);
                fixture.AssertDependency(VnResult.VN_OK);
                Assert.That(fixture.Pack.Unload(), Is.EqualTo(VnResult.VN_OK));
                fixture.AssertDependency(VnResult.VN_CONTENT_NOT_LOADED);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator EditedRuntimeReferenceCannotAbandonTheActualLoadOwner()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                Set(fixture.Pack, "runtime", null);
                Object.DestroyImmediate(fixture.Source);
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                fixture.ReleaseCaller();
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                fixture.AssertDependency(VnResult.VN_CONTENT_NOT_LOADED);
            }
            yield return null;
        }

        private sealed class Fixture : IDisposable
        {
            public readonly GameObject Owner = new GameObject("Pack cleanup Runtime");
            public readonly GameObject Source = new GameObject("Dependency Pack owner");
            public readonly HarkenAudioSystem Runtime;
            public readonly HarkenPack Pack;
            public readonly string Root = Path.Combine(Application.streamingAssetsPath, "Audio/U4Original");
            public ulong DependencyId { get; private set; }
            private CompanionTestPublication publication;
            private ulong callerId;
            private Pack caller;
            private ulong probeSeed;

            private readonly HarkenProjectSettingsLease settingsLease = HarkenProjectSettingsLease.Acquire();

            public Fixture()
            {
                Owner.SetActive(false);
                Source.SetActive(false);
                Runtime = Owner.AddComponent<HarkenAudioSystem>();
                Pack = Source.AddComponent<HarkenPack>();
            }

            public void Start()
            {
                Assert.That(CompanionTestPublication.Read(Root, null, out publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction("Play_U4_B", out CompanionTestAction dependency), Is.True);
                DependencyId = dependency.OwnerPackStableId;
                callerId = dependency.OwnerPackStableId;
                Set(Pack, "runtime", Runtime);
                Set(Pack, "packStableId", DependencyId);
                Set(Pack, "loadOnReady", false);
                settingsLease.Settings.SetPackDirectory(Root);
                Assert.That(Runtime.ConfigureBeforeStartup("Audio/U4Original", publication.InitPack.Path, Array.Empty<ulong>(), true, false),
                    Is.EqualTo(VnResult.VN_OK));
                Owner.SetActive(true);
                Source.SetActive(true);
                Assert.That(Runtime.Session.PrepareActions(new[] { dependency.Name }, publication.ProjectId,
                    out ActionPreparation ready), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Pack.Load(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Runtime.Session.AcquirePack(callerId, out caller), Is.EqualTo(VnResult.VN_OK));
                Assert.That(ready.Release(), Is.EqualTo(VnResult.VN_OK));
            }

            public void ReleaseCaller()
            {
                Assert.That(Runtime.Session.ReleasePack(callerId, caller), Is.EqualTo(VnResult.VN_OK));
#if HARKEN_SDK_TEST_NULL_PUMP
                // Null devices render only when explicitly pumped; Unity frames service logic alone.
                Assert.That(HarkenSdkTest_PumpNull(1), Is.EqualTo(VnResult.VN_OK));
#endif
            }

            public void AssertDependency(VnResult expected)
            {
                Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                VnResult result = Actions.Resolve("Play_U4_B", publication.ProjectId, out ActionResolved resolved);
                if (expected != VnResult.VN_OK)
                {
                    Assert.That(result, Is.Not.EqualTo(VnResult.VN_OK));
                    return;
                }
                Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post(in resolved, Emitter.None, probeSeed++, out ActionFiring firing),
                    Is.EqualTo(VnResult.VN_OK));
                Assert.That(firing.IsValid, Is.True);
            }

            public void Dispose()
            {
                Assert.That(Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                if (Source != null) Object.Destroy(Source);
                Object.Destroy(Owner);
                settingsLease.Dispose();
            }
        }

        private static bool IsUnload(string operation, VnResult result)
            => operation.StartsWith("Packs.UnloadPack(", StringComparison.Ordinal) && result == VnResult.VN_OK;

        private static int PendingCleanup(HarkenAudioSystem runtime) => Get<ICollection>(runtime, "packCleanup").Count;

        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance).GetValue(target);

        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance).SetValue(target, value);
    }
}
