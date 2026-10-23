#if HARKEN_SDK_TEST_NULL_PUMP
using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using Harken.Unity.Editor;
using NUnit.Framework;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenSpatialPlaybackTests
    {
        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpAndCopy(
            [Out] float[] samples, uint capacity, out uint frames, out uint channels);

        [Test]
        public void ReenabledListenerRepublishesItsUnchangedPoseAfterCameraSwitch()
        {
            using (var fixture = new Fixture(false, "SpatialPacks"))
            {
                var other = new GameObject("Other camera");
                try
                {
                    HarkenListener first = fixture.Listener.gameObject.AddComponent<HarkenListener>();
                    HarkenListener second = other.AddComponent<HarkenListener>();
                    SetListenerEnabled(first, true);
                    SetListenerEnabled(second, false);
                    other.transform.rotation = Quaternion.Euler(0, 180, 0);
                    fixture.Actor.position = Vector3.right * 80;
                    fixture.Play(fixture.CreateSource());
                    Vector2 original = fixture.Render(8, first);
                    Assert.That(original.y, Is.GreaterThan(original.x * 3));
                    SetListenerEnabled(first, false);
                    SetListenerEnabled(second, true);
                    Vector2 switched = fixture.Render(8, second);
                    Assert.That(switched.x, Is.GreaterThan(switched.y * 3));
                    SetListenerEnabled(second, false);
                    SetListenerEnabled(first, true);
                    Vector2 restored = fixture.Render(8, first);
                    Assert.That(restored.y, Is.GreaterThan(restored.x * 3));
                    Assert.That(restored.y, Is.GreaterThan(0.001f));
                }
                finally { Object.DestroyImmediate(other); }
            }
        }

        private static void SetListenerEnabled(HarkenListener listener, bool enabled)
        {
            listener.enabled = enabled;
            // EditMode does not run ordinary MonoBehaviour enable/disable callbacks.
            typeof(HarkenListener).GetMethod(enabled ? "OnEnable" : "OnDisable",
                BindingFlags.Instance | BindingFlags.NonPublic).Invoke(listener, null);
        }

        [TestCase(true)]
        [TestCase(false)]
        public void SpatialPlaybackRendersMovementRotationDistanceAndFrozenTails(bool editorPreview)
        {
            if (editorPreview) HarkenEditorServiceTests.RequirePublicationTooling();
            using (var fixture = new Fixture(editorPreview, "SpatialPacks"))
            {
                // The authored attenuation profile widens near sources toward center.
                fixture.Actor.position = Vector3.left * 80;
                var source = fixture.CreateSource();
                var original = fixture.Play(source);
                Vector2 left = fixture.Render();
                Assert.That(left.x, Is.GreaterThan(left.y * 3));
                Assert.That(left.x, Is.GreaterThan(0.001f));

                fixture.Actor.position = Vector3.right * 80;
                Vector2 right = fixture.Render();
                Assert.That(right.y, Is.GreaterThan(right.x * 3));

                fixture.Listener.rotation = Quaternion.Euler(0, 180, 0);
                Vector2 turned = fixture.Render();
                Assert.That(turned.x, Is.GreaterThan(turned.y * 3));
                fixture.Listener.rotation = Quaternion.identity;
                fixture.Actor.position = Vector3.right * 2;
                Vector2 near = fixture.Render();
                fixture.Actor.position = Vector3.right * 90;
                Vector2 far = fixture.Render();
                // Exported volume curves interpolate amplitude: 0.1009 / 0.98002.
                Assert.That(far.magnitude / near.magnitude, Is.EqualTo(0.103f).Within(0.02f));

                fixture.Actor.position = Vector3.left * 80;
                Assert.That(source.EndFollow(), Is.EqualTo(VnResult.VN_OK));
                fixture.Actor.position = Vector3.right * 80;
                Vector2 tail = fixture.Render();
                Assert.That(tail.x, Is.GreaterThan(tail.y * 3));
                Assert.That(tail.x, Is.GreaterThan(0.001f));

                var recycled = fixture.CreateSource();
                Assert.That(recycled, Is.Not.SameAs(source));
                var replacement = fixture.Play(recycled);
                fixture.Render();
                fixture.Stop(replacement);
                Vector2 originalOnly = fixture.Render();
                Assert.That(originalOnly.x, Is.GreaterThan(originalOnly.y * 3));
                Assert.That(originalOnly.x, Is.GreaterThan(0.001f));
                fixture.Stop(original);
                fixture.Play(recycled);
                Vector2 replacementOnly = fixture.Render();
                Assert.That(replacementOnly.y, Is.GreaterThan(replacementOnly.x * 3));
                Assert.That(replacementOnly.y, Is.GreaterThan(0.001f));
            }
        }

        [TestCase(true)]
        [TestCase(false)]
        public void EndedSourceRetainsDeferredPlaybackThenReleasesItsEmitter(bool editorPreview)
        {
            if (editorPreview) HarkenEditorServiceTests.RequirePublicationTooling();
            using (var fixture = new Fixture(editorPreview, "SpatialPacks"))
            {
                fixture.Actor.position = Vector3.right * 80;
                var source = fixture.CreateSource();
                var firing = fixture.Play(source, "Play_Spatial_Delayed");
                fixture.Actor.position = Vector3.left * 80;
                Assert.That(source.EndFollow(), Is.EqualTo(VnResult.VN_OK));
                fixture.Actor.position = Vector3.right * 80;
                Assert.That(fixture.Render().magnitude, Is.LessThan(0.0001f));
                Vector2 delayed = fixture.Render(64);
                Assert.That(delayed.x, Is.GreaterThan(0.001f));
                Assert.That(delayed.x, Is.GreaterThan(delayed.y * 3));
                fixture.Render(1024);
                Assert.That(firing.Query(out _), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                var emitter = (Emitter)typeof(HarkenSpatialSource).GetField("emitter",
                    BindingFlags.Instance | BindingFlags.NonPublic).GetValue(source);
                Assert.That(emitter.IsValid, Is.False);
                fixture.Play(fixture.CreateSource());
                Vector2 replay = fixture.Render();
                Assert.That(replay.y, Is.GreaterThan(replay.x * 3));
            }
        }

        [TestCase(true)]
        [TestCase(false)]
        public void Authored2DPlaybackIgnoresSpatialSourceMovement(bool editorPreview)
        {
            if (editorPreview) HarkenEditorServiceTests.RequirePublicationTooling();
            Vector2 stationary = RenderAuthored2D(editorPreview, false);
            Vector2 moving = RenderAuthored2D(editorPreview, true);
            // Compare the same playback window; the authored waveform varies over time.
            Assert.That(moving.magnitude, Is.EqualTo(stationary.magnitude).Within(stationary.magnitude * 0.001f));
        }

        private static Vector2 RenderAuthored2D(bool editorPreview, bool moveSource)
        {
            using (var fixture = new Fixture(editorPreview, "U4Original"))
            {
                fixture.Actor.position = Vector3.left * 2;
                fixture.Play(fixture.CreateSource());
                Vector2 near = fixture.Render();
                if (moveSource) fixture.Actor.position = Vector3.right * 90;
                Vector2 far = fixture.Render();
                Assert.That(near.magnitude, Is.GreaterThan(0.001f));
                Assert.That(far.magnitude, Is.GreaterThan(0.001f));
                Assert.That(near.x, Is.EqualTo(near.y).Within(0.0001f));
                Assert.That(far.x, Is.EqualTo(far.y).Within(0.0001f));
                return far;
            }
        }

        private sealed class Fixture : IDisposable
        {
            private readonly GameObject actor = new GameObject("Spatial test source");
            private readonly GameObject listener = new GameObject("Spatial test listener");
            private readonly GameObject owner = new GameObject("Spatial test owner");
            private readonly HarkenEditorPreview preview;
            private readonly HarkenEditorPreview.Scope scope;
            private readonly HarkenAudioSystem runtime;
            private readonly string projectId;
            private readonly float[] samples = new float[8192];
            public Transform Actor => actor.transform;
            public Transform Listener => listener.transform;

            public Fixture(bool editorPreview, string publication)
            {
                owner.SetActive(false);
                string root = Path.Combine(Application.streamingAssetsPath, "Audio", publication);
                Assert.That(HarkenPreparedPacks.Capture(PackManifest.ManifestFileName,
                    path => File.ReadAllBytes(Path.Combine(root, path)), out var prepared, out _, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                projectId = prepared.Publication.ProjectId;
                if (editorPreview)
                {
                    preview = new HarkenEditorPreview();
                    Set(preview, "forceNullDevice", true);
                    scope = preview.CreateScope();
                    string[] names = publication == "SpatialPacks"
                        ? new[] { "Play_U4_A", "Play_Spatial_Delayed" } : new[] { "Play_U4_A" };
                    Assert.That(scope.Prepare(prepared, names), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.SetListener(Listener), Is.EqualTo(VnResult.VN_OK));
                }
                else
                {
                    runtime = owner.AddComponent<HarkenAudioSystem>();
                    Set(runtime, "autoInitialize", false);
                    Set(runtime, "persistAcrossScenes", false);
                    Assert.That(runtime.ConfigureBeforeStartup("Audio/" + publication, prepared.Publication.InitPack.Path,
                        Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                    owner.SetActive(true);
                    typeof(HarkenAudioSystem).GetMethod("Awake", BindingFlags.Instance | BindingFlags.NonPublic).Invoke(runtime, null);
                    Assert.That(runtime.StartRuntime(prepared), Is.EqualTo(VnResult.VN_OK), runtime.LastStartupError);
                }
            }

            public HarkenSpatialSource CreateSource()
            {
                HarkenSpatialSource source;
                VnResult result = scope != null ? scope.CreateSpatialSource(Actor, out source)
                    : runtime.CreateSpatialSource(Actor, out source);
                Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                return source;
            }

            public ActionFiring Play(HarkenSpatialSource source, string name = "Play_U4_A")
            {
                ActionFiring firing;
                VnResult result = scope != null ? scope.Play(name, source, out firing)
                    : runtime.PlayAction(name, projectId, source, out firing);
                Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                return firing;
            }

            public void Stop(ActionFiring firing)
                => Assert.That(scope != null ? scope.Stop(firing, 0) : firing.Stop(0), Is.EqualTo(VnResult.VN_OK));

            public Vector2 Render(int blockCount = 8, HarkenListener component = null)
            {
                VnSnapshot before = HarkenEngine.DefaultSnapshot();
                Assert.That(HarkenEngine.Capture(ref before), Is.EqualTo(VnResult.VN_OK));
                Vector2 energy = default;
                for (int block = 0; block < blockCount; ++block)
                {
                    if (scope != null) Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
                    else
                    {
                        if (component == null) Assert.That(UnitySpatial.SetListener(Listener), Is.EqualTo(VnResult.VN_OK));
                        else Assert.That(runtime.SelectListener(component), Is.EqualTo(VnResult.VN_OK));
                        typeof(HarkenAudioSystem).GetMethod("AdvanceFrame", BindingFlags.Instance | BindingFlags.NonPublic).Invoke(runtime, null);
                    }
                    Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length, out uint frames, out uint channels),
                        Is.EqualTo(VnResult.VN_OK));
                    Assert.That(channels, Is.EqualTo(2));
                    if (block < blockCount - 4) continue;
                    for (int frame = 0; frame < frames; ++frame)
                    {
                        energy.x += samples[frame * 2] * samples[frame * 2];
                        energy.y += samples[frame * 2 + 1] * samples[frame * 2 + 1];
                    }
                }
                VnSnapshot after = HarkenEngine.DefaultSnapshot();
                Assert.That(HarkenEngine.Capture(ref after), Is.EqualTo(VnResult.VN_OK));
                Assert.That(after.blockIndex, Is.GreaterThan(before.blockIndex));
                return new Vector2(Mathf.Sqrt(energy.x), Mathf.Sqrt(energy.y));
            }

            public void Dispose()
            {
                preview?.Shutdown();
                if (runtime != null) runtime.ShutdownRuntime();
                Object.DestroyImmediate(owner);
                Object.DestroyImmediate(actor);
                Object.DestroyImmediate(listener);
            }

            private static void Set(object target, string name, object value)
                => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);
        }
    }
}
#endif
