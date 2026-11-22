using System.Collections;
using System.Reflection;
using System.Text.RegularExpressions;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenStartupResultTests
    {
        [UnityTest]
        public IEnumerator AutomaticStartFailsWhenNoPackDirectoryIsSelected()
        {
            var owner = new GameObject("Unselected pack directory");
            owner.SetActive(false);
            var runtime = owner.AddComponent<HarkenAudioSystem>();
            using (var lease = HarkenProjectSettingsLease.Acquire())
            {
                lease.Settings.SetPackDirectory(string.Empty);
                Set(runtime, "packRoot", "Audio/GeneratedPacks");
                LogAssert.Expect(LogType.Error, new Regex("Harken operation failed: PackBootstrap.Start: " + VnResult.VN_INVALID_ARG));
                owner.SetActive(true);
                Assert.That(runtime.State, Is.EqualTo(HarkenRuntimeState.Failed));
                Assert.That(runtime.IsReady, Is.False);
                Assert.That(runtime.LastStartupError, Does.Not.Contain("StreamingAssets"));
                Assert.That(runtime.ShutdownRuntime(), Is.EqualTo(VnResult.VN_OK));
            }
            Object.Destroy(owner);
            yield return null;
        }

        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);
    }
}
