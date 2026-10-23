using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Harken;

namespace Harken.Test
{
    internal static class CompanionSourceContract
    {
        private static readonly string Root = Path.Combine(AppContext.BaseDirectory, "NativePublications", "UnityFixture", "CompanionPacks");
        private static byte[] Read(string path)
        {
            string file = Path.Combine(Root, path);
            return File.Exists(file) ? File.ReadAllBytes(file) : null;
        }
        private static VnConfig Config()
        {
            var config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            return config;
        }
        private static void Check(bool condition, string message)
        { if (!condition) throw new InvalidOperationException(message); }
        private static void Ok(VnResult result, string message) => Check(result == VnResult.VN_OK, message + ": " + result);

        internal static bool Run()
        {
            try
            {
                Synchronous();
                using (var context = new ControlContext()) context.Run(VersionedRefresh);
                using (var context = new ControlContext()) context.Run(Asynchronous);
                using (var context = new ControlContext()) context.Run(ReentrantMetadata);
                ReentrantShutdown();
                Console.WriteLine("[Companion sources] Reader snapshots, async sharing, cancellation, retry, rename and restart isolation PASS.");
                return true;
            }
            catch (Exception error) { Console.WriteLine("[Companion sources] FAIL: " + error); return false; }
        }

        private static void Synchronous()
        {
            int reads = 0;
            ulong revision = 1;
            bool mutate = false, halfPair = false, fail = false;
            var source = PackSource.FromReader(path =>
            {
                ++reads;
                if (path.StartsWith("French/", StringComparison.Ordinal))
                {
                    if (fail) throw new IOException("Read failure is not missing.");
                    return halfPair && path.EndsWith(".json", StringComparison.Ordinal) ? Encoding.UTF8.GetBytes("{}") : null;
                }
                if (mutate && path == "Play_Head.json") ++revision;
                return Read(path);
            }, "French", () => revision);
            var config = Config();
            Ok(PackBootstrap.Start(source, ref config, out PackBootstrapSession session), "reader Init");
            using (session)
            {
                Check(reads == 2 && session.Publication == null, "Init-only reader");
                halfPair = true;
                Check(session.PrepareActions(new[] { "Play_Head" }, session.ProjectId, out _) == VnResult.VN_BAD_PACK, "half pair cannot fall back");
                halfPair = false; fail = true;
                Check(session.PrepareActions(new[] { "Play_Head" }, session.ProjectId, out _) == VnResult.VN_PACK_PROVIDER_FAILED, "read error cannot fall back");
                fail = false; mutate = true;
                Check(session.PrepareActions(new[] { "Play_Head" }, session.ProjectId, out _) == VnResult.VN_PACK_STALE_REVISION, "mixed revision rejected");
                Check(Actions.Resolve("Play_Head", out _) != VnResult.VN_OK, "failed capture publishes no name");
                mutate = false;
                Ok(session.PrepareActions(new[] { "Play_Head" }, session.ProjectId, out ActionPreparation first), "reader retry");
                int after = reads;
                Ok(session.PrepareActions(new[] { "Play_Head" }, session.ProjectId, out ActionPreparation warm), "reader warm");
                Check(after == reads, "warm reads zero bytes");
                Ok(first.Release(), "reader first release"); Ok(warm.Release(), "reader last release");
            }
        }

