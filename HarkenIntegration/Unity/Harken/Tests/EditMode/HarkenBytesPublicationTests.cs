using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Tests
{
    public sealed class HarkenBytesPublicationTests
    {
        private const string ActionName = "Play_U4_A";
        private static string Source => Path.Combine(Application.streamingAssetsPath, "Audio", "U4Original");

#if HARKEN_SDK_TEST_NULL_PUMP
        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpAndCopy(
            [Out] float[] samples, uint capacity, out uint frames, out uint channels);

        [Test]
        public void AssetBundleLoadsCompanionPairsAndProducesAudio()
        {
            string folder = "Assets/HarkenBundle_" + Guid.NewGuid().ToString("N");
            string output = Path.Combine(Path.GetTempPath(), "HarkenBundle_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
            Directory.CreateDirectory(output);
            AssetBundle bundle = null;
            try
            {
                foreach (string file in Directory.GetFiles(Source))
                    if (!file.EndsWith(".meta")) File.Copy(file, folder + "/" + Path.GetFileName(file));
                AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
                string[] assets = Directory.GetFiles(folder).Where(p => !p.EndsWith(".meta")).Select(p => p.Replace('\\', '/')).ToArray();
                foreach (string path in assets.Where(p => p.EndsWith(".bytes")))
                    Assert.That(AssetDatabase.LoadAssetAtPath<TextAsset>(path), Is.Not.Null, path);
                var build = new AssetBundleBuild { assetBundleName = "audio", assetNames = assets,
                    addressableNames = assets.Select(Path.GetFileName).ToArray() };
                Assert.That(BuildPipeline.BuildAssetBundles(output, new[] { build }, BuildAssetBundleOptions.ForceRebuildAssetBundle,
                    BuildTarget.StandaloneWindows64), Is.Not.Null);
                bundle = AssetBundle.LoadFromFile(Path.Combine(output, "audio"));
                Assert.That(bundle, Is.Not.Null);
                var requested = new List<string>();
                byte[] Read(string path)
                {
                    requested.Add(path);
                    var asset = bundle.LoadAsset<TextAsset>(path);
                    return asset != null ? asset.bytes : null;
                }
                PackSource source = PackSource.FromReader(Read);
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                PackBootstrapSession session = null;
                try
                {
                    Assert.That(PackBootstrap.Start(source, ref config, out session), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(session.PrepareActions(new[] { ActionName }, session.ProjectId, out var preparation),
                        Is.EqualTo(VnResult.VN_OK));
                    Assert.That(Actions.Resolve(ActionName, session.ProjectId, out var action), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(Emitter.CreateStateOnly(out var emitter), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(Actions.Post(in action, emitter, 0, out var firing), Is.EqualTo(VnResult.VN_OK));
                    var samples = new float[8192];
                    double energy = 0;
                    for (int i = 0; i < 32; ++i)
                    {
                        Assert.That(HarkenEngine.Update(), Is.EqualTo(VnResult.VN_OK));
                        Array.Clear(samples, 0, samples.Length);
                        Assert.That(HarkenSdkTest_PumpAndCopy(samples, (uint)samples.Length, out uint frames, out uint channels),
                            Is.EqualTo(VnResult.VN_OK));
                        Assert.That(frames, Is.GreaterThan(0));
                        for (int j = 0; j < frames * channels; ++j) energy += samples[j] * samples[j];
                    }
                    Assert.That(energy, Is.GreaterThan(0.001));
                    Assert.That(requested, Does.Contain("Init.bytes"));
                    Assert.That(requested, Has.None.EndsWith(".pack"));
                    Assert.That(requested, Has.None.EndsWith(".bytes.bytes"));
                }
                finally { session?.Dispose(); }
            }
            finally
            {
                if (bundle != null) bundle.Unload(true);
                AssetDatabase.DeleteAsset(folder);
                Directory.Delete(output, true);
            }
        }
#endif
    }
}
