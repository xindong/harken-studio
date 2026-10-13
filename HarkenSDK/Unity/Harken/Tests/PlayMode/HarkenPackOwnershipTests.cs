using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;
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
        public IEnumerator DestroyedDependencyRequestIsEventuallyReleased()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                ExpectRefusal();
                Object.DestroyImmediate(fixture.Source);
                Assert.That(PendingCleanup(fixture.Runtime), Is.EqualTo(1));
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
        public IEnumerator ExplicitUnloadRefusalRetainsTheComponentRequestForManualRetry()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                ExpectRefusal();
                Assert.That(fixture.Pack.Unload(), Is.EqualTo(VnResult.VN_IN_USE));
                Assert.That(fixture.Pack.IsLoaded, Is.True);
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                fixture.ReleaseCaller();
                for (int i = 0; i < 3; ++i) yield return null;
                fixture.AssertDependency(VnResult.VN_OK);
                Assert.That(fixture.Pack.Unload(), Is.EqualTo(VnResult.VN_OK));
                fixture.AssertDependency(VnResult.VN_CONTENT_NOT_LOADED);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator RefusalListenerCanReenableWithoutLosingTheNewSharedRequest()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                bool reenabled = false;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (reenabled || !IsRefusal(operation, result)) return;
                    reenabled = true;
                    Assert.That(fixture.Pack.IsLoaded, Is.False);
                    Assert.That(PendingCleanup(owner), Is.EqualTo(1));
                    fixture.Pack.enabled = true;
                    Assert.That(fixture.Pack.Load(), Is.EqualTo(VnResult.VN_OK));
                };
                ExpectRefusal();
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
        public IEnumerator RefusalListenerCanRestartWithoutTransferringOldOwnershipToTheNewSession()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                PackBootstrapSession previous = fixture.Runtime.Session;
                bool restarted = false;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (restarted || !IsRefusal(operation, result)) return;
                    restarted = true;
                    Assert.That(PendingCleanup(owner), Is.EqualTo(1));
                    Assert.That(owner.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(PendingCleanup(owner), Is.Zero);
                    Assert.That(owner.StartRuntime(fixture.Root), Is.EqualTo(VnResult.VN_OK));
                    fixture.Pack.enabled = true;
                    Assert.That(fixture.Pack.Load(), Is.EqualTo(VnResult.VN_OK));
                };
                ExpectRefusal();
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
        public IEnumerator CleanupListenerCanRestartAfterRetiringTheOldRequest()
        {
            using (var fixture = new Fixture())
            {
                fixture.Start();
                PackBootstrapSession previous = fixture.Runtime.Session;
                ExpectRefusal();
                Object.DestroyImmediate(fixture.Source);
                fixture.ReleaseCaller();
                bool restarted = false;
                Pack current = default;
                fixture.Runtime.OperationCompleted += (owner, operation, result) =>
                {
                    if (restarted || operation != "Packs.UnloadPack(cleanup)") return;
                    restarted = true;
                    Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(PendingCleanup(owner), Is.Zero);
                    Assert.That(owner.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(owner.StartRuntime(fixture.Root), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(owner.Session.AcquirePack(fixture.DependencyId, out current), Is.EqualTo(VnResult.VN_OK));
                };
                for (int i = 0; i < 3; ++i) yield return null;
                Assert.That(restarted, Is.True);
                Assert.That(previous.IsActive, Is.False);
                Assert.That(PendingCleanup(fixture.Runtime), Is.Zero);
                fixture.AssertDependency(VnResult.VN_OK);
                Assert.That(fixture.Runtime.Session.ReleasePack(fixture.DependencyId, current), Is.EqualTo(VnResult.VN_OK));
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
                ExpectRefusal();
                Object.DestroyImmediate(fixture.Source);
                Assert.That(PendingCleanup(fixture.Runtime), Is.EqualTo(1));
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
            private GameplayPublication publication;
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
                Assert.That(GameplayPublication.Read(Root, null, out publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction("Play_U4_B", out GameplayAction dependency), Is.True);
                Assert.That(publication.TryFindAction("Play_U4_Caller", out GameplayAction consumer), Is.True);
                DependencyId = dependency.OwnerPackStableId;
                callerId = consumer.OwnerPackStableId;
                Set(Pack, "runtime", Runtime);
                Set(Pack, "packStableId", DependencyId);
                Set(Pack, "loadOnReady", false);
                settingsLease.Settings.SetPackDirectory(Root);
                Assert.That(Runtime.ConfigureBeforeStartup("Audio/U4Original", "Init.pack", Array.Empty<ulong>(), true, false),
                    Is.EqualTo(VnResult.VN_OK));
                Owner.SetActive(true);
                Source.SetActive(true);
                Assert.That(Pack.Load(), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Runtime.Session.AcquirePack(callerId, out caller), Is.EqualTo(VnResult.VN_OK));
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
                Assert.That(Actions.Resolve("Play_U4_B", publication.ProjectId, out ActionResolved resolved),
                    Is.EqualTo(VnResult.VN_OK), "The catalog entry remains published after content unload.");
                Assert.That(Actions.Post(in resolved, Emitter.None, probeSeed++, out ActionFiring firing),
                    Is.EqualTo(expected), "Checked admission must reflect the outstanding Pack requests.");
                Assert.That(firing.IsValid, Is.EqualTo(expected == VnResult.VN_OK));
                // Successful probes retain their firing slots until this fixture terminates the session.
            }

            public void Dispose()
            {
                Assert.That(Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                if (Source != null) Object.Destroy(Source);
                Object.Destroy(Owner);
                settingsLease.Dispose();
            }
        }

        private static bool IsRefusal(string operation, VnResult result)
            => operation.StartsWith("Packs.UnloadPack(", StringComparison.Ordinal) && result == VnResult.VN_IN_USE;

        private static void ExpectRefusal()
            => LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: Packs.UnloadPack.*VN_IN_USE"));

        private static int PendingCleanup(HarkenAudioSystem runtime) => Get<ICollection>(runtime, "packCleanup").Count;

        private static T Get<T>(object target, string name)
            => (T)target.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance).GetValue(target);

        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance).SetValue(target, value);
    }
}
