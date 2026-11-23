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
        public ActionPreparation Preparation { get; }
        internal PackStartupResult(VnResult result, PackBootstrapSession session = null, ActionPreparation preparation = null)
        { Result = result; Session = session; Preparation = preparation; }
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
        private readonly Dictionary<string, PendingCapture> packReads = new Dictionary<string, PendingCapture>(StringComparer.Ordinal);
        private readonly AsyncLocal<bool> inPackReadCallback = new AsyncLocal<bool>();

        private CancellationTokenSource languageSwitchCancellation;
        internal bool LanguageSwitchPending => languageSwitchCancellation != null;

        /// <summary>
        /// Atomically selects a language for loaded Actions and future preparation. Existing firings
        /// retain their old content. Concurrent preparation or switching returns VN_IN_USE.
        /// Frozen catalog sources cannot select another language; directory and reader sources can.
        /// </summary>
        public async Task<VnResult> SetLanguageAsync(string language, CancellationToken cancellation = default)
        {
            cancellation.ThrowIfCancellationRequested();
            VnResult allowed = PackMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            if (!PackDirectorySource.ValidLanguage(language)) return VnResult.VN_INVALID_ARG;
            language = language ?? string.Empty;
            if (language == Language) return VnResult.VN_OK;
            if (packSource == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            if (packReads.Count != 0) return VnResult.VN_IN_USE;
            if (packSource.IsAsync && !PackSource.HasControlContext) return VnResult.VN_INVALID_ARG;
            CancellationTokenSource operation = null;
            try
            {
                PackSource source = packSource.ForLanguage(language);
                if (source == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
                operation = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
                languageSwitchCancellation = operation;
                CancellationToken operationToken = operation.Token;
                EnsureOwnerIndexes();
                var nextPacks = new Dictionary<ulong, SharedPack>(packs);
                var nextNames = new Dictionary<string, SharedPack>(metadataNames, StringComparer.Ordinal);
                var changes = new Dictionary<SharedPack, PackMetadataInput>();
                var captures = new List<PackMetadataInput>();
                SharedPack[] owners = packOwners;
                byte[] init = initBytes;
                VnObjectId project = VnObjectId.Parse(ProjectId);
                ulong retained = 0;
                foreach (SharedPack owner in owners)
                {
                    if (!owner.Handle.IsValid) continue;
                    PackMetadataCapture captured = await source.CaptureAsync(owner.Names.Name, project,
                        init, operationToken, SetReadCallbackPhase, retained);
                    if (RequireOwnerThread() != VnResult.VN_OK) return VnResult.VN_INVALID_ARG;
                    if (!active) return VnResult.VN_NOT_INITIALIZED;
                    if (captured.Result != VnResult.VN_OK) return captured.Result;
                    PackMetadataInput input = captured.Input;
                    if (input.Names.RootId != owner.Names.RootId) return VnResult.VN_PACK_STALE_REVISION;
                    captures.Add(input);
                    retained += input.RetainedResidentBytes;
                    // A missing localized variant keeps an already loaded generic Pack unchanged.
                    if (owner.Names.Language.Length == 0 && input.Names.Language.Length == 0) continue;
                    nextPacks.Remove(owner.StableId);
                    if (nextPacks.TryGetValue(input.Facts.packStableId, out SharedPack collision) &&
                        !ReferenceEquals(collision, owner)) return VnResult.VN_BAD_PACK;
                    nextPacks[input.Facts.packStableId] = owner;
                    changes.Add(owner, input);
                }
                foreach (PackMetadataInput input in captures)
                {
                    VnResult valid = source.Validate(input);
                    if (!active) return VnResult.VN_NOT_INITIALIZED;
                    if (valid != VnResult.VN_OK) return valid;
                }
                operationToken.ThrowIfCancellationRequested();
                VnResult locked = source.LockPublication(initSelection, captures, out var guards);
                if (locked != VnResult.VN_OK) return locked;
                using var held = guards;
                if (changes.Count != 0)
                {
                    VnResult result = CommitActionPacks(nextPacks, nextNames, changes);
                    if (result != VnResult.VN_OK) return result;
                }
                // Publication is irreversible for this call; cancellation after it cannot discard ownership.
                packSource = source;
                Language = language;
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            finally
            {
                // A broken host context is reclaimed by owner-thread Shutdown.
                if (RequireOwnerThread() == VnResult.VN_OK)
                {
                    if (ReferenceEquals(languageSwitchCancellation, operation))
                    {
                        languageSwitchCancellation = null;
                        mutationPhase = MutationPhase.Idle;
                    }
                    operation?.Dispose();
                }
            }
        }

        private sealed class PendingCapture
        {
            internal readonly CancellationTokenSource Cancellation = new CancellationTokenSource();
            internal readonly TaskCompletionSource<PackMetadataCapture> Completion =
                new TaskCompletionSource<PackMetadataCapture>(TaskCreationOptions.RunContinuationsAsynchronously);
            internal Task<PackMetadataCapture> Task => Completion.Task;
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
            VnResult allowed = PackMutationAllowed();
            if (allowed != VnResult.VN_OK) return new ActionPreparationResult(allowed);
            VnResult valid = ValidateRequestProject(names, projectId);
            if (valid != VnResult.VN_OK) return new ActionPreparationResult(valid);
            string[] requested = UniqueNames(names);
            if (requested == null) return new ActionPreparationResult(VnResult.VN_INVALID_ARG);
            var preparation = new ActionPreparation(this, requested, refresh);
            VnResult result = await preparation.PrepareAsync(cancellation);
            return new ActionPreparationResult(result, preparation);
        }

        internal async Task<VnResult> AcquireActionPacksAsync(string[] names, SharedPack[] acquired,
            bool refresh, CancellationToken cancellation)
        {
            VnResult allowed = PackMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            cancellation.ThrowIfCancellationRequested();
            if (!packSource.IsAsync) return AcquireActionPacks(names, acquired, refresh, 0, null, cancellation);
            bool warm = !refresh;
            foreach (string name in names)
                warm &= metadataNames.TryGetValue(name, out SharedPack owner) && owner.Handle.IsValid;
            if (warm) return AcquireActionPacks(names, acquired, false);
            if (!PackSource.HasControlContext) return VnResult.VN_INVALID_ARG;

            var before = new Dictionary<SharedPack, (ulong Handle, PackMetadata Names)>();
            foreach (SharedPack owner in packs.Values) before[owner] = (owner.Handle.Handle, owner.Names);
            var captured = new Dictionary<string, PackMetadataInput>(StringComparer.Ordinal);
            var subscriptions = new List<(string Name, PendingCapture Read)>();
            try
            {
                foreach (string name in names)
                {
                    if (!refresh && metadataNames.TryGetValue(name, out SharedPack owner) && owner.Handle.IsValid) continue;
                    if (!packReads.TryGetValue(name, out PendingCapture pending))
                    {
                        pending = new PendingCapture();
                        packReads.Add(name, pending);
                        _ = CompleteCapture(pending, name);
                    }
                    ++pending.Waiters;
                    subscriptions.Add((name, pending));
                    PackMetadataCapture result = await AwaitCapture(pending.Task, cancellation);
                    if (RequireOwnerThread() != VnResult.VN_OK) return VnResult.VN_INVALID_ARG;
                    if (!active) return VnResult.VN_NOT_INITIALIZED;
                    if (result.Result != VnResult.VN_OK) return result.Result;
                    captured.Add(name, result.Input);
                }
                cancellation.ThrowIfCancellationRequested();
                if (!active) return VnResult.VN_NOT_INITIALIZED;
                foreach (var pair in captured)
                {
                    PackMetadataInput input = pair.Value;
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
                return AcquireActionPacks(names, acquired, refresh, 0, captured, cancellation);
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
                    if (packReads.TryGetValue(subscription.Name, out PendingCapture current) && ReferenceEquals(current, pending))
                        packReads.Remove(subscription.Name);
                    CancelRead(pending);
                }
            }
        }

        private async Task CompleteCapture(PendingCapture pending, string name)
        {
            try
            {
                pending.Completion.TrySetResult(await packSource.CaptureAsync(name,
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
            inPackReadCallback.Value = reading;
            mutationPhase = reading ? MutationPhase.ReadingContent : MutationPhase.Idle;
        }

        private static async Task<PackMetadataCapture> AwaitCapture(Task<PackMetadataCapture> task, CancellationToken cancellation)
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

        private void CancelLanguageSwitch()
        {
            CancellationTokenSource pending = languageSwitchCancellation;
            languageSwitchCancellation = null;
            if (pending == null) return;
            try { pending.Cancel(); } catch { }
            pending.Dispose();
        }

        private void CancelPackReads()
        {
            foreach (PendingCapture pending in packReads.Values) CancelRead(pending);
            packReads.Clear();
        }
    }
}
