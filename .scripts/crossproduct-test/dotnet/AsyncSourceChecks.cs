using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Harken;

internal sealed class AsyncSourceChecks
{
    private readonly string root;
    private readonly string action;
    private readonly string renamed;

    private AsyncSourceChecks(string root, string action)
    {
        this.root = root;
        this.action = action;
        renamed = action + "Renamed";
    }

    internal static void Run(string root, string action)
    {
        var checks = new AsyncSourceChecks(root, action);
        using (var context = new ControlContext()) context.Run(checks.SharingCancellationAndStaleCompletion);
        using (var context = new ControlContext()) context.Run(checks.ShutdownFromReadCallback);
        using (var context = new ControlContext()) context.Run(checks.MetadataCallbackCannotAwaitItself);
    }

    private byte[] Read(string path)
    {
        string file = Path.Combine(root, path);
        return File.Exists(file) ? File.ReadAllBytes(file) : null;
    }

    private static VnConfig Config()
    {
        VnConfig config = HarkenEngine.DefaultConfig();
        config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
        return config;
    }

    private async Task SharingCancellationAndStaleCompletion()
    {
        string bytesPath = action + ".bytes";
        int actionReads = 0;
        var gates = new Queue<TaskCompletionSource<byte[]>>();
        CancellationToken providerToken = default;
        var source = PackSource.FromAsyncReader((path, token) =>
        {
            if (path == bytesPath)
            {
                ++actionReads;
                providerToken = token;
                var gate = new TaskCompletionSource<byte[]>();
                gates.Enqueue(gate);
                return gate.Task;
            }
            if (path == renamed + ".bytes") return Task.FromResult(Read(bytesPath));
            if (path == renamed + ".json") return Task.FromResult(Encoding.UTF8.GetBytes(
                Encoding.UTF8.GetString(Read(action + ".json")).Replace(action, renamed)));
            return Task.FromResult(Read(path));
        });
        PackStartupResult started = await PackBootstrap.StartAsync(source, Config());
        Ok(started.Result, "async Init");
        PackBootstrapSession session = started.Session;
        try
        {
            using (var cancel = new CancellationTokenSource())
            {
                var first = session.PrepareActionsAsync(new[] { action }, session.ProjectId, cancel.Token);
                var second = session.PrepareActionsAsync(new[] { action }, session.ProjectId);
                Check(actionReads == 1, "same-name reads are shared");
                await Task.Run(() => cancel.Cancel());
                await Cancelled(first);
                Check(!providerToken.IsCancellationRequested, "one caller cannot cancel another");
                gates.Dequeue().SetResult(Read(bytesPath));
                ActionPreparationResult ready = await second;
                Ok(ready.Result, "remaining caller prepares");
                ActionPreparationResult warm = await session.PrepareActionsAsync(new[] { action }, session.ProjectId);
                Ok(warm.Result, "warm prepare");
                Check(actionReads == 1, "warm prepare reads nothing");
                Ok(ready.Preparation.Release(), "shared caller release");
                Ok(warm.Preparation.Release(), "warm caller release");
            }

            using (var cancel = new CancellationTokenSource())
            {
                var old = session.PrepareActionsAsync(new[] { action }, session.ProjectId, cancel.Token);
                var oldGate = gates.Dequeue();
                CancellationToken oldToken = providerToken;
                cancel.Cancel();
                await Cancelled(old);
                Check(oldToken.IsCancellationRequested, "the last caller cancels the underlying read");
                var retry = session.PrepareActionsAsync(new[] { action }, session.ProjectId);
                var retryGate = gates.Dequeue();
                oldGate.SetResult(Read(bytesPath));
                await Task.Yield();
                int count = actionReads;
                var joined = session.PrepareActionsAsync(new[] { action }, session.ProjectId);
                Check(count == actionReads, "an old completion cannot remove the new pending read");
                retryGate.SetResult(Read(bytesPath));
                ActionPreparationResult a = await retry;
                ActionPreparationResult b = await joined;
                Ok(a.Result, "retry prepares");
                Ok(b.Result, "joined retry prepares");

                var stale = session.RefreshActionAsync(action, session.ProjectId);
                var staleGate = gates.Dequeue();
                ActionPreparationResult rename = await session.RefreshActionAsync(renamed, session.ProjectId);
                Ok(rename.Result, "concurrent rename");
                staleGate.SetResult(Read(bytesPath));
                Check((await stale).Result == VnResult.VN_PACK_STALE_REVISION, "an old refresh cannot undo a rename");
                Check(Actions.Resolve(action, out _) != VnResult.VN_OK, "the old name stays retired");
                Ok(Actions.Resolve(renamed, out _), "the new name survives a stale capture");
                Ok(a.Preparation.Release(), "old owner release A");
                Ok(b.Preparation.Release(), "old owner release B");
                Ok(rename.Preparation.Release(), "new owner release");
            }

            var pending = session.RefreshActionAsync(action, session.ProjectId);
            var late = gates.Dequeue();
            Ok(session.Shutdown(), "shutdown with a pending read");
            await Cancelled(pending);
            VnConfig config = Config();
            Ok(PackBootstrap.Start(PackSource.FromReader(Read), ref config, out PackBootstrapSession restarted),
                "restart while the old read is pending");
            using (restarted)
            {
                late.SetResult(Read(bytesPath));
                await Cancelled(pending);
                Check(Actions.Resolve(action, out _) != VnResult.VN_OK, "an old completion does not publish into a new session");
                Ok(restarted.PrepareActions(new[] { action }, restarted.ProjectId, out ActionPreparation scope),
                    "the new session prepares");
                Ok(scope.Release(), "new session release");
            }
        }
        finally { session.Dispose(); }
    }