        private static async Task Asynchronous()
        {
            int actionReads = 0;
            var gates = new Queue<TaskCompletionSource<byte[]>>();
            CancellationToken providerToken = default;
            bool gateReads = true;
            var source = PackSource.FromAsyncReader((path, token) =>
            {
                if (path == "Play_Head.bytes")
                {
                    ++actionReads;
                    providerToken = token;
                    if (gateReads)
                    {
                        var gate = new TaskCompletionSource<byte[]>();
                        gates.Enqueue(gate);
                        return gate.Task; // Deliberately ignores cancellation to exercise stale completion.
                    }
                }
                if (path == "Play_New.bytes") return Task.FromResult(Read("Play_Head.bytes"));
                if (path == "Play_New.json") return Task.FromResult(Encoding.UTF8.GetBytes(
                    Encoding.UTF8.GetString(Read("Play_Head.json")).Replace("Play_Head", "Play_New")));
                return Task.FromResult(Read(path));
            });
            PackStartupResult started = await PackBootstrap.StartAsync(source, Config());
            Ok(started.Result, "async Init");
            PackBootstrapSession session = started.Session;
            try
            {
                using (var cancel = new CancellationTokenSource())
                {
                    var first = session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId, cancel.Token);
                    var second = session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                    Check(actionReads == 1, "same-name reads coalesce");
                    await Task.Run(() => cancel.Cancel());
                    await Cancelled(first);
                    Check(!providerToken.IsCancellationRequested, "one subscriber cannot cancel another");
                    gates.Dequeue().SetResult(Read("Play_Head.bytes"));
                    ActionPreparationResult ready = await second;
                    Ok(ready.Result, "remaining subscriber prepares");
                    var warm = await session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                    Ok(warm.Result, "async warm");
                    Check(actionReads == 1, "async warm no read");
                    Ok(ready.Preparation.Release(), "shared subscriber release");
                    Ok(warm.Preparation.Release(), "warm subscriber release");
                }

                using (var cancel = new CancellationTokenSource())
                {
                    var old = session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId, cancel.Token);
                    var oldGate = gates.Dequeue();
                    CancellationToken oldToken = providerToken;
                    cancel.Cancel();
                    await Cancelled(old);
                    Check(oldToken.IsCancellationRequested, "last subscriber cancels underlying read");
                    var retry = session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                    var retryGate = gates.Dequeue();
                    oldGate.SetResult(Read("Play_Head.bytes"));
                    await Task.Yield();
                    int count = actionReads;
                    var joined = session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                    Check(count == actionReads, "old completion cannot remove new pending read");
                    retryGate.SetResult(Read("Play_Head.bytes"));
                    var a = await retry; var b = await joined;
                    Ok(a.Result, "retry prepares"); Ok(b.Result, "retry joined");

                    var stale = session.RefreshActionAsync("Play_Head", session.ProjectId);
                    var staleGate = gates.Dequeue();
                    var renamed = await session.RefreshActionAsync("Play_New", session.ProjectId);
                    Ok(renamed.Result, "concurrent explicit rename");
                    staleGate.SetResult(Read("Play_Head.bytes"));
                    Check((await stale).Result == VnResult.VN_PACK_STALE_REVISION, "old refresh cannot undo NameOnly");
                    Check(Actions.Resolve("Play_Head", out _) != VnResult.VN_OK, "old name remains retired");
                    Ok(Actions.Resolve("Play_New", out _), "new name survives stale capture");
                    Ok(a.Preparation.Release(), "old owner lease A"); Ok(b.Preparation.Release(), "old owner lease B");
                    Ok(renamed.Preparation.Release(), "new owner lease");
                }

