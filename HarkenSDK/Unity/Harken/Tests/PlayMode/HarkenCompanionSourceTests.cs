#if UNITY_EDITOR
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using UnityEditor;
using UnityEngine;
using UnityEngine.TestTools;
using Object = UnityEngine.Object;

namespace Harken.Unity.Tests
{
    public sealed class HarkenCompanionSourceTests
    {
        private static string Root => Path.Combine(Application.streamingAssetsPath, "Audio", "CompanionPacks");
        private static byte[] Read(string path)
        {
            string file = Path.Combine(Root, path);
            return File.Exists(file) ? File.ReadAllBytes(file) : null;
        }
        private static void Set(object target, string name, object value)
            => target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).SetValue(target, value);

        [UnityTest]
        public IEnumerator TextAssetsUseFrozenReferencesAndTheExistingSynchronousPlayEntry()
        {
            using (var fixture = new Fixture())
            {
                var assets = ScriptableObject.CreateInstance<HarkenPackSource>();
                string imported = "Assets/HarkenCompanionTest_" + Guid.NewGuid().ToString("N") + "/";
                try
                {
                    Directory.CreateDirectory(imported);
                    foreach (string name in new[] { "Init.bytes", "Init.json", "Play_Head.bytes", "Play_Head.json" })
                    {
                        File.Copy(Path.Combine(Root, name), imported + name);
                        AssetDatabase.ImportAsset(imported + name, ImportAssetOptions.ForceSynchronousImport);
                    }
                    Set(assets, "initPack", AssetDatabase.LoadAssetAtPath<TextAsset>(imported + "Init.bytes"));
                    Set(assets, "content", new[]
                    {
                        new HarkenPackSource.ContentAsset { relativePath = "Play_Head.bytes", asset = AssetDatabase.LoadAssetAtPath<TextAsset>(imported + "Play_Head.bytes") },
                        new HarkenPackSource.ContentAsset { relativePath = "Play_Head.json", asset = AssetDatabase.LoadAssetAtPath<TextAsset>(imported + "Play_Head.json") }
                    });
                    Assert.That(assets.CaptureCompanion(AssetDatabase.LoadAssetAtPath<TextAsset>(imported + "Init.json"),
                        out PackSource source, out string error), Is.EqualTo(VnResult.VN_OK), error);
                    Set(assets, "content", Array.Empty<HarkenPackSource.ContentAsset>());
                    Assert.That(fixture.Runtime.StartRuntime(source), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.Session.Publication, Is.Null);
                    Assert.That(Actions.Resolve("Play_Head", out _), Is.Not.EqualTo(VnResult.VN_OK));
                    Assert.That(fixture.Runtime.PlayAction("Play_Head", fixture.Runtime.Session.ProjectId, out ActionFiring firing),
                        Is.EqualTo(VnResult.VN_OK));
                    Assert.That(firing.IsValid, Is.True);
                }
                finally
                {
                    fixture.Runtime.ShutdownRuntime();
                    Object.Destroy(assets);
                    AssetDatabase.DeleteAsset(imported.TrimEnd('/'));
                }
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator UnsupportedVersionsRefuseStartupAndRefreshWithoutLosingPlayback()
        {
            using (var fixture = new Fixture())
            {
                string invalid = "Init.json";
                var source = PackSource.FromAsyncReader(async (path, token) =>
                {
                    await Task.Yield();
                    byte[] bytes = Read(path);
                    return path == invalid ? Encoding.UTF8.GetBytes(Regex.Replace(Encoding.UTF8.GetString(bytes),
                        "\"schemaVersion\"\\s*:\\s*3", "\"schemaVersion\":3.0000000000000001")) : bytes;
                });
                LogAssert.Expect(LogType.Error, "Harken operation failed: PackBootstrap.Start: VN_BAD_PACK");
                var start = fixture.Runtime.StartRuntimeAsync(source);
                yield return Until(start);
                Assert.That(start.GetAwaiter().GetResult(), Is.EqualTo(VnResult.VN_BAD_PACK));
                Assert.That(fixture.Runtime.Session, Is.Null);
                invalid = null;
                start = fixture.Runtime.StartRuntimeAsync(source);
                yield return Until(start);
                Assert.That(start.GetAwaiter().GetResult(), Is.EqualTo(VnResult.VN_OK));
                var session = fixture.Runtime.Session;
                var play = fixture.Runtime.PlayActionAsync("Play_Head", session.ProjectId);
                yield return Until(play);
                var played = play.GetAwaiter().GetResult();
                Assert.That(played.Result, Is.EqualTo(VnResult.VN_OK));
                invalid = "Play_Head.json";
                var refresh = session.RefreshActionAsync("Play_Head", session.ProjectId);
                yield return Until(refresh);
                Assert.That(refresh.GetAwaiter().GetResult().Result, Is.EqualTo(VnResult.VN_BAD_PACK));
                Assert.That(played.Firing.Query(out _), Is.EqualTo(VnResult.VN_OK));
                Assert.That(fixture.Runtime.PlayAction("Play_Head", session.ProjectId, out _), Is.EqualTo(VnResult.VN_OK));
                invalid = null;
                refresh = session.RefreshActionAsync("Play_Head", session.ProjectId);
                yield return Until(refresh);
                var corrected = refresh.GetAwaiter().GetResult();
                Assert.That(corrected.Result, Is.EqualTo(VnResult.VN_OK));
                Assert.That(corrected.Preparation.Release(), Is.EqualTo(VnResult.VN_OK));
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator CancelledStartupCannotClearNewStartupAndColdPlayWaitsForBytes()
        {
            using (var fixture = new Fixture())
            {
                var oldInit = new TaskCompletionSource<byte[]>();
                var newInit = new TaskCompletionSource<byte[]>();
                var content = new TaskCompletionSource<byte[]>();
                int reads = 0;
                var old = fixture.Runtime.StartRuntimeAsync(PackSource.FromAsyncReader((path, token) =>
                    path == "Init.bytes" ? oldInit.Task : Task.FromResult(Read(path))));
                fixture.Owner.SetActive(false);
                fixture.Owner.SetActive(true);
                var current = fixture.Runtime.StartRuntimeAsync(PackSource.FromAsyncReader((path, token) =>
                {
                    if (path == "Init.bytes") return newInit.Task;
                    if (path == "Play_Head.bytes") { ++reads; return content.Task; }
                    return Task.FromResult(Read(path));
                }));
                oldInit.SetResult(Read("Init.bytes"));
                yield return Until(old);
                Assert.That(old.IsCanceled, Is.True);
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Starting));
                Assert.That(current.IsCompleted, Is.False);
                newInit.SetResult(Read("Init.bytes"));
                yield return Until(current);
                Assert.That(current.GetAwaiter().GetResult(), Is.EqualTo(VnResult.VN_OK));
                string project = fixture.Runtime.Session.ProjectId;
                using (var cancel = new CancellationTokenSource())
                {
                    var first = fixture.Runtime.PlayActionAsync("Play_Head", project, cancel.Token);
                    var second = fixture.Runtime.PlayActionAsync("Play_Head", project);
                    Assert.That(reads, Is.EqualTo(1));
                    Assert.That(second.IsCompleted, Is.False);
                    cancel.Cancel();
                    yield return Until(first);
                    Assert.That(first.IsCanceled, Is.True);
                    content.SetResult(Read("Play_Head.bytes"));
                    yield return Until(second);
                    var result = second.GetAwaiter().GetResult();
                    Assert.That(result.Result, Is.EqualTo(VnResult.VN_OK));
                    Assert.That(result.Firing.IsValid, Is.True);
                    Assert.That(fixture.Runtime.PlayAction("Play_Head", project, out _), Is.EqualTo(VnResult.VN_OK));
                    Assert.That(reads, Is.EqualTo(1));
                }
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator CallerCancelledStartupReleasesCallbacksBeforeRetry()
        {
            using (var fixture = new Fixture())
            using (var cancel = new CancellationTokenSource())
            {
                var init = new TaskCompletionSource<byte[]>();
                var start = fixture.Runtime.StartRuntimeAsync(PackSource.FromAsyncReader((path, token) =>
                    path == "Init.bytes" ? init.Task : Task.FromResult(Read(path))), cancel.Token);
                HarkenCallbacks.Registration Registration(string name) => (HarkenCallbacks.Registration)
                    typeof(HarkenAudioSystem).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)
                        .GetValue(fixture.Runtime);
                var notification = Registration("notificationRegistration");
                var log = Registration("logRegistration");
                Assert.That(notification.IsActive && log.IsActive, Is.True);
                cancel.Cancel();
                yield return Until(start);
                Assert.That(start.IsCanceled, Is.True);
                Assert.That(fixture.Runtime.State, Is.EqualTo(HarkenRuntimeState.Stopped));
                Assert.That(notification.IsActive || log.IsActive, Is.False);
                Assert.That(fixture.Runtime.StartRuntime(PackSource.FromReader(Read)), Is.EqualTo(VnResult.VN_OK));
                init.SetResult(Read("Init.bytes"));
                Assert.That(Registration("notificationRegistration").IsActive, Is.True);
                Assert.That(Registration("logRegistration").IsActive, Is.True);
            }
            yield return null;
        }

        [UnityTest]
        public IEnumerator StreamingReaderDistinguishesMissingPairsFromReadErrors()
        {
            using (var fixture = new Fixture())
            using (var server = new SourceServer())
            {
                var start = fixture.Runtime.StartRuntimeAsync(HarkenStreamingPackSource.Create(server.Root, "French"));
                yield return Until(start);
                Assert.That(start.GetAwaiter().GetResult(), Is.EqualTo(VnResult.VN_OK));
                var failed = fixture.Runtime.Session.PrepareActionsAsync(new[] { "Play_Head" }, fixture.Runtime.Session.ProjectId);
                yield return Until(failed);
                Assert.That(failed.GetAwaiter().GetResult().Result, Is.EqualTo(VnResult.VN_PACK_PROVIDER_FAILED));
                Assert.That(server.GeneralReads, Is.Zero, "500 must not fall back to General");
                server.Fail = false;
                var play = fixture.Runtime.PlayActionAsync("Play_Head", fixture.Runtime.Session.ProjectId);
                yield return Until(play);
                Assert.That(play.GetAwaiter().GetResult().Result, Is.EqualTo(VnResult.VN_OK));
                Assert.That(server.GeneralReads, Is.EqualTo(2), "two explicit 404 responses permit one General pair");
            }
            yield return null;
        }

        private static IEnumerator Until(Task task)
        {
            float deadline = Time.realtimeSinceStartup + 15;
            while (!task.IsCompleted && Time.realtimeSinceStartup < deadline) yield return null;
            Assert.That(task.IsCompleted, Is.True, "Async resource request timed out.");
        }

        private sealed class Fixture : IDisposable
        {
            internal readonly GameObject Owner = new GameObject("Companion source Runtime");
            internal readonly HarkenAudioSystem Runtime;
            private readonly HarkenProjectSettingsLease settings = HarkenProjectSettingsLease.Acquire();
            internal Fixture()
            {
                Owner.SetActive(false);
                Runtime = Owner.AddComponent<HarkenAudioSystem>();
                Set(Runtime, "autoInitialize", false);
                Set(Runtime, "persistAcrossScenes", false);
                Assert.That(Runtime.ConfigureBeforeStartup("Unused", "Init.bytes", Array.Empty<ulong>(), true, false), Is.EqualTo(VnResult.VN_OK));
                Assert.That(Runtime.ConfigurePlatformBeforeStartup(null), Is.EqualTo(VnResult.VN_OK));
                Owner.SetActive(true);
            }
            public void Dispose()
            {
                Runtime.ShutdownRuntime();
                Object.Destroy(Owner);
                settings.Dispose();
            }
        }

        private sealed class SourceServer : IDisposable
        {
            private readonly HttpListener listener = new HttpListener();
            private readonly Task serving;
            private readonly string directory = HarkenCompanionSourceTests.Root;
            internal readonly string Root;
            internal volatile bool Fail = true;
            internal int GeneralReads;
            internal SourceServer()
            {
                var portProbe = new System.Net.Sockets.TcpListener(IPAddress.Loopback, 0);
                portProbe.Start();
                int port = ((IPEndPoint)portProbe.LocalEndpoint).Port;
                portProbe.Stop();
                Root = "http://127.0.0.1:" + port + "/";
                listener.Prefixes.Add(Root);
                listener.Start();
                serving = Task.Run(async () =>
                {
                    try
                    {
                        while (listener.IsListening)
                        {
                            HttpListenerContext context = await listener.GetContextAsync();
                            string path = Uri.UnescapeDataString(context.Request.Url.AbsolutePath.TrimStart('/'));
                            byte[] bytes = null;
                            if (path.StartsWith("French/", StringComparison.Ordinal)) context.Response.StatusCode = Fail ? 500 : 404;
                            else if (path == "Init.bytes" || path == "Init.json" ||
                                path == "Play_Head.bytes" || path == "Play_Head.json")
                            {
                                string file = Path.Combine(directory, path);
                                if (File.Exists(file)) bytes = File.ReadAllBytes(file);
                                context.Response.StatusCode = bytes == null ? 404 : 200;
                                if (path.StartsWith("Play_Head", StringComparison.Ordinal)) Interlocked.Increment(ref GeneralReads);
                            }
                            else context.Response.StatusCode = 404;
                            if (bytes != null) context.Response.OutputStream.Write(bytes, 0, bytes.Length);
                            context.Response.Close();
                        }
                    }
                    catch (HttpListenerException) when (!listener.IsListening) { }
                    catch (ObjectDisposedException) { }
                });
            }
            public void Dispose() { listener.Close(); serving.GetAwaiter().GetResult(); }
        }
    }
}
#endif
