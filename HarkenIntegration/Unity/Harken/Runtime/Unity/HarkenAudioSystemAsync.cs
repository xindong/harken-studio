using System;
using System.Threading;
using System.Threading.Tasks;

namespace Harken.Unity
{
    public readonly struct HarkenActionPlayResult
    {
        public VnResult Result { get; }
        public ActionFiring Firing { get; }
        internal HarkenActionPlayResult(VnResult result, ActionFiring firing = default) { Result = result; Firing = firing; }
    }

    public partial class HarkenAudioSystem
    {
        private PackSource configuredCompanionSource;
        private CancellationTokenSource companionStartup;

        public VnResult ConfigureCompanionSourceBeforeStartup(PackSource source)
        {
            if (source == null || starting || shuttingDown || (session != null && session.IsActive)) return VnResult.VN_INVALID_ARG;
            configuredCompanionSource = source;
            return VnResult.VN_OK;
        }

        public VnResult StartRuntime(PackSource source)
        {
            if (!TryBeginStartup(out VnResult rejection)) return rejection;
            try
            {
                VnResult result = CompanionStartupConfig(source, out VnConfig config);
                if (result == VnResult.VN_OK) result = PackBootstrap.Start(source, ref config, out session);
                return CompleteSessionStartup(result, source?.Root ?? string.Empty);
            }
            finally { starting = false; }
        }

        /// <summary>Completes only after Init is ready. Cancellation does not publish a failed-start event.</summary>
        public async Task<VnResult> StartRuntimeAsync(PackSource source, CancellationToken cancellation = default)
        {
            cancellation.ThrowIfCancellationRequested();
            if (!TryBeginStartup(out VnResult rejection)) return rejection;
            var request = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
            companionStartup = request;
            try
            {
                VnResult result = CompanionStartupConfig(source, out VnConfig config);
                if (result != VnResult.VN_OK) return CompleteSessionStartup(result, string.Empty);
                if (source.IsAsync && !PackSource.HasControlContext)
                    return CompleteSessionStartup(VnResult.VN_INVALID_ARG, string.Empty);
                CompanionCapture captured = await source.CaptureAsync(null, default, null, request.Token);
                request.Token.ThrowIfCancellationRequested();
                if (!this || !isActiveAndEnabled || !ReferenceEquals(companionStartup, request) ||
                    Environment.CurrentManagedThreadId != gameThreadId) return VnResult.VN_NOT_INITIALIZED;
                result = captured.Result;
                // Attach the session in the same control-thread step that initializes Native.
                if (result == VnResult.VN_OK)
                    result = PackBootstrap.StartCaptured(source, captured.Input, ref config, out session, request.Token);
                return CompleteSessionStartup(result, source.Root);
            }
            catch (OperationCanceledException)
            {
                if (this && ReferenceEquals(companionStartup, request)) State = HarkenRuntimeState.Stopped;
                throw;
            }
            finally
            {
                if (ReferenceEquals(companionStartup, request))
                {
                    if (session == null || !session.IsActive)
                    {
                        managedFirings = Array.Empty<ManagedFiring>();
                        DisposeCallbackRegistrations();
                    }
                    companionStartup = null;
                    starting = false;
                }
                request.Dispose();
            }
        }

        private VnResult CompanionStartupConfig(PackSource source, out VnConfig config)
        {
            config = default;
            if (source == null) return VnResult.VN_INVALID_ARG;
            // The explicit source selects Init; initPack only configures directory startup.
            if (startupPackStableIds != null && startupPackStableIds.Length != 0) return VnResult.VN_INVALID_ARG;
            if (!string.IsNullOrEmpty(language) && language != source.Language) return VnResult.VN_PACK_STALE_REVISION;
            return CreateStartupConfig(out config);
        }

        private async void StartConfiguredCompanion()
        {
            try { await StartRuntimeAsync(configuredCompanionSource); }
            catch (OperationCanceledException) { }
        }

        private void CancelCompanionStartup()
        {
            CancellationTokenSource pending = companionStartup;
            if (pending == null) return;
            companionStartup = null;
            starting = false;
            try { pending.Cancel(); } catch { }
        }

        /// <summary>Waits for preparation, then posts using the normal managed firing ownership.</summary>
        public Task<HarkenActionPlayResult> PlayActionAsync(string actionName, string projectId,
            CancellationToken cancellation = default)
            => PlayActionAsync(actionName, projectId, null, cancellation);

        public async Task<HarkenActionPlayResult> PlayActionAsync(string actionName, string projectId,
            HarkenSpatialSource source, CancellationToken cancellation = default)
        {
            cancellation.ThrowIfCancellationRequested();
            if (publishingSpatial) return new HarkenActionPlayResult(VnResult.VN_IN_USE);
            PackBootstrapSession owner = session;
            if (!IsReady || owner == null || !owner.IsActive || shuttingDown)
                return new HarkenActionPlayResult(VnResult.VN_NOT_INITIALIZED);
            ulong seed = managedCallerSeed++;
            ActionPreparationResult prepared = await owner.PrepareActionsAsync(new[] { actionName }, projectId, cancellation);
            HarkenActionPlayResult outcome;
            try
            {
                cancellation.ThrowIfCancellationRequested();
                if (!SpatialSessionIsCurrent(owner))
                    outcome = new HarkenActionPlayResult(VnResult.VN_NOT_INITIALIZED);
                else if (prepared.Result != VnResult.VN_OK)
                    outcome = new HarkenActionPlayResult(prepared.Result);
                else
                {
                    VnResult result = PlayActionCore(actionName, projectId, seed, source, out ActionFiring firing);
                    outcome = new HarkenActionPlayResult(result, firing);
                }
            }
            finally
            {
                if (prepared.Preparation != null && prepared.Preparation.Release() != VnResult.VN_OK && this)
                    TakeFiringCleanup(default, owner, 0, prepared.Preparation);
            }
            return SpatialSessionIsCurrent(owner)
                ? outcome : new HarkenActionPlayResult(VnResult.VN_NOT_INITIALIZED);
        }
    }
}
