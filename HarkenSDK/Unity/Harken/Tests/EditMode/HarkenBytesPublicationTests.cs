using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using Harken.Unity.Editor;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenBytesPublicationTests
    {
        private const string ActionName = "Play_Random_Draw";
        private static string Source => Path.Combine(Application.streamingAssetsPath, "Audio", "GeneratedPacks", "Windows");

        [Test]
        public void NativeBytesPublicationIgnoresUnrelatedLegacyInitAndNeverUsesDoubleSuffix()
        {
            string folder = "Assets/HarkenBytes_" + Guid.NewGuid().ToString("N");
            Directory.CreateDirectory(folder);
            try
            {
                foreach (string file in Directory.GetFiles(Source))
                    if (!file.EndsWith(".meta")) File.Copy(file, folder + "/" + Path.GetFileName(file));
                Assert.That(File.Exists(folder + "/Init.bytes"), Is.True, "Run against the current producer's sample publication.");
                File.WriteAllBytes(folder + "/Init.pack", new byte[] { 1 });
                File.WriteAllBytes(folder + "/Init.pack.bytes", new byte[] { 2 });
                File.WriteAllBytes(folder + "/Init.bytes.bytes", new byte[] { 3 });
                File.WriteAllBytes(folder + "/HarkenPackManifest.json.bytes", new byte[] { 4 });
                File.WriteAllBytes(folder + "/HarkenEditorWaveforms.json.bytes", new byte[] { 5 });
                AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
                var catalog = new HarkenEditorCatalog();
                Assert.That(catalog.Refresh(Path.GetFullPath(folder)), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
                Assert.That(catalog.PreparedPacks, Is.Not.Null);
                Assert.That(catalog.Publication.InitPack.Path, Is.EqualTo("Init.bytes"));
                Assert.That(catalog.GetActionPreview(ActionName, out var preview, out string error), Is.EqualTo(VnResult.VN_OK), error);
                Assert.That(preview.Kind, Is.EqualTo(HarkenActionPreviewKind.RandomCandidates));
                Assert.That(preview.Candidates.Count, Is.EqualTo(2));
                AssetDatabase.DeleteAsset(folder + "/Init.bytes");
                Assert.That(catalog.Refresh(Path.GetFullPath(folder)), Is.Not.EqualTo(VnResult.VN_OK));
                Assert.That(catalog.Publication, Is.Null);
            }
            finally { AssetDatabase.DeleteAsset(folder); }
        }

        [TestCase(false)]
        [TestCase(true)]
        public void LegacyDiskPublicationsRemainPlayableAndRejectCompetingInit(bool wrapped)
        {
            string folder = Path.Combine(Path.GetTempPath(), "HarkenLegacy_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
            try
            {
                string legacy = Path.Combine(Application.streamingAssetsPath, "Audio", "U4Original");
                foreach (string file in Directory.GetFiles(legacy))
                    if (!file.EndsWith(".meta"))
                        File.Copy(file, Path.Combine(folder, Path.GetFileName(file) +
                            (wrapped && file.EndsWith(".pack") ? ".bytes" : "")));
                var catalog = new HarkenEditorCatalog();
                Assert.That(catalog.Refresh(folder), Is.EqualTo(VnResult.VN_OK), catalog.Diagnostic);
                Assert.That(catalog.GetPreparationOrder("Play_U4_A", out var ids, out string error), Is.EqualTo(VnResult.VN_OK), error);
                using (var preview = new HarkenEditorPreview())
                {
                    Assert.That(preview.Prepare(folder, null, ids, true), Is.EqualTo(VnResult.VN_OK), preview.Diagnostic);
                    Assert.That(preview.Play("Play_U4_A", catalog.Publication.ProjectId, out var firing), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(firing.IsValid, Is.True);
                }
                File.WriteAllBytes(Path.Combine(folder, wrapped ? "Init.pack" : "Init.pack.bytes"), new byte[] { 0 });
                Assert.That(catalog.Refresh(folder), Is.EqualTo(VnResult.VN_INVALID_ARG));
                Assert.That(catalog.Publication, Is.Null);
            }
            finally { Directory.Delete(folder, true); }
        }

        [TestCase(null, false, true)]
        [TestCase(null, true, true)]
        [TestCase("Init.bytes", false, true)]
        [TestCase("Init.bytes", true, true)]
        [TestCase("Init.pack", false, false)]
        [TestCase("Init.pack", true, false)]
        [TestCase("Missing.bytes", true, false)]
        public void StartupDistinguishesSerializedDefaultFromExplicitInit(string configured, bool prepared, bool accepted)
        {
            using (var lease = HarkenProjectSettingsLease.Acquire())
            {
                lease.Settings.SetInitPack("Init.pack");
                var owner = new GameObject("Bytes startup");
                owner.SetActive(false);
                var runtime = owner.AddComponent<HarkenAudioSystem>();
                try
                {
                    Set(runtime, "autoInitialize", false);
                    Set(runtime, "persistAcrossScenes", false);
                    Set(runtime, "nullDevice", true);
                    if (configured != null)
                        Assert.That(runtime.ConfigureBeforeStartup("Audio/GeneratedPacks", configured,
                            Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                    owner.SetActive(true);
                    typeof(HarkenAudioSystem).GetMethod("Awake", BindingFlags.Instance | BindingFlags.NonPublic).Invoke(runtime, null);
                    if (!accepted) LogAssert.Expect(LogType.Error, "Harken operation failed: PackBootstrap.Start: VN_PACK_STALE_REVISION");
                    VnResult result = prepared
                        ? runtime.StartRuntime(PackManifest.ManifestFileName, path => HarkenPackFiles.Read(Path.Combine(Source, path)))
                        : runtime.StartRuntime(Path.GetDirectoryName(Source));
                    Assert.That(result, Is.EqualTo(accepted ? VnResult.VN_OK : VnResult.VN_PACK_STALE_REVISION), runtime.LastStartupError);
                    Assert.That(runtime.IsReady, Is.EqualTo(accepted));
                }
                finally { runtime.ShutdownRuntime(); Object.DestroyImmediate(owner); }
            }
        }

#if HARKEN_SDK_TEST_NULL_PUMP
        [DllImport(HarkenNative.LibraryName, CallingConvention = CallingConvention.Cdecl)]
        private static extern VnResult HarkenSdkTest_PumpAndCopy(
            [Out] float[] samples, uint capacity, out uint frames, out uint channels);

        [Test]
        public void AssetBundleLoadsNativeBytesAndRandomProducesFreshAudio()
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
                Assert.That(HarkenPreparedPacks.Capture(PackManifest.ManifestFileName, Read,
                    out var prepared, out _, out string error), Is.EqualTo(VnResult.VN_OK), error);
                using (var preview = new HarkenEditorPreview())
                {
                    Set(preview, "forceNullDevice", true);
                    var scope = preview.CreateScope();
                    Assert.That(scope.Prepare(prepared, new[] { ActionName }), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(scope.Play(ActionName, out var firing), Is.EqualTo(VnResult.VN_OK));
                    var samples = new float[8192];
                    double energy = 0;
                    for (int i = 0; i < 32; ++i)
                    {
                        Assert.That(preview.Tick(), Is.EqualTo(VnResult.VN_OK));
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
            }
            finally
            {
                if (bundle != null) bundle.Unload(true);
                AssetDatabase.DeleteAsset(folder);
                Directory.Delete(output, true);
            }
        }
#endif
        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);
    }
}
