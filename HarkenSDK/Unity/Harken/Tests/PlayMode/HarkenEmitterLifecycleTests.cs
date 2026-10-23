using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Profiling;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenEmitterLifecycleTests
    {
        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpAndCopy(
            [Out] float[] samples, uint capacity, out uint frames, out uint channels);

        [StructLayout(LayoutKind.Sequential)]
        private struct AcceptedSpatialSnapshot
        {
            public ulong Firing, Instance, Emitter, Block;
            public VnVec3 SourcePosition, SourceForward, ListenerPosition, ListenerRight;
        }

        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_CaptureAcceptedSpatial(
            ulong firing, out AcceptedSpatialSnapshot snapshot);

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
                Emitter oldComponent = f.Emitter.Handle;
                Assert.That(f.Runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                f.Start();
                Assert.That(next.SetDistanceScale(1), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(oldComponent.SetDistanceScale(1), Is.EqualTo(VnResult.VN_STALE_HANDLE));
                Assert.That(f.Emitter.Handle.Handle, Is.Not.EqualTo(oldComponent.Handle));
                Assert.That(f.Emitter.SetDistanceScale(1), Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        #if HARKEN_SDK_TEST_NULL_PUMP
        [UnityTest]
        public IEnumerator WarmMovingSpatialFrameKeepsZeroManagedAllocations()
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                f.Camera.SetActive(true);
                var actors = new Transform[8];
                var sources = new HarkenSpatialSource[8];
                for (int i = 0; i < actors.Length; ++i)
                {
                    GameObject actor = i == 0 ? f.Actor : new GameObject("Allocation source " + i);
                    if (i != 0)
                    {
                        actor.transform.SetParent(f.Actor.transform);
                        actor.AddComponent<HarkenEmitter>();
                    }
                    actors[i] = actor.transform;
                    Assert.That(f.Runtime.CreateSpatialSource(actors[i], out sources[i]),
                        Is.EqualTo(VnResult.VN_OK));
                }
                Recorder recorder = Recorder.Get("GC.Alloc");
                recorder.enabled = false;
                recorder.FilterToCurrentThread();
                try
                {
                    AssertAllocationRecorder(recorder);
                    int allocations = 0;
                    int activeFrames = 0;
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        foreach (HarkenSpatialSource source in sources)
                            Assert.That(f.Runtime.PlayAction(f.Action.ActionName, f.Action.ProjectId, source, out _),
                                Is.EqualTo(VnResult.VN_OK));
                        // Warm a complete playback/retirement cycle, including cached success notifications.
                        for (int frame = 0; frame < 1024; ++frame)
                        {
                            for (int i = 0; i < actors.Length; ++i)
                                actors[i].position = new Vector3((frame & 1) == 0 ? 80 + i : -80 - i, 0, i);
                            f.Camera.transform.position = new Vector3(frame & 1, 0, 0);
                            f.Camera.transform.rotation = Quaternion.Euler(0, (frame & 1) * 180, 0);
                            recorder.enabled = true;
                            f.Runtime.AdvanceFrame();
                            recorder.enabled = false;
                            int count = recorder.sampleBlockCount;
                            if (pass != 0) allocations += count;
                            f.RenderPhysicalBlocks(1);
                            var snapshot = HarkenEngine.DefaultSnapshot();
                            Assert.That(HarkenEngine.Capture(ref snapshot), Is.EqualTo(VnResult.VN_OK));
                            if (pass != 0 && snapshot.activeVoiceCount == 8) ++activeFrames;
                        }
                    }
                    AssertAllocationRecorder(recorder);
                    Assert.That(activeFrames, Is.GreaterThan(64), "The measured frames must include eight active Voices.");
                    Assert.That(allocations, Is.Zero,
                        "1024 warm moving frames with 8 component emitters, 8 Transform sources and one Listener.");
                    Debug.Log($"SPATIAL_GC frames=1024 activeFrames={activeFrames} components=8 sources=8 listeners=1 allocations={allocations}");
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
            int count = recorder.sampleBlockCount;
            GC.KeepAlive(control);
            Assert.That(count, Is.GreaterThan(0), "The allocation recorder must detect the positive control.");
        }

        [UnityTest]
        public IEnumerator ComponentPostUsesPosesMovedInOrdinaryUpdate() => ImmediatePoseCase(false);

        [UnityTest]
        public IEnumerator TransformSourcePostUsesPosesMovedInOrdinaryUpdate() => ImmediatePoseCase(true);

        [UnityTest]
        public IEnumerator ComponentDistanceScaleAppliesAtFirstSoundAndResetsWithIdentity() => ImmediatePoseCase(false, 2);

        [UnityTest]
        public IEnumerator TransformSourceDistanceScaleAppliesAtFirstSoundAndResetsWithIdentity() => ImmediatePoseCase(true, 2);

        private static IEnumerator ImmediatePoseCase(bool useSpatialSource, float scale = 1)
        {
            Vector2 reference = default;
            for (int pass = 0; pass < (scale == 1 ? 2 : 3); ++pass)
            {
                using (var f = new Fixture())
                {
                    f.Start();
                    f.Actor.SetActive(true);
                    f.Camera.SetActive(true);
                    f.Actor.transform.position = Vector3.right * 80;
                    HarkenSpatialSource source = null;
                    if (useSpatialSource)
                        Assert.That(f.Runtime.CreateSpatialSource(f.Actor.transform, out source), Is.EqualTo(VnResult.VN_OK));
                    if (pass == 2)
                    {
                        if (useSpatialSource)
                        {
                            Assert.That(source.SetDistanceScale(scale), Is.EqualTo(VnResult.VN_OK));
                            Assert.That(source.Validate(f.Runtime, f.Runtime.Session, out Emitter previous), Is.EqualTo(VnResult.VN_OK));
                            Assert.That(source.EndFollow(), Is.EqualTo(VnResult.VN_OK));
                            Assert.That(source.Maintain(), Is.EqualTo(VnResult.VN_OK));
                            Assert.That(f.Runtime.CreateSpatialSource(f.Actor.transform, out source), Is.EqualTo(VnResult.VN_OK));
                            Assert.That(source.Validate(f.Runtime, f.Runtime.Session, out Emitter current), Is.EqualTo(VnResult.VN_OK));
                            Assert.That(current.Handle, Is.Not.EqualTo(previous.Handle));
                        }
                        else
                        {
                            Assert.That(f.Emitter.SetDistanceScale(scale), Is.EqualTo(VnResult.VN_OK));
                            Emitter previous = f.Emitter.Handle;
                            Assert.That(f.Emitter.DestroyEmitter(), Is.EqualTo(VnResult.VN_OK));
                            Assert.That(f.Emitter.Create(), Is.EqualTo(VnResult.VN_OK));
                            Assert.That(f.Emitter.Handle.Handle, Is.Not.EqualTo(previous.Handle));
                        }
                    }
                    var driver = f.Owner.AddComponent<HarkenSpatialPlayerLoopProbe>();
                    Vector2 rendered = default;
                    ActionFiring posted = default;
                    int currentPass = pass;
                    driver.BeforeLate = () =>
                    {
                        if (currentPass != 0)
                        {
                            f.Actor.transform.position = Vector3.right * 160;
                            f.Camera.transform.position = Vector3.right * (160 + 80 * (currentPass == 1 ? scale : 1));
                        }
                        if (currentPass == 1 && scale != 1)
                        {
                            Assert.That(useSpatialSource ? source.SetDistanceScale(scale) : f.Emitter.SetDistanceScale(scale),
                                Is.EqualTo(VnResult.VN_OK));
                        }
                        VnResult result = useSpatialSource
                            ? f.Runtime.PlayAction(f.Action.ActionName, f.Action.ProjectId, source, out posted)
                            : f.Action.Play();
                        if (!useSpatialSource) posted = f.Action.Firing;
                        Assert.That(result, Is.EqualTo(VnResult.VN_OK));
                        // This executes inside Update, immediately after Play and before any LateUpdate.
                        AssertAcceptedSpatial(posted, f.Actor.transform, f.Camera.transform,
                            useSpatialSource ? 0UL : f.Emitter.Handle.Handle);
                    };
                    yield return f.FirstAudible(driver, () => posted, value => rendered = value);
                    if (pass == 0)
                    {
                        reference = rendered;
                        Assert.That(reference.y, Is.GreaterThan(reference.x * 3));
                        Assert.That(reference.y, Is.GreaterThan(0.001f));
                    }
                    else
                    {
                        Assert.That(rendered.x, Is.GreaterThan(rendered.y * 3));
                        Assert.That(rendered.x, Is.EqualTo(reference.y).Within(reference.y * 0.02f));
                    }
                }
            }
        }

        private static void AssertAcceptedSpatial(ActionFiring firing, Transform source,
            Transform listener, ulong expectedEmitter = 0)
        {
            Assert.That(HarkenSdkTest_CaptureAcceptedSpatial(firing.Handle, out AcceptedSpatialSnapshot accepted),
                Is.EqualTo(VnResult.VN_OK));
            Assert.That(accepted.Firing, Is.EqualTo(firing.Handle));
            Assert.That(firing.QueryStep(0, out VnActionStepResult step), Is.EqualTo(VnResult.VN_OK));
            Assert.That(accepted.Instance, Is.EqualTo(step.producedPlayableInstance).And.Not.Zero);
            Assert.That(accepted.Emitter, Is.Not.Zero);
            if (expectedEmitter != 0) Assert.That(accepted.Emitter, Is.EqualTo(expectedEmitter));
            AssertPose(accepted.SourcePosition, UnitySpatial.ToHarken(source.position));
            AssertPose(accepted.SourceForward, UnitySpatial.ToHarken(source.forward));
            AssertPose(accepted.ListenerPosition, UnitySpatial.ToHarken(listener.position));
            AssertPose(accepted.ListenerRight, UnitySpatial.ToHarken(listener.right));
            Debug.Log($"SPATIAL_ACCEPTED frame={Time.frameCount} firing={accepted.Firing} instance={accepted.Instance} emitter={accepted.Emitter} block={accepted.Block} sourceX={accepted.SourcePosition.x:R} listenerX={accepted.ListenerPosition.x:R}");
        }

        private static void AssertPose(VnVec3 actual, VnVec3 expected)
        {
            Assert.That(actual.x, Is.EqualTo(expected.x).Within(0.00001f));
            Assert.That(actual.y, Is.EqualTo(expected.y).Within(0.00001f));
            Assert.That(actual.z, Is.EqualTo(expected.z).Within(0.00001f));
        }

        [UnityTest]
        public IEnumerator AsyncPreparationUsesCurrentSourceAndListenerAtFirstSound() => AsyncPreparationCase(1);

        [UnityTest]
        public IEnumerator AsyncPreparationUsesDistanceScaleChangedWhileAwaitingContent() => AsyncPreparationCase(2);

        private static IEnumerator AsyncPreparationCase(float scale)
        {
            Vector2 reference = default;
            for (int pass = 0; pass < 2; ++pass)
            {
                using (var f = new Fixture(companion: pass != 0))
                {
                    var content = new TaskCompletionSource<byte[]>();
                    Dictionary<string, byte[]> companion = f.CompanionBytes();
                    int contentReads = 0;
                    if (pass == 0) f.Start();
                    else
                    {
                        var init = new TaskCompletionSource<byte[]>();
                        var start = f.Runtime.StartRuntimeAsync(PackSource.FromAsyncReader((path, token) =>
                        {
                            if (path == "Init.bytes") return init.Task;
                            if (path == "Play_U4_A.bytes") { ++contentReads; return content.Task; }
                            return Task.FromResult(companion.TryGetValue(path, out byte[] bytes) ? bytes : null);
                        }));
                        Assert.That(start.IsCompleted, Is.False);
                        yield return null;
                        init.SetResult(companion["Init.bytes"]);
                        yield return Until(start);
                        Assert.That(start.GetAwaiter().GetResult(), Is.EqualTo(VnResult.VN_OK));
                        Assert.That(f.Runtime.Session.Publication, Is.Null);
                        Assert.That(contentReads, Is.Zero);
                    }
                    f.Actor.SetActive(true);
                    f.Camera.SetActive(true);
                    f.Actor.transform.position = Vector3.right * 80;
                    Assert.That(f.Runtime.CreateSpatialSource(f.Actor.transform, out HarkenSpatialSource source),
                        Is.EqualTo(VnResult.VN_OK));
                    var driver = f.Owner.AddComponent<HarkenSpatialPlayerLoopProbe>();
                    ActionFiring firing = default;
                    if (pass == 0)
                    {
                        driver.BeforeLate = () =>
                        {
                            f.Actor.transform.position = Vector3.right * 160;
                            f.Camera.transform.position = Vector3.right * 240;
                            Assert.That(f.Runtime.PlayAction(f.Action.ActionName, f.Action.ProjectId, source, out firing),
                                Is.EqualTo(VnResult.VN_OK));
                            AssertAcceptedSpatial(firing, f.Actor.transform, f.Camera.transform);
                        };
                    }
                    else
                    {
                        var boundary = f.Owner.AddComponent<HarkenSpatialBeforeBoundaryProbe>();
                        int postFrame = -1;
                        int postBoundary = -1;
                        f.Runtime.OperationCompleted += (_, operation, result) =>
                        {
                            if (operation != "HarkenAudioSystem.PlayAction" || result != VnResult.VN_OK) return;
                            postFrame = Time.frameCount;
                            postBoundary = boundary.LateUpdateCount;
                        };
                        var play = f.Runtime.PlayActionAsync(f.Action.ActionName, f.Action.ProjectId, source);
                        var accepted = CaptureSuccessfulAsyncPost(play, f.Actor.transform, f.Camera.transform,
                            boundary, () => postFrame, () => postBoundary);
                        Assert.That(f.Runtime.Session.Publication, Is.Null);
                        Assert.That(contentReads, Is.EqualTo(1));
                        Assert.That(play.IsCompleted, Is.False);
                        yield return null;
                        Assert.That(play.IsCompleted, Is.False);
                        driver.BeforeLate = () =>
                        {
                            f.Actor.transform.position = Vector3.right * 160;
                            f.Camera.transform.position = Vector3.right * (160 + 80 * scale);
                            if (scale != 1) Assert.That(source.SetDistanceScale(scale), Is.EqualTo(VnResult.VN_OK));
                            content.SetResult(companion["Play_U4_A.bytes"]);
                        };
                        yield return Until(accepted);
                        HarkenActionPlayResult result = accepted.GetAwaiter().GetResult();
                        Assert.That(result.Result, Is.EqualTo(VnResult.VN_OK));
                        firing = result.Firing;
                        Assert.That(contentReads, Is.EqualTo(1));
                    }
                    Vector2 first = default;
                    yield return f.FirstAudible(driver, () => firing, value => first = value);
                    Assert.That(first.x, Is.GreaterThan(0.001f));
                    Assert.That(first.x, Is.GreaterThan(first.y * 3));
                    if (pass == 0) reference = first;
                    else
                    {
                        Assert.That(first.x, Is.EqualTo(reference.x).Within(reference.x * 0.02f));
                        Assert.That(first.y, Is.EqualTo(reference.y).Within(0.00001f));
                    }
                }
            }
        }

        private static async Task<HarkenActionPlayResult> CaptureSuccessfulAsyncPost(
            Task<HarkenActionPlayResult> play, Transform source, Transform listener,
            HarkenSpatialBeforeBoundaryProbe boundary, Func<int> postFrame, Func<int> postBoundary)
        {
            HarkenActionPlayResult result = await play;
            Assert.That(result.Result, Is.EqualTo(VnResult.VN_OK));
            // Capture in the successful continuation, before yielding back to the test coroutine.
            AssertAcceptedSpatial(result.Firing, source, listener);
            Assert.That(postFrame(), Is.GreaterThanOrEqualTo(0));
            Assert.That(Time.frameCount, Is.EqualTo(postFrame()));
            Assert.That(boundary.LateUpdateCount, Is.EqualTo(postBoundary()),
                "No spatial LateUpdate may correct the poses between Post and accepted-pose capture.");
            Debug.Log($"ASYNC_SPATIAL_ACCEPTED postFrame={postFrame()} captureFrame={Time.frameCount} postBoundary={postBoundary()} captureBoundary={boundary.LateUpdateCount}");
            return result;
        }

        private static IEnumerator Until(Task task)
        {
            float deadline = Time.realtimeSinceStartup + 15;
            while (!task.IsCompleted && Time.realtimeSinceStartup < deadline) yield return null;
            Assert.That(task.IsCompleted, Is.True, "Spatial preparation timed out.");
        }

        [UnityTest]
        public IEnumerator FrameEndFollowsSourceAndListenerWithoutAnotherLogicalUpdate()
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                f.Camera.SetActive(true);
                f.Actor.transform.position = Vector3.right * 80;
                var driver = f.Owner.AddComponent<HarkenSpatialPlayerLoopProbe>();
                bool finished = false;
                Vector2 reference = default;
                driver.BeforeLate = () => Assert.That(f.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                yield return f.FirstAudible(driver, () => f.Action.Firing, _ => { });
                reference = f.RenderPhysicalBlocks(8);
                Assert.That(reference.y, Is.GreaterThan(reference.x * 3));
                finished = false;
                Vector2 moved = default;
                driver.BeforeLate = () =>
                {
                    f.Actor.transform.position = Vector3.right * 160;
                    f.Camera.transform.position = Vector3.right * 240;
                    driver.AfterLate = () => { moved = f.RenderPhysicalBlocks(8); finished = true; };
                };
                for (int frame = 0; frame < 10 && !finished; ++frame) yield return null;
                Assert.That(finished, Is.True);
                Assert.That(moved.x, Is.GreaterThan(moved.y * 3));
                Assert.That(moved.x, Is.EqualTo(reference.y).Within(reference.y * 0.1f));
            }
        }

        [UnityTest]
        public IEnumerator LateWritesOnEitherSideOfTheBoundaryUseTheIntendedSubmission()
        {
            using (var f = new Fixture())
            {
                f.Start();
                f.Actor.SetActive(true);
                f.Camera.SetActive(true);
                f.Actor.transform.position = Vector3.right * 80;
                var driver = f.Owner.AddComponent<HarkenSpatialPlayerLoopProbe>();
                var earlier = f.Owner.AddComponent<HarkenSpatialBeforeBoundaryProbe>();
                driver.BeforeLate = () => Assert.That(f.Action.Play(), Is.EqualTo(VnResult.VN_OK));
                yield return f.FirstAudible(driver, () => f.Action.Firing, _ => { });
                bool finished = false;
                Vector2 beforeBoundary = default;
                earlier.OnLate = () => f.Actor.transform.position = Vector3.left * 80;
                driver.AfterLate = () => { beforeBoundary = f.RenderPhysicalBlocks(8); finished = true; };
                for (int frame = 0; frame < 10 && !finished; ++frame) yield return null;
                Assert.That(finished, Is.True);
                Assert.That(beforeBoundary.x, Is.GreaterThan(beforeBoundary.y * 3));
                finished = false;
                Vector2 afterBoundary = default;
                driver.AfterLate = () =>
                {
                    f.Actor.transform.position = Vector3.right * 80;
                    afterBoundary = f.RenderPhysicalBlocks(8);
                    finished = true;
                };
                for (int frame = 0; frame < 10 && !finished; ++frame) yield return null;
                Assert.That(finished, Is.True);
                Assert.That(afterBoundary.x, Is.GreaterThan(afterBoundary.y * 3));
                finished = false;
                Vector2 nextSubmission = default;
                driver.AfterLate = () => { nextSubmission = f.RenderPhysicalBlocks(8); finished = true; };
                for (int frame = 0; frame < 10 && !finished; ++frame) yield return null;
                Assert.That(finished, Is.True);
                Assert.That(nextSubmission.y, Is.GreaterThan(nextSubmission.x * 3));
            }
        }

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

            public Fixture(bool companion = false)
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
                Assert.That(Runtime.ConfigureBeforeStartup("Audio/SpatialPacks", publication.InitPack.Path,
                    companion ? Array.Empty<ulong>() : new[] { selected.OwnerPackStableId }, true, false), Is.EqualTo(VnResult.VN_OK));
                if (companion) Assert.That(Runtime.ConfigurePlatformBeforeStartup(null), Is.EqualTo(VnResult.VN_OK));
                Owner.SetActive(true);
            }

            public Dictionary<string, byte[]> CompanionBytes()
            {
                byte[] init = File.ReadAllBytes(Path.Combine(root, "Init.bytes"));
                string version;
                using (var reader = new BinaryReader(new MemoryStream(init)))
                {
                    reader.BaseStream.Position = 4;
                    version = reader.ReadUInt32().ToString(CultureInfo.InvariantCulture);
                }
                Assert.That(GameplayPublication.Read(root, null, out GameplayPublication publication, out string error),
                    Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(publication.TryFindAction(Action.ActionName, out GameplayAction action), Is.True);
                Assert.That(publication.TryFindPack(action.OwnerPackStableId, out PackManifestEntry pack), Is.True);
                return new Dictionary<string, byte[]>
                {
                    ["Init.bytes"] = init,
                    ["Init.json"] = Encoding.UTF8.GetBytes("{\"schemaVersion\":" + version + ",\"variables\":[]}"),
                    ["Play_U4_A.bytes"] = File.ReadAllBytes(Path.Combine(root, pack.Path)),
                    ["Play_U4_A.json"] = Encoding.UTF8.GetBytes("{\"schemaVersion\":" + version +
                        ",\"name\":\"" + action.Name + "\",\"guid\":\"" + action.ActionStableId + "\"}")
                };
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

            public IEnumerator FirstAudible(HarkenSpatialPlayerLoopProbe driver,
                Func<ActionFiring> firing, Action<Vector2> accept)
            {
                bool audible = false;
                int blocks = 0;
                // Installation ACKs use ordinary PlayerLoop service. Never pump a logical update here.
                for (int frame = 0; frame < 32 && !audible; ++frame)
                {
                    driver.AfterLate = () =>
                    {
                        if (firing().Handle == 0) return;
                        Vector2 energy = RenderPhysicalBlocks(1);
                        ++blocks;
                        Assert.That(firing().Query(out VnActionFiringInfo info), Is.EqualTo(VnResult.VN_OK));
                        if (energy.x + energy.y <= 0.000001f) return;
                        var snapshot = HarkenEngine.DefaultSnapshot();
                        Assert.That(HarkenEngine.Capture(ref snapshot), Is.EqualTo(VnResult.VN_OK));
                        Debug.Log($"SPATIAL_FIRST_AUDIBLE blocks={blocks} block={snapshot.blockIndex} state={info.state} left={energy.x:R} right={energy.y:R}");
                        audible = true;
                        accept(energy);
                    };
                    yield return null;
                }
                Assert.That(audible, Is.True, "Normal installation service did not produce an audible block.");
            }

            public Vector2 RenderPhysicalBlocks(int blockCount)
            {
                PackBootstrapSession session = Runtime.Session;
                Vector2 energy = default;
                for (int block = 0; block < blockCount; ++block)
                {
                    // The bridge requires exactly one newly rendered block before copying its samples.
                    Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length, out uint frames, out uint channels),
                        Is.EqualTo(VnResult.VN_OK));
                    Assert.That(channels, Is.EqualTo(2));
                    Assert.That(frames, Is.GreaterThan(0));
                    if (block < blockCount - 4) continue;
                    for (int frame = 0; frame < frames; ++frame)
                    {
                        energy.x += samples[frame * 2] * samples[frame * 2];
                        energy.y += samples[frame * 2 + 1] * samples[frame * 2 + 1];
                    }
                }
                Assert.That(Runtime.Session, Is.SameAs(session));
                Assert.That(session.IsActive, Is.True);
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

    [DefaultExecutionOrder(9999)]
    public sealed class HarkenSpatialBeforeBoundaryProbe : MonoBehaviour
    {
        internal Action OnLate;
        internal int LateUpdateCount { get; private set; }
        private void LateUpdate()
        {
            ++LateUpdateCount;
            Action callback = OnLate;
            OnLate = null;
            callback?.Invoke();
        }
    }

    [DefaultExecutionOrder(10001)]
    public sealed class HarkenSpatialPlayerLoopProbe : MonoBehaviour
    {
        internal Action BeforeLate;
        internal Action AfterLate;

        private void Update()
        {
            Action callback = BeforeLate;
            BeforeLate = null;
            callback?.Invoke();
        }

        private void LateUpdate()
        {
            Action callback = AfterLate;
            AfterLate = null;
            callback?.Invoke();
        }
    }
}
