using System;
using System.Collections;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Harken;
using Harken.Unity;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace HarkenCrossProduct
{
    [Serializable]
    public sealed class Inputs
    {
        public string projectId;
        public string main;
        public string renamed;
        public string otherProject;
        public string platformRoot;
        public string[] platforms;
        public string[] languages;
        public string toneAction;
        public string toneActionId;
        public string tonePackStableId;
        public string shortAction;
        public string spatialAction;
        public string flatAction;
        public string localizedAction;
        public string renamedAction;
        public string variable;
        public MeterLayout meter;

        public static Inputs Load()
        {
            string path = Path.Combine(Application.streamingAssetsPath, "HarkenCrossProduct", "inputs.json");
            return JsonUtility.FromJson<Inputs>(File.ReadAllText(path));
        }

        public static string Absolute(string relative) => Path.Combine(Application.streamingAssetsPath, relative);
    }

    [Serializable]
    public sealed class MeterLayout
    {
        public int size;
        public int mixChannelCount;
        public int busCount;
        public int channelPeaks;
        public int maxChannels;
    }

    public static class DevelopmentMeter
    {
        [DllImport("HarkenEngineDev", CallingConvention = CallingConvention.Cdecl)]
        private static extern void VnPreview_PumpNullMixer(uint blockCount);

        [DllImport("HarkenEngineDev", CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult VnPreview_EnableChannelPeakMetering();

        [DllImport("HarkenEngineDev", CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult VnPreview_TakeChannelPeaks(IntPtr peaks);

        public static void Advance(uint blocks) => VnPreview_PumpNullMixer(blocks);

        public static void Enable() => Assert.That(VnPreview_EnableChannelPeakMetering(), Is.EqualTo(VnResult.VN_OK));

        public static float[] Pump(MeterLayout layout, uint blocks)
        {
            var channels = new float[2];
            IntPtr buffer = Marshal.AllocHGlobal(layout.size);
            try
            {
                for (uint i = 0; i < blocks; ++i)
                {
                    VnPreview_PumpNullMixer(1);
                    for (int b = 0; b < layout.size; ++b) Marshal.WriteByte(buffer, b, 0);
                    Marshal.WriteInt32(buffer, 0, layout.size);
                    Assert.That(VnPreview_TakeChannelPeaks(buffer), Is.EqualTo(VnResult.VN_OK));
                    int mix = Marshal.ReadInt32(buffer, layout.mixChannelCount);
                    int buses = Marshal.ReadInt32(buffer, layout.busCount);
                    for (int bus = 0; bus < buses; ++bus)
                        for (int channel = 0; channel < Math.Min(mix, channels.Length); ++channel)
                        {
                            int offset = layout.channelPeaks + (bus * layout.maxChannels + channel) * sizeof(float);
                            float peak = BitConverter.ToSingle(BitConverter.GetBytes(Marshal.ReadInt32(buffer, offset)), 0);
                            channels[channel] = Math.Max(channels[channel], peak);
                        }
                }
            }
            finally { Marshal.FreeHGlobal(buffer); }
            return channels;
        }

        public static float Peak(float[] channels) => Math.Max(channels[0], channels[1]);
        public static float Balance(float[] channels) => channels[0] - channels[1];
    }

    public sealed class HarkenCrossProductPlayModeTests
    {
        private const float Audible = 0.01f;
        private const float Silent = 0.001f;
        private const uint PlayBlocks = 40;
        private const float StartupSeconds = 30;

        private Inputs inputs;
        private GameObject owner;
        private HarkenAudioSystem runtime;

        [SetUp]
        public void SetUp()
        {
            LogAssert.ignoreFailingMessages = true;
            inputs = Inputs.Load();
        }

        [UnityTearDown]
        public IEnumerator TearDown()
        {
            if (runtime != null && runtime.Session != null && runtime.Session.IsActive) runtime.ShutdownRuntime();
            if (owner != null) Object.Destroy(owner);
            owner = null;
            runtime = null;
            yield return null;
        }

        private void CreateRuntime(string language = null, string platform = null)
        {
            owner = new GameObject("Harken Cross-Product Runtime");
            owner.SetActive(false);
            runtime = owner.AddComponent<HarkenAudioSystem>();
            Assert.That(runtime.ConfigureBeforeStartup("HarkenCrossProduct", "Init.bytes", Array.Empty<ulong>(), true, false),
                Is.EqualTo(VnResult.VN_OK));
            if (language != null) Assert.That(runtime.ConfigureLanguageBeforeStartup(language), Is.EqualTo(VnResult.VN_OK));
            if (platform != null) Assert.That(runtime.ConfigurePlatformBeforeStartup(null, platform), Is.EqualTo(VnResult.VN_OK));
        }

        private IEnumerator StartFrom(PackSource source, string language = null)
        {
            CreateRuntime(language);
            Assert.That(runtime.ConfigureCompanionSourceBeforeStartup(source), Is.EqualTo(VnResult.VN_OK));
            owner.SetActive(true);
            yield return WaitForStartup();
        }

        private IEnumerator WaitForStartup()
        {
            float deadline = Time.realtimeSinceStartup + StartupSeconds;
            while (runtime.State == HarkenRuntimeState.Starting && Time.realtimeSinceStartup < deadline) yield return null;
        }

        private void AssertReady() =>
            Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Ready), runtime.LastStartupError);

        private static IEnumerator Await(Task task)
        {
            float deadline = Time.realtimeSinceStartup + StartupSeconds;
            while (!task.IsCompleted && Time.realtimeSinceStartup < deadline) yield return null;
            Assert.That(task.IsCompleted, Is.True, "task did not complete");
        }

        private IEnumerator Measure(Action<float[]> result, uint blocks = PlayBlocks)
        {
            yield return null;
            result(DevelopmentMeter.Pump(inputs.meter, blocks));
        }

        private string Main => Inputs.Absolute(inputs.main);

        private ActionPreparation Prepare(params string[] actions)
        {
            Assert.That(runtime.Session.PrepareActions(actions, inputs.projectId, out ActionPreparation preparation),
                Is.EqualTo(VnResult.VN_OK));
            return preparation;
        }

        private static VnVec3 Vec(float x, float y, float z) => new VnVec3 { x = x, y = y, z = z };

        [UnityTest]
        public IEnumerator DirectorySourceReachesReadyAndShutsDown()
        {
            yield return StartFrom(PackSource.FromDirectory(Main));
            AssertReady();
            Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
        }

        [UnityTest]
        public IEnumerator ReaderAndAsyncReaderSourcesStart()
        {
            string main = Main;
            CreateRuntime();
            owner.SetActive(true);
            yield return WaitForStartup();
            if (runtime.Session != null && runtime.Session.IsActive) runtime.ShutdownRuntime();
            Assert.That(runtime.StartRuntime(PackSource.FromReader(name => File.ReadAllBytes(Path.Combine(main, name)))),
                Is.EqualTo(VnResult.VN_OK));
            AssertReady();
            Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));

            Task<VnResult> started = runtime.StartRuntimeAsync(PackSource.FromAsyncReader(
                (name, cancellation) => Task.FromResult(File.ReadAllBytes(Path.Combine(main, name)))));
            yield return Await(started);
            Assert.That(started.Result, Is.EqualTo(VnResult.VN_OK));
            AssertReady();
        }

        [UnityTest]
        public IEnumerator UnsupportedMetadataAndMissingInitRefuseThenRetry()
        {
            string copy = Path.Combine(Application.temporaryCachePath, "HarkenCrossProduct-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(copy);
            foreach (string file in Directory.GetFiles(Main)) File.Copy(file, Path.Combine(copy, Path.GetFileName(file)));
            string init = Path.Combine(copy, "Init.bytes");
            string initJson = Path.Combine(copy, "Init.json");
            byte[] initBytes = File.ReadAllBytes(init);
            string metadata = File.ReadAllText(initJson);
            try
            {
                File.Delete(init);
                yield return StartFrom(PackSource.FromDirectory(copy));
                Assert.That(runtime.State, Is.Not.EqualTo(HarkenRuntimeState.Ready), "missing Init must refuse startup");

                File.WriteAllBytes(init, initBytes);
                File.WriteAllText(initJson, System.Text.RegularExpressions.Regex.Replace(
                    metadata, "\"packMetadataVersion\"\\s*:\\s*\\d+", "\"packMetadataVersion\": 999999"));
                Assert.That(runtime.StartRuntime(PackSource.FromDirectory(copy)), Is.Not.EqualTo(VnResult.VN_OK),
                    "unsupported metadata version must refuse startup");

                File.WriteAllText(initJson, metadata);
                Assert.That(runtime.StartRuntime(PackSource.FromDirectory(copy)), Is.EqualTo(VnResult.VN_OK));
                AssertReady();
            }
            finally
            {
                if (runtime != null && runtime.Session != null && runtime.Session.IsActive) runtime.ShutdownRuntime();
                Directory.Delete(copy, true);
            }
        }

        [UnityTest]
        public IEnumerator PostedActionIsAudibleAndStopsSilenceIt()
        {
            yield return StartFrom(PackSource.FromDirectory(Main));
            AssertReady();
            DevelopmentMeter.Enable();

            Assert.That(runtime.PlayAction(inputs.toneAction, inputs.projectId, out ActionFiring actionFiring), Is.EqualTo(VnResult.VN_OK));
            float[] playing = null;
            yield return Measure(value => playing = value);
            Assert.That(DevelopmentMeter.Peak(playing), Is.GreaterThan(Audible), "posted Action is audible");
            Assert.That(actionFiring.Stop(0), Is.EqualTo(VnResult.VN_OK));
            float[] stopped = null;
            yield return Measure(value => stopped = value);
            yield return Measure(value => stopped = value, 4);
            Assert.That(DevelopmentMeter.Peak(stopped), Is.LessThan(Silent), "Action stop silences audio");

            ActionPreparation tone = Prepare(inputs.toneAction);
            Assert.That(Emitter.Create(out Emitter emitter), Is.EqualTo(VnResult.VN_OK));
            Assert.That(Actions.Post(inputs.toneAction, emitter, out ActionFiring emitterFiring), Is.EqualTo(VnResult.VN_OK));
            yield return Measure(value => playing = value);
            Assert.That(DevelopmentMeter.Peak(playing), Is.GreaterThan(Audible));
            Assert.That(emitter.Stop(0), Is.EqualTo(VnResult.VN_OK));
            yield return Measure(value => stopped = value);
            yield return Measure(value => stopped = value, 4);
            Assert.That(DevelopmentMeter.Peak(stopped), Is.LessThan(Silent), "emitter stop silences audio");
            emitter.Destroy();
            Assert.That(tone.Release(), Is.EqualTo(VnResult.VN_OK));

            Assert.That(runtime.PlayAction(inputs.shortAction, inputs.projectId, out ActionFiring shortFiring), Is.EqualTo(VnResult.VN_OK));
            yield return Measure(_ => { }, 400);
            yield return null;
            VnResult query = shortFiring.Query(out VnActionFiringInfo info);
            Assert.That(query != VnResult.VN_OK || info.livePlayableCount == 0, Is.True, "natural completion releases playables");
        }

        [UnityTest]
        public IEnumerator EmitterAndListenerMovementReachAudioAndFlatPlaybackIgnoresIt()
        {
            yield return StartFrom(PackSource.FromDirectory(Main));
            AssertReady();
            DevelopmentMeter.Enable();
            ActionPreparation preparation = Prepare(inputs.spatialAction, inputs.flatAction);
            Assert.That(Listener.Set(Vec(0, 0, 0), Vec(0, 0, 1), Vec(0, 1, 0)), Is.EqualTo(VnResult.VN_OK));
            Assert.That(Emitter.Create(out Emitter emitter), Is.EqualTo(VnResult.VN_OK));
            try
            {
                Assert.That(emitter.SetPosition(Vec(-5, 0, 0)), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post(inputs.spatialAction, emitter, out _), Is.EqualTo(VnResult.VN_OK));
                float[] first = null, moved = null, listenerMoved = null;
                yield return Measure(value => first = value);
                Assert.That(emitter.SetPosition(Vec(5, 0, 0)), Is.EqualTo(VnResult.VN_OK));
                yield return Measure(value => moved = value);
                Assert.That(Math.Sign(DevelopmentMeter.Balance(first)), Is.Not.EqualTo(Math.Sign(DevelopmentMeter.Balance(moved))),
                    "emitter movement changes the stereo balance");
                Assert.That(Listener.Set(Vec(10, 0, 0), Vec(0, 0, 1), Vec(0, 1, 0)), Is.EqualTo(VnResult.VN_OK));
                yield return Measure(value => listenerMoved = value);
                Assert.That(Math.Sign(DevelopmentMeter.Balance(listenerMoved)), Is.Not.EqualTo(Math.Sign(DevelopmentMeter.Balance(moved))),
                    "listener movement changes the stereo balance");
                emitter.Stop(0);
                yield return Measure(_ => { }, 8);

                Assert.That(Listener.Set(Vec(0, 0, 0), Vec(0, 0, 1), Vec(0, 1, 0)), Is.EqualTo(VnResult.VN_OK));
                Assert.That(emitter.SetPosition(Vec(-5, 0, 0)), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Actions.Post(inputs.flatAction, emitter, out _), Is.EqualTo(VnResult.VN_OK));
                float[] flatLeft = null, flatRight = null;
                yield return Measure(value => flatLeft = value);
                Assert.That(emitter.SetPosition(Vec(5, 0, 0)), Is.EqualTo(VnResult.VN_OK));
                yield return Measure(value => flatRight = value);
                Assert.That(DevelopmentMeter.Peak(flatLeft), Is.GreaterThan(Audible));
                Assert.That(Math.Abs(DevelopmentMeter.Balance(flatLeft) - DevelopmentMeter.Balance(flatRight)),
                    Is.LessThan(Audible), "2D playback ignores spatial movement");
            }
            finally
            {
                emitter.Destroy();
                preparation.Release();
            }
        }

        [UnityTest]
        public IEnumerator RuntimeLanguageSelectsItsContent()
        {
            foreach (string language in inputs.languages)
            {
                yield return StartFrom(PackSource.FromDirectory(Main, language), language);
                AssertReady();
                Assert.That(runtime.Language, Is.EqualTo(language));
                DevelopmentMeter.Enable();
                Assert.That(runtime.PlayAction(inputs.localizedAction, inputs.projectId, out _), Is.EqualTo(VnResult.VN_OK), language);
                float[] peaks = null;
                yield return Measure(value => peaks = value);
                Assert.That(DevelopmentMeter.Peak(peaks), Is.GreaterThan(Audible), language);
                yield return TearDown();
            }
            yield return StartFrom(PackSource.FromDirectory(Main, "NoSuchLanguage"), "NoSuchLanguage");
            Assert.That(runtime.State, Is.Not.EqualTo(HarkenRuntimeState.Ready), "an unexported language is refused");
        }

        [UnityTest]
        public IEnumerator PlatformDirectoriesLoadByName()
        {
            foreach (string platform in inputs.platforms)
            {
                CreateRuntime(null, platform);
                owner.SetActive(true);
                yield return WaitForStartup();
                if (runtime.Session != null && runtime.Session.IsActive) runtime.ShutdownRuntime();
                Assert.That(runtime.StartRuntime(Inputs.Absolute(inputs.platformRoot)), Is.EqualTo(VnResult.VN_OK), platform);
                AssertReady();
                Assert.That(Path.GetFileName(runtime.PackRootPath.TrimEnd('/', '\\')), Is.EqualTo(platform));
                yield return TearDown();
            }
        }

        [UnityTest]
        public IEnumerator RenamedActionKeepsItsIdentityAndAnotherProjectIsRefused()
        {
            yield return StartFrom(PackSource.FromDirectory(Inputs.Absolute(inputs.renamed)));
            AssertReady();
            Assert.That(runtime.PlayAction(inputs.renamedAction, inputs.projectId, out _), Is.EqualTo(VnResult.VN_OK));
            Assert.That(runtime.PlayAction(inputs.toneAction, inputs.projectId, out _), Is.Not.EqualTo(VnResult.VN_OK),
                "the old name no longer resolves");
            Assert.That(Actions.Resolve(inputs.renamedAction, inputs.projectId, out ActionResolved resolved), Is.EqualTo(VnResult.VN_OK));
            Assert.That(resolved.ActionId, Is.EqualTo(VnObjectId.Parse(inputs.toneActionId)), "renaming keeps the Action identity");
            yield return TearDown();

            yield return StartFrom(PackSource.FromDirectory(Inputs.Absolute(inputs.otherProject)));
            AssertReady();
            Assert.That(runtime.PlayAction(inputs.toneAction, inputs.projectId, out _), Is.Not.EqualTo(VnResult.VN_OK),
                "content of another project does not satisfy this project's name");
        }

        [UnityTest]
        public IEnumerator ExplicitPackOwnershipFollowsReferencesAndDoesNotCrossSessions()
        {
            ulong packId = ulong.Parse(inputs.tonePackStableId);
            VnObjectId actionId = VnObjectId.Parse(inputs.toneActionId);
            yield return StartFrom(PackSource.FromDirectory(Main));
            AssertReady();
            PackBootstrapSession session = runtime.Session;
            Assert.That(session.PrepareActions(new[] { inputs.toneAction }, inputs.projectId, out ActionPreparation preparation),
                Is.EqualTo(VnResult.VN_OK));
            Assert.That(session.AcquirePack(packId, out Pack first), Is.EqualTo(VnResult.VN_OK));
            Assert.That(session.AcquirePack(packId, out Pack second), Is.EqualTo(VnResult.VN_OK));
            Assert.That(preparation.Release(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(session.ReleasePack(packId, first), Is.EqualTo(VnResult.VN_OK));
            Assert.That(Actions.Post(actionId, Emitter.None, out ActionFiring held), Is.EqualTo(VnResult.VN_OK),
                "content stays loaded while a reference remains");
            held.Stop(0);
            yield return null;
            Assert.That(session.ReleasePack(packId, second), Is.EqualTo(VnResult.VN_OK));
            yield return Measure(_ => { }, 4);
            Assert.That(Actions.Post(actionId, Emitter.None, out _), Is.Not.EqualTo(VnResult.VN_OK),
                "content unloads after the last reference");

            Assert.That(session.AcquirePack(packId, out _), Is.EqualTo(VnResult.VN_OK));
            Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
            Assert.That(runtime.StartRuntime(PackSource.FromDirectory(Main)), Is.EqualTo(VnResult.VN_OK));
            Assert.That(Actions.Post(actionId, Emitter.None, out _), Is.Not.EqualTo(VnResult.VN_OK),
                "a new session does not inherit the previous session's references");
        }

        [UnityTest]
        public IEnumerator VariableWriteByNameIsAccepted()
        {
            yield return StartFrom(PackSource.FromDirectory(Main));
            AssertReady();
            Assert.That(Values.Resolve(inputs.variable, out VnObjectId variableId), Is.EqualTo(VnResult.VN_OK));
            Assert.That(Values.SetValue(inputs.variable, 0.5f), Is.EqualTo(VnResult.VN_OK));
            yield return null;
            Assert.That(Values.QueryVariable(HarkenVariableId.FromGenerated(inputs.variable, variableId),
                out VnVariableQueryResultV1 _), Is.EqualTo(VnResult.VN_OK));
        }
    }
}
