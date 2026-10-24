using System;
using System.Collections;
using System.IO;
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
        public IEnumerator TwoAndroidExportDirectoriesLoadByName()
        {
            string[] platforms = { "Android_Low", "Android_High" };
            var settingsLease = HarkenProjectSettingsLease.Acquire();
            settingsLease.Settings.SetPackDirectory(Path.Combine(Application.streamingAssetsPath, PackRoot));
            settingsLease.Settings.SetPlayerExports(platforms);
            try
            {
            for (int i = 0; i < platforms.Length; ++i)
            {
                var platform = platforms[i];
                string init = null;
                yield return ReadPackagedJson(platform + "/Init.json", value => init = value);
                Assert.That(init, Is.Not.Empty);
                var owner = new GameObject("Platform " + platform);
                owner.SetActive(false);
                var runtime = owner.AddComponent<HarkenAudioSystem>();
                try
                {
                    Assert.That(runtime.ConfigureBeforeStartup(PackRoot, "Init.bytes",
                        Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(runtime.ConfigurePlatformBeforeStartup(null, platform), Is.EqualTo(VnResult.VN_OK));
                    owner.SetActive(true);
                    float deadline = Time.realtimeSinceStartup + 30;
                    while (runtime.State == HarkenRuntimeState.Starting && Time.realtimeSinceStartup < deadline)
                        yield return null;
                    Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Ready), runtime.LastStartupError);
                    Assert.That(Path.GetFileName(runtime.PackRootPath), Is.EqualTo(platform));
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
