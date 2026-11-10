using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.RegularExpressions;
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
                ShutdownReleasesSourceContent();
                using (var context = new ControlContext()) context.Run(VersionedRefresh);
                using (var context = new ControlContext()) context.Run(Asynchronous);
                using (var context = new ControlContext()) context.Run(ReentrantMetadata);
                using (var context = new ControlContext()) context.Run(LanguageSwitch);
                using (var context = new ControlContext()) context.Run(LanguageSwitchBatchAndReentrancy);
                ReentrantShutdown();
                Console.WriteLine("[Companion sources] Reader snapshots, async sharing, cancellation, retry, rename and restart isolation PASS.");
                return true;
            }
            catch (Exception error) { Console.WriteLine("[Companion sources] FAIL: " + error); return false; }
        }

        private sealed class RetainedReader
        {
            internal readonly byte[] Repository = new byte[8 * 1024 * 1024];
            internal byte[] ReadFile(string path) => Read(path);
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static (PackBootstrapSession Session, ActionPreparation Scope,
            WeakReference Source, WeakReference Init, int InitLength) StartRetainedSource()
        {
            var reader = new RetainedReader();
            var config = Config();
            Ok(PackBootstrap.Start(PackSource.FromReader(reader.ReadFile, streamOpen: (path, revision, bytes, cancellation) =>
                { GC.KeepAlive(reader); return null; }), ref config,
                out PackBootstrapSession session), "retained reader start");
            Ok(session.PrepareActions(new[] { "Play_Head" }, session.ProjectId,
                out ActionPreparation scope), "retained scope");
            var snapshot = (byte[])typeof(PackBootstrapSession).GetField("initBytes",
                BindingFlags.Instance | BindingFlags.NonPublic).GetValue(session);
            return (session, scope, new WeakReference(reader), new WeakReference(snapshot), snapshot.Length);
        }

        private static void CollectSourceContent()
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
        }

        private static void ShutdownReleasesSourceContent()
        {
            var retained = StartRetainedSource();
            try
            {
                CollectSourceContent();
                Check(retained.Source.IsAlive && retained.Init.IsAlive, "active session owns source content");
                Ok(retained.Session.Shutdown(), "retained session shutdown");
                CollectSourceContent();
                Check(!retained.Source.IsAlive && !retained.Init.IsAlive, "closed session releases source content");
                Check(retained.Session.RefreshAction("Play_Head", retained.Session.ProjectId, out _) ==
                    VnResult.VN_NOT_INITIALIZED, "closed refresh preserves result");
                Check(retained.Scope.Prepare() == VnResult.VN_NOT_INITIALIZED, "closed scope cannot reacquire");
                Ok(retained.Scope.Release(), "closed scope release");
                Console.WriteLine($"[Companion retention] Released source payload={8 * 1024 * 1024} Init={retained.InitLength} bytes with old session/scope alive.");
                GC.KeepAlive(retained.Session);
                GC.KeepAlive(retained.Scope);
            }
            finally { retained.Session.Dispose(); }
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
                if (path != invalid) return bytes;
                string json = Encoding.UTF8.GetString(bytes);
                var parser = new JsonParser(json, "schemaVersion");
                var root = parser.Read(0) as Dictionary<string, object>;
                Check(parser.End && root != null && root.TryGetValue("schemaVersion", out object version) &&
                    version is JsonParser.NumberToken, "version injection requires a top-level numeric schemaVersion");
                Check(((JsonParser.NumberToken)root["schemaVersion"]).Text != "0",
                    "version injection requires a different source version");
                foreach (Match match in Regex.Matches(json,
                    "\"schemaVersion\"\\s*:\\s*(?<value>-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)"))
                {
                    Group value = match.Groups["value"];
                    string changed = json.Remove(value.Index, value.Length).Insert(value.Index, "0");
                    var changedParser = new JsonParser(changed, "schemaVersion");
                    var changedRoot = (Dictionary<string, object>)changedParser.Read(0);
                    if (changedParser.End && changedRoot["schemaVersion"] is JsonParser.NumberToken changedVersion && changedVersion.Text == "0")
                    {
                        Check(changed != json, "version injection must change the companion");
                        return Encoding.UTF8.GetBytes(changed);
                    }
                }
                throw new InvalidOperationException("version injection did not replace the top-level schemaVersion");
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

        private static async Task LanguageSwitch()
        {
            foreach (int mode in new[] { 0, 1, 2 })
            {
                PackSource source = mode == 0 ? PackSource.FromDirectory(Root) : mode == 1
                    ? PackSource.FromReader(Read, streamOpen: (path, revision, length, cancel) => null)
                    : PackSource.FromAsyncReader(async (path, token) => { await Task.Yield(); return Read(path); });
                PackStartupResult start = await PackBootstrap.StartAsync(source, Config());
                Ok(start.Result, "language Init");
                using (PackBootstrapSession session = start.Session)
                {
                    ActionPreparationResult prepared = await session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                    Ok(prepared.Result, "language preparation");
                    ActionPreparation scope = prepared.Preparation;
                    Ok(Actions.Resolve("Play_Head", out ActionResolved before), "language root");
                    Ok(Actions.Post(in before, Emitter.None, 71, out ActionFiring old), "old language firing");
                    Ok(source.IsAsync ? VnResult.VN_OK : source.Capture("Play_Head", VnObjectId.Parse(session.ProjectId),
                        Read("Init.bytes"), out _), "source remains readable");
                    var general = await source.CaptureAsync("Play_Head", VnObjectId.Parse(session.ProjectId), Read("Init.bytes"), default);
                    Ok(session.AcquirePack(general.Input.Facts.packStableId, out Pack raw), "old language raw lease");
                    ulong revision = session.SetRevision;
                    Ok(await session.SetLanguageAsync("English"), "select English");
                    Check(session.Language == "English" && session.SetRevision != revision && scope.IsPrepared,
                        "selection commits without invalidating preparation");
                    Ok(old.Query(out _), "old language firing survives");
                    var english = await source.ForLanguage("English").CaptureAsync("Play_Head", VnObjectId.Parse(session.ProjectId), Read("Init.bytes"), default);
                    if (mode == 1)
                    {
                        Check(!PackStreamBinding.Same(general.Input.Streams, english.Input.Streams), "localized stream scope changes");
                        var generic = await source.ForLanguage("French").CaptureAsync("Play_Head", VnObjectId.Parse(session.ProjectId), Read("Init.bytes"), default);
                        Check(PackStreamBinding.Same(general.Input.Streams, generic.Input.Streams), "generic fallback retains stream provider identity");
                    }
                    Ok(session.AcquirePack(english.Input.Facts.packStableId, out Pack localized), "new language Pack");
                    Ok(session.ReleasePack(english.Input.Facts.packStableId, localized), "localized lease release");
                    Ok(session.ReleasePack(general.Input.Facts.packStableId, raw), "old raw lease follows owner");
                    Ok(Actions.Resolve("Play_Head", out ActionResolved after), "new language root");
                    Ok(Actions.Post(in after, Emitter.None, 72, out _), "new language firing");
                    Ok(await session.SetLanguageAsync("French"), "missing language falls back");
                    Ok(session.AcquirePack(general.Input.Facts.packStableId, out Pack fallback), "fallback is General");
                    revision = session.SetRevision;
                    Ok(await session.SetLanguageAsync("German"), "generic remains installed");
                    Check(session.SetRevision == revision, "generic selection does not replace content");
                    Ok(session.ReleasePack(general.Input.Facts.packStableId, fallback), "fallback raw lease release");
                    Ok(scope.Release(), "language scope release");
                    Ok(await session.SetLanguageAsync("English"), "select with unloaded owner");
                    Ok(await scope.PrepareAsync(), "released scope prepares new language");
                    Ok(scope.Release(), "reprepared scope release");
                    Check(await session.SetLanguageAsync("../English") == VnResult.VN_INVALID_ARG, "invalid language refused");
                }
            }

            bool block = false, corrupt = false, mutate = false;
            ulong version = 1;
            var gates = new Queue<TaskCompletionSource<byte[]>>();
            CancellationToken provider = default;
            var asynchronous = PackSource.FromAsyncReader((path, token) =>
            {
                if (path == "English/Play_Head.bytes" && block)
                {
                    provider = token;
                    var gate = new TaskCompletionSource<byte[]>();
                    gates.Enqueue(gate);
                    return gate.Task;
                }
                if (path == "English/Play_Head.json")
                {
                    if (mutate) ++version;
                    if (corrupt) return Task.FromResult(Encoding.UTF8.GetBytes("{"));
                }
                return Task.FromResult(Read(path));
            }, revision: () => version);
            PackStartupResult started = await PackBootstrap.StartAsync(asynchronous, Config());
            Ok(started.Result, "cancelable language Init");
            using (PackBootstrapSession session = started.Session)
            {
                var prepared = await session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                Ok(prepared.Result, "cancelable language scope");
                ulong revision = session.SetRevision;
                corrupt = true;
                Check(await session.SetLanguageAsync("English") == VnResult.VN_BAD_PACK, "corrupt variant refused");
                corrupt = false; mutate = true;
                Check(await session.SetLanguageAsync("English") == VnResult.VN_PACK_STALE_REVISION, "changing publication refused");
                mutate = false;
                Check(session.Language == string.Empty && session.SetRevision == revision && prepared.Preparation.IsPrepared,
                    "failures preserve language, publication and preparation");
                block = true;
                using (var cancellation = new CancellationTokenSource())
                {
                    Task<VnResult> switching = session.SetLanguageAsync("English", cancellation.Token);
                    Check(!switching.IsCompleted, "switch waits for content");
                    Check(prepared.Preparation.Release() == VnResult.VN_IN_USE && prepared.Preparation.IsPrepared,
                        "rejected release preserves preparation");
                    Check(await session.SetLanguageAsync("French") == VnResult.VN_IN_USE, "parallel switch refused");
                    Check((await session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId)).Result == VnResult.VN_IN_USE,
                        "parallel preparation refused");
                    cancellation.Cancel();
                    await Cancelled(switching);
                    Check(provider.IsCancellationRequested && session.Language == string.Empty && session.SetRevision == revision,
                        "cancellation leaves old publication");
                    block = false;
                    Ok(await session.SetLanguageAsync("English"), "new switch after cancellation");
                    gates.Dequeue().SetResult(Read("English/Play_Head.bytes"));
                    await Task.Yield();
                    Check(session.Language == "English", "late read cannot overwrite selection");
                }
                Ok(await session.SetLanguageAsync(string.Empty), "return to General");
                block = true;
                Task<VnResult> shutdownRead = session.SetLanguageAsync("English");
                Ok(session.Shutdown(), "shutdown during switch");
                await Cancelled(shutdownRead);
                Check(provider.IsCancellationRequested, "shutdown cancels provider");
                gates.Dequeue().SetResult(Read("English/Play_Head.bytes"));
                await Task.Yield();
            }
            Console.WriteLine("[Language switch] Directory/readers, fallback, retained leases, rollback, cancellation and shutdown PASS.");
        }

        private static async Task LanguageSwitchBatchAndReentrancy()
        {
            string directory = Path.Combine(AppContext.BaseDirectory, "NativePublications", "UnityFixture", "U4Original");
            bool corrupt = false;
            var captures = new List<string>();
            PackSource source = PackSource.FromReader(path =>
            {
                if (path.StartsWith("French/", StringComparison.Ordinal)) return null;
                string file = Path.Combine(directory, path);
                if (path != "Init.json" && path.EndsWith(".json", StringComparison.Ordinal))
                {
                    captures.Add(path);
                    if (corrupt && captures.Count == 2) return Encoding.UTF8.GetBytes("{");
                }
                return File.Exists(file) ? File.ReadAllBytes(file) : null;
            });
            var config = Config();
            Ok(PackBootstrap.Start(source, ref config, out PackBootstrapSession session), "batch language Init");
            using (session)
            {
                Ok(session.PrepareActions(new[] { "Play_U4_A", "Play_U4_B" }, session.ProjectId,
                    out ActionPreparation ready), "batch language prepare");
                Ok(Actions.Resolve("Play_U4_A", out ActionResolved a), "batch A");
                Ok(Actions.Resolve("Play_U4_B", out ActionResolved b), "batch B");
                ulong revision = session.SetRevision;
                corrupt = true; captures.Clear();
                Check(await session.SetLanguageAsync("French") == VnResult.VN_BAD_PACK, "second Action failure refuses whole selection");
                Check(captures.Count == 2 && session.SetRevision == revision && session.Language == string.Empty && ready.IsPrepared,
                    "batch rollback preserves both owners and selected language");
                Ok(Actions.Post(in a, Emitter.None, 81, out _), "old A token remains valid");
                Ok(Actions.Post(in b, Emitter.None, 82, out _), "old B token remains valid");
                corrupt = false;
                Ok(await session.SetLanguageAsync("French"), "batch fallback retry");
                Check(session.SetRevision == revision, "all-generic switch keeps content");
                Ok(ready.Release(), "batch language release");
            }

            session = null;
            var reentrant = PackSource.FromAsyncReader((path, token) =>
            {
                if (path == "English/Play_Head.bytes")
                {
                    token.Register(() => throw new InvalidOperationException("Foreign cancellation callback."));
                    Ok(session.Shutdown(), "reader can shut down pending switch");
                }
                return Task.FromResult(Read(path));
            });
            var start = await PackBootstrap.StartAsync(reentrant, Config());
            Ok(start.Result, "reentrant switch Init");
            using (session = start.Session)
            {
                var ready = await session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId);
                Ok(ready.Result, "reentrant switch prepare");
                await Cancelled(session.SetLanguageAsync("English"));
                Check(!session.IsActive, "reentrant shutdown remains final despite foreign callback failure");
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
