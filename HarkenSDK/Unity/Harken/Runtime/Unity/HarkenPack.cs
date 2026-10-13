using UnityEngine;

namespace Harken.Unity
{
    [DefaultExecutionOrder(-9000)]
    public sealed class HarkenPack : MonoBehaviour, IHarkenRuntimeParticipant
    {
        [SerializeField] private HarkenAudioSystem runtime;
        [SerializeField] private ulong packStableId;
        [SerializeField] private bool loadOnReady = true;

        private Pack pack;
        private PackBootstrapSession owningSession;
        private HarkenAudioSystem owningRuntime;
        private HarkenAudioSystem boundRuntime;
        private ulong loadedStableId;
        private bool runtimeBound;

        public int ShutdownOrder => 100;
        public ulong PackStableId => packStableId;
        public Pack Handle => IsLoaded ? pack : default;
        public bool IsLoaded => pack.IsValid && owningSession != null && owningSession.IsActive;
        public bool LoadOnReady => loadOnReady;
        public VnResult LastResult { get; private set; } = VnResult.VN_OK;

        private void OnEnable()
        {
            TryBindRuntime();
        }

        private void Update()
        {
            TryBindRuntime();
        }

        private bool TryBindRuntime()
        {
            if (runtimeBound && boundRuntime != null)
            {
                return true;
            }
            runtimeBound = false;
            runtime = runtime != null ? runtime : HarkenAudioSystem.Instance;
            if (runtime == null)
            {
                LastResult = VnResult.VN_NOT_INITIALIZED;
                return false;
            }
            runtime.RegisterParticipant(this);
            runtime.BecameReady += HandleRuntimeReady;
            boundRuntime = runtime;
            runtimeBound = true;
            if (runtime.IsReady && loadOnReady)
            {
                runtime.QueuePackLoad(this);
            }
            return true;
        }

        public VnResult Load()
        {
            if (IsLoaded)
            {
                return Observe("HarkenPack.Load", loadedStableId == packStableId &&
                    runtime != null && ReferenceEquals(owningSession, runtime.Session)
                    ? VnResult.VN_OK : VnResult.VN_IN_USE);
            }
            if (runtime == null || !runtime.IsReady || packStableId == 0)
            {
                return Observe("HarkenPack.Load", VnResult.VN_INVALID_ARG);
            }
            if (runtime.Session == null || !runtime.Session.IsActive)
                return Observe("HarkenPack.Load", VnResult.VN_NOT_INITIALIZED);
            VnResult result = runtime.Session.AcquirePack(packStableId, out pack);
            if (result == VnResult.VN_OK)
            {
                owningSession = runtime.Session;
                owningRuntime = runtime;
                loadedStableId = packStableId;
            }
            return Observe("Packs.LoadPack(" + packStableId + ")", result);
        }

        public VnResult Unload()
        {
            if (!IsLoaded)
            {
                ForgetLoad();
                return Observe("HarkenPack.Unload", VnResult.VN_OK);
            }
            HarkenAudioSystem owner = owningRuntime;
            ulong stableId = loadedStableId;
            VnResult result = owningSession.ReleasePack(loadedStableId, pack);
            if (result == VnResult.VN_OK)
                ForgetLoad();
            LastResult = result;
            return owner != null ? owner.ObserveResult("Packs.UnloadPack(" + stableId + ")", result) : result;
        }

        public void BeforeRuntimeShutdown()
        {
            boundRuntime?.CancelPackLoad(this);
        }

        private void HandleRuntimeReady(HarkenAudioSystem owner)
        {
            owner.RegisterParticipant(this);
            if (loadOnReady)
            {
                owner.QueuePackLoad(this);
            }
        }

        private VnResult Observe(string operation, VnResult result)
        {
            LastResult = result;
            return runtime != null
                ? runtime.ObserveResult(operation, result)
                : result;
        }

        private void OnDisable()
        {
            HarkenAudioSystem bound = boundRuntime;
            boundRuntime = null;
            runtimeBound = false;
            if (bound != null)
            {
                bound.BecameReady -= HandleRuntimeReady;
                bound.CancelPackLoad(this);
                bound.UnregisterParticipant(this);
            }

            Pack detached = pack;
            PackBootstrapSession session = owningSession;
            HarkenAudioSystem owner = owningRuntime;
            ulong stableId = loadedStableId;
            ForgetLoad();
            if (!detached.IsValid || session == null || !session.IsActive) return;

            VnResult result = session.ReleasePack(stableId, detached);
            // Transfer a refused request before listeners can re-enable this component or restart its session.
            if (result != VnResult.VN_OK && owner != null)
                owner.TakePackCleanup(detached, session, stableId, result);
            LastResult = result;
            if (owner != null) owner.ObserveResult("Packs.UnloadPack(" + stableId + ")", result);
        }

        private void ForgetLoad()
        {
            pack = default;
            owningSession = null;
            owningRuntime = null;
            loadedStableId = 0;
        }
    }
}