    private async Task ShutdownFromReadCallback()
    {
        PackBootstrapSession session = null;
        var source = PackSource.FromAsyncReader((path, token) =>
        {
            if (path == action + ".bytes") Ok(session.Shutdown(), "shutdown inside the read callback");
            return Task.FromResult(Read(path));
        });
        PackStartupResult started = await PackBootstrap.StartAsync(source, Config());
        Ok(started.Result, "Init before callback shutdown");
        session = started.Session;
        await Cancelled(session.PrepareActionsAsync(new[] { action }, session.ProjectId));
        Check(!session.IsActive, "a shutdown inside the read callback survives completion");
        using (var cancel = new CancellationTokenSource())
        {
            var sync = PackSource.FromReader(path =>
            {
                if (path == action + ".json") cancel.Cancel();
                return Read(path);
            });
            VnConfig config = Config();
            Ok(PackBootstrap.Start(sync, ref config, out session), "Init before cancelled capture");
            using (session)
            {
                await Cancelled(session.PrepareActionsAsync(new[] { action }, session.ProjectId, cancel.Token));
                Check(Actions.Resolve(action, out _) != VnResult.VN_OK, "a capture cancelled mid-read publishes nothing");
            }
        }
    }

    private async Task MetadataCallbackCannotAwaitItself()
    {
        PackBootstrapSession session = null;
        var source = PackSource.FromAsyncReader(async (path, token) =>
        {
            if (path == action + ".bytes") await Task.Yield();
            if (path == action + ".json")
            {
                var nested = await session.PrepareActionsAsync(new[] { action }, session.ProjectId);
                Check(nested.Result == VnResult.VN_IN_USE, "a metadata callback cannot await its own pending read");
                await Task.Yield();
                nested = await session.PrepareActionsAsync(new[] { action }, session.ProjectId);
                Check(nested.Result == VnResult.VN_IN_USE, "a reader continuation cannot await its own pending read");
            }
            return Read(path);
        });
        PackStartupResult started = await PackBootstrap.StartAsync(source, Config());
        Ok(started.Result, "Init before nested prepare");
        session = started.Session;
        using (session)
        {
            ActionPreparationResult ready = await session.PrepareActionsAsync(new[] { action }, session.ProjectId);
            Ok(ready.Result, "nested prepare returns without deadlock");
            Ok(ready.Preparation.Release(), "nested prepare release");
        }
    }

    private static async Task Cancelled(Task task)
    {
        try
        {
            await task;
        }
        catch (OperationCanceledException)
        {
            return;
        }
        throw new InvalidOperationException("expected a cancelled task");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static void Ok(VnResult result, string operation)
    {
        if (result != VnResult.VN_OK) throw new InvalidOperationException(operation + ": " + result);
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
                    if (!queue.TryTake(out Action next, 30000)) throw new TimeoutException("control-context task stalled");
                    next();
                }
                task.GetAwaiter().GetResult();
            }
            finally { SetSynchronizationContext(previous); }
        }

        public void Dispose() => queue.Dispose();
    }
}
