using System;
using System.Collections;
using System.IO;
using System.Linq;
using System.Text;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Networking;
using UnityEngine.TestTools;

namespace Harken.Unity.Tests
{
    public sealed class HarkenPlatformDeliveryTests
    {
        private const string PackRoot = "Audio/PlatformContractPacks";

        [UnityTest]
        public IEnumerator TwoAndroidInstancesLoadByIdAndName()
        {
            string index = null;
            yield return ReadPackagedJson(PackManifest.IndexFileName, value => index = value);
            Assert.That(PackManifest.TryParseIndex(index, out var platforms), Is.True);
            Assert.That(platforms.Length, Is.EqualTo(2));
            Assert.That(platforms.All(platform => platform.BasePlatform == "Android"), Is.True);
            Assert.That(PackManifest.TrySelectPlatform(platforms, null, null, "Android",
                out _, out string ambiguity), Is.False);
            foreach (var platform in platforms) Assert.That(ambiguity, Does.Contain(platform.Id));

            var settingsLease = HarkenProjectSettingsLease.Acquire();
            settingsLease.Settings.SetPackDirectory(Path.Combine(Application.streamingAssetsPath, PackRoot));
            settingsLease.Settings.SetPlayerExports(platforms.Select(platform => Path.GetFileName(platform.Path)).ToArray());
            try
            {
            for (int i = 0; i < platforms.Length; ++i)
            {
                var platform = platforms[i];
                string manifest = null;
                yield return ReadPackagedJson(platform.Path + "/" + PackManifest.ManifestFileName,
                    value => manifest = value);
                Assert.That(PackManifest.TryParseManifest(manifest, out var actual, out var packs), Is.True);
                Assert.That(PackManifest.MatchesPlatform(platform, actual), Is.True);
                var owner = new GameObject("Platform " + platform.Name);
                owner.SetActive(false);
                var runtime = owner.AddComponent<HarkenAudioSystem>();
                try
                {
                    Assert.That(runtime.ConfigureBeforeStartup(PackRoot, packs.Single(pack => pack.Name == "Init").Path,
                        packs.Where(pack => pack.Name != "Init").Select(pack => pack.PackStableId).ToArray(),
                        true, false), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(i == 0 ? runtime.ConfigurePlatformBeforeStartup(platform.Id)
                        : runtime.ConfigurePlatformBeforeStartup(null, platform.Name), Is.EqualTo(VnResult.VN_OK));
                    owner.SetActive(true);
                    float deadline = Time.realtimeSinceStartup + 30;
                    while (runtime.State == HarkenRuntimeState.Starting && Time.realtimeSinceStartup < deadline)
                        yield return null;
                    Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Ready), runtime.LastStartupError);
                    Assert.That(Path.GetFileName(runtime.PackRootPath), Is.EqualTo(platform.Name));
                    Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
                }
                finally
                {
                    if (runtime.Session != null && runtime.Session.IsActive) runtime.ShutdownRuntime();
                    UnityEngine.Object.Destroy(owner);
                }
                yield return null;
            }
            }
            finally { settingsLease.Dispose(); }
        }

        private static IEnumerator ReadPackagedJson(string relativePath, Action<string> complete)
        {
            string source = Application.streamingAssetsPath.TrimEnd('/', '\\') + "/" + PackRoot + "/" + relativePath;
            if (!source.Contains("://")) source = new Uri(Path.GetFullPath(source)).AbsoluteUri;
            using (var request = UnityWebRequest.Get(source))
            {
                yield return request.SendWebRequest();
                Assert.That(request.result, Is.EqualTo(UnityWebRequest.Result.Success),
                    "Install the two-instance delivery fixture under StreamingAssets/" + PackRoot + ": " + request.error);
                complete(Encoding.UTF8.GetString(request.downloadHandler.data));
            }
        }
    }
}
