using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace Harken
{
    public readonly struct PackStartupResult
    {
        public VnResult Result { get; }
        public PackBootstrapSession Session { get; }
        internal PackStartupResult(VnResult result, PackBootstrapSession session = null) { Result = result; Session = session; }
    }

    public readonly struct ActionPreparationResult
    {
        public VnResult Result { get; }
        public ActionPreparation Preparation { get; }
        internal ActionPreparationResult(VnResult result, ActionPreparation preparation = null)
        { Result = result; Preparation = preparation; }
    }

    public sealed partial class PackBootstrapSession
    {
        private readonly Dictionary<string, PendingCapture> companionReads = new Dictionary<string, PendingCapture>(StringComparer.Ordinal);
        private readonly AsyncLocal<bool> inCompanionReadCallback = new AsyncLocal<bool>();

        private sealed class PendingCapture
        {
            internal readonly CancellationTokenSource Cancellation = new CancellationTokenSource();
            internal readonly TaskCompletionSource<CompanionCapture> Completion =
                new TaskCompletionSource<CompanionCapture>(TaskCreationOptions.RunContinuationsAsynchronously);
            internal Task<CompanionCapture> Task => Completion.Task;
            internal int Waiters;
            internal bool Cancelled;
        }

        public async Task<ActionPreparationResult> PrepareActionsAsync(string[] names, string projectId,
            CancellationToken cancellation = default)
            => await PrepareActionsAsyncCore(names, projectId, false, cancellation);

        public async Task<ActionPreparationResult> RefreshActionAsync(string name, string projectId,
            CancellationToken cancellation = default)
            => await PrepareActionsAsyncCore(new[] { name }, projectId, true, cancellation);

        private async Task<ActionPreparationResult> PrepareActionsAsyncCore(string[] names, string projectId,
            bool refresh, CancellationToken cancellation)
        {
            cancellation.ThrowIfCancellationRequested();
            if (companionSource == null)
            {
                if (refresh) return new ActionPreparationResult(VnResult.VN_CAPABILITY_UNAVAILABLE);
                VnResult legacy = PrepareActions(names, projectId, out ActionPreparation scope);
                return new ActionPreparationResult(legacy, scope);
            }
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return new ActionPreparationResult(allowed);
            if (names == null || !Guid.TryParseExact(projectId, "D", out Guid project) || project == Guid.Empty)
                return new ActionPreparationResult(VnResult.VN_INVALID_ARG);
            if (project.ToString("D") != ProjectId) return new ActionPreparationResult(VnResult.VN_GAMEPLAY_WRONG_PROJECT);
            var unique = new HashSet<string>(StringComparer.Ordinal);
            var requested = new List<string>(names.Length);
            foreach (string name in names)
            {
                if (!PackCompanion.IsName(name) || name == "Init") return new ActionPreparationResult(VnResult.VN_INVALID_ARG);
                if (unique.Add(name)) requested.Add(name);
            }
            var preparation = new ActionPreparation(this, requested.ToArray(), refresh);
            VnResult result = await preparation.PrepareAsync(cancellation);
            return new ActionPreparationResult(result, preparation);
        }

        internal async Task<VnResult> AcquireCompanionsAsync(string[] names, SharedPack[] acquired,
            bool refresh, CancellationToken cancellation)
        {
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            cancellation.ThrowIfCancellationRequested();
            if (!companionSource.IsAsync) return AcquireCompanions(names, acquired, refresh, 0, null, cancellation);
            bool warm = !refresh;
            foreach (string name in names)
                warm &= companionNames.TryGetValue(name, out SharedPack owner) && owner.Handle.IsValid;
            if (warm) return AcquireCompanions(names, acquired, false);
            if (!PackSource.HasControlContext) return VnResult.VN_INVALID_ARG;

            var before = new Dictionary<SharedPack, (ulong Handle, PackCompanion Names)>();
            foreach (SharedPack owner in packs.Values) before[owner] = (owner.Handle.Handle, owner.Names);
            var captured = new Dictionary<string, CompanionInput>(StringComparer.Ordinal);
            var subscriptions = new List<(string Name, PendingCapture Read)>();
            try
            {
                foreach (string name in names)
                {
                    if (!refresh && companionNames.TryGetValue(name, out SharedPack owner) && owner.Handle.IsValid) continue;
                    if (!companionReads.TryGetValue(name, out PendingCapture pending))
                    {
                        pending = new PendingCapture();
                        companionReads.Add(name, pending);
                        _ = CompleteCapture(pending, name);
                    }
                    ++pending.Waiters;
                    subscriptions.Add((name, pending));
                    CompanionCapture result = await AwaitCapture(pending.Task, cancellation);
                    if (RequireOwnerThread() != VnResult.VN_OK) return VnResult.VN_INVALID_ARG;
                    if (!active) return VnResult.VN_NOT_INITIALIZED;
                    if (result.Result != VnResult.VN_OK) return result.Result;
                    captured.Add(name, result.Input);
                }
                cancellation.ThrowIfCancellationRequested();
                if (!active) return VnResult.VN_NOT_INITIALIZED;
                foreach (var pair in captured)
                {
                    CompanionInput input = pair.Value;
                    foreach (SharedPack owner in packs.Values)
                    {
                        if (owner.Names.RootId != input.Names.RootId) continue;
                        if (before.TryGetValue(owner, out var old) && old.Handle == owner.Handle.Handle &&
                            ReferenceEquals(old.Names, owner.Names)) break;
                        // Another waiter may have installed this exact captured input already.
                        if (!refresh && owner.Handle.IsValid && owner.StableId == input.Facts.packStableId &&
                            owner.ContentHash == input.Facts.contentHash && owner.ByteCount == input.Bytes.Length &&
                            owner.Names.Name == input.Names.Name && owner.Names.Language == input.Names.Language) break;
                        return VnResult.VN_PACK_STALE_REVISION;
                    }
                }
                // No await or cancellation check after native commit: a successful caller owns its lease.
                return AcquireCompanions(names, acquired, refresh, 0, captured, cancellation);
            }
            finally
            {
                // A broken host context must not mutate session state from a worker thread.
                // Its pending entries are reclaimed by owner-thread Shutdown.
                if (RequireOwnerThread() == VnResult.VN_OK)
                foreach (var subscription in subscriptions)
                {
                    PendingCapture pending = subscription.Read;
                    if (--pending.Waiters != 0) continue;
                    if (companionReads.TryGetValue(subscription.Name, out PendingCapture current) && ReferenceEquals(current, pending))
                        companionReads.Remove(subscription.Name);
                    CancelRead(pending);
                }
            }
        }

        private async Task CompleteCapture(PendingCapture pending, string name)
        {
            try
            {
                pending.Completion.TrySetResult(await companionSource.CaptureAsync(name,
                    VnObjectId.Parse(ProjectId), initBytes, pending.Cancellation.Token, SetReadCallbackPhase));
            }
            catch (OperationCanceledException) { pending.Completion.TrySetCanceled(); }
            catch (Exception error) { pending.Completion.TrySetException(error); }
        }

        private void SetReadCallbackPhase(bool reading)
        {
            if (RequireOwnerThread() != VnResult.VN_OK)
                throw new InvalidOperationException("The source must resume on the session control thread.");
            // Flow into the reader's own continuations without blocking independent subscribers.
            inCompanionReadCallback.Value = reading;
            mutationPhase = reading ? MutationPhase.ReadingContent : MutationPhase.Idle;
        }

        private static async Task<CompanionCapture> AwaitCapture(Task<CompanionCapture> task, CancellationToken cancellation)
        {
            if (!cancellation.CanBeCanceled) return await task;
            var cancelled = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            using (cancellation.Register(() => cancelled.TrySetResult(true)))
            {
                await Task.WhenAny(task, cancelled.Task);
                cancellation.ThrowIfCancellationRequested();
                return await task;
            }
        }

        private static void CancelRead(PendingCapture pending)
        {
            if (pending.Cancelled) return;
            pending.Cancelled = true;
            // Provider cancellation callbacks are foreign code and cannot undo committed state.
            try { pending.Cancellation.Cancel(); } catch { }
            _ = ObserveRead(pending);
        }

        private static async Task ObserveRead(PendingCapture pending)
        {
            try { await pending.Task; } catch { }
            finally { pending.Cancellation.Dispose(); }
        }

        private void CancelCompanionReads()
        {
            foreach (PendingCapture pending in companionReads.Values) CancelRead(pending);
            companionReads.Clear();
        }
    }
}