                var pending = session.RefreshActionAsync("Play_Head", session.ProjectId);
                var late = gates.Dequeue();
                Ok(session.Shutdown(), "shutdown with pending capture");
                await Cancelled(pending);
                var config = Config();
                Ok(PackBootstrap.Start(PackSource.FromReader(Read), ref config, out PackBootstrapSession restarted), "restart while old reader pending");
                using (restarted)
                {
                    late.SetResult(Read("Play_Head.bytes"));
                    await Cancelled(pending);
                    Check(Actions.Resolve("Play_Head", out _) != VnResult.VN_OK, "old completion does not publish into restart");
                    Ok(restarted.PrepareActions(new[] { "Play_Head" }, restarted.ProjectId, out ActionPreparation scope), "new source works");
                    Ok(scope.Release(), "new source release");
                }
            }
            finally { session.Dispose(); }
        }

        private static async Task VersionedRefresh()
        {
            string invalid = "Init.json";
            var source = PackSource.FromAsyncReader(async (path, token) =>
            {
                await Task.Yield();
                byte[] bytes = Read(path);
                return path == invalid ? Encoding.UTF8.GetBytes(System.Text.RegularExpressions.Regex.Replace(
                    Encoding.UTF8.GetString(bytes), "\"schemaVersion\"\\s*:\\s*3", "\"schemaVersion\":999")) : bytes;
            });
            var start = await PackBootstrap.StartAsync(source, Config());
            Check(start.Result == VnResult.VN_BAD_PACK && start.Session == null, "async bad Init leaves no session");
            invalid = null;
            start = await PackBootstrap.StartAsync(source, Config());
            Ok(start.Result, "async corrected Init retries");
            using (var session = start.Session)
            {
                var ready = await session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                Ok(ready.Result, "versioned source prepares");
                Ok(Actions.Resolve("Play_Head", out ActionResolved resolved), "versioned source resolve");
                Ok(HarkenEngine.Update(), "versioned source update");
                Ok(Actions.Post(in resolved, Emitter.None, 50, out ActionFiring firing), "versioned source post");
                invalid = "Play_Head.json";
                Check((await session.RefreshActionAsync("Play_Head", session.ProjectId)).Result == VnResult.VN_BAD_PACK,
                    "async bad Action version rejected");
                Ok(Actions.Resolve("Play_Head", out _), "async version failure preserves name");
                Ok(Actions.Post(in resolved, Emitter.None, 51, out _), "async version failure preserves resolved token");
                Ok(firing.Query(out _), "async version failure preserves firing");
                invalid = null;
                var refreshed = await session.RefreshActionAsync("Play_Head", session.ProjectId);
                Ok(refreshed.Result, "async corrected Action retries");
                Ok(ready.Preparation.Release(), "original lease survives rejected refresh");
                Ok(refreshed.Preparation.Release(), "refreshed lease releases");
                Check(Actions.Resolve("Play_Head", out _) != VnResult.VN_OK, "no leaked lease after version retry");
            }
        }

        private static void ReentrantShutdown()
        {
            using (var context = new ControlContext()) context.Run(async () =>
            {
                PackBootstrapSession session = null;
                var source = PackSource.FromAsyncReader((path, token) =>
                {
                    if (path == "Play_Head.bytes") Ok(session.Shutdown(), "reentrant shutdown");
                    return Task.FromResult(Read(path));
                });
                var started = await PackBootstrap.StartAsync(source, Config());
                Ok(started.Result, "reentrant Init");
                session = started.Session;
                await Cancelled(session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId));
                Check(!session.IsActive, "read callback shutdown survives completion");
                using (var cancel = new CancellationTokenSource())
                {
                    var sync = PackSource.FromReader(path =>
                    {
                        if (path == "Play_Head.json") cancel.Cancel();
                        return Read(path);
                    });
                    var config = Config();
                    Ok(PackBootstrap.Start(sync, ref config, out session), "cancel sync Init");
                    using (session)
                    {
                        await Cancelled(session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId, cancel.Token));
                        Check(Actions.Resolve("Play_Head", out _) != VnResult.VN_OK, "cancel during sync capture does not publish");
                    }
                }
            });
        }

        private static async Task ReentrantMetadata()
        {
            PackBootstrapSession session = null;
            var source = PackSource.FromAsyncReader(async (path, token) =>
            {
                if (path == "Play_Head.bytes") await Task.Yield();
                if (path == "Play_Head.json")
                {
                    var nested = await session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                    Check(nested.Result == VnResult.VN_IN_USE, "metadata callback cannot await its own pending read");
                    await Task.Yield();
                    nested = await session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                    Check(nested.Result == VnResult.VN_IN_USE, "reader continuation cannot await its own pending read");
                }
                return Read(path);
            });
            var started = await PackBootstrap.StartAsync(source, Config());
            Ok(started.Result, "reentrant metadata Init");
            session = started.Session;
            using (session)
            {
                var ready = await session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                Ok(ready.Result, "metadata callback returns without self-deadlock");
                Ok(ready.Preparation.Release(), "metadata callback scope release");
            }
        }

        private static async Task Cancelled(Task task)
        {
            try { await task; throw new InvalidOperationException("Expected cancelled task."); }
            catch (OperationCanceledException) { }
        }

        private sealed class ControlContext : SynchronizationContext, IDisposable
        {
            private readonly BlockingCollection<Action> queue = new BlockingCollection<Action>();
            public override void Post(SendOrPostCallback callback, object state) => queue.Add(() => callback(state));
            internal void Run(Func<Task> operation)
            {
                SynchronizationContext previous = Current;
                SetSynchronizationContext(this);
                try
                {
                    Task task = operation();
                    while (!task.IsCompleted)
                    {
                        if (!queue.TryTake(out Action next, 30000)) throw new TimeoutException("Control-context task stalled.");
                        next();
                    }
                    task.GetAwaiter().GetResult();
                }
                finally { SetSynchronizationContext(previous); }
            }
            public void Dispose() => queue.Dispose();
        }
    }
}
