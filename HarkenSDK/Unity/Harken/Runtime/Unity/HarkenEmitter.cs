using UnityEngine;
using Harken;

namespace Harken.Unity
{
    [DefaultExecutionOrder(-8000)]
    public sealed class HarkenEmitter : MonoBehaviour, IHarkenRuntimeParticipant
    {
        [SerializeField] private HarkenAudioSystem runtime;
        [SerializeField] private uint stopFadeMs = 50;

        private Emitter emitter;
        private PackBootstrapSession owningSession;
        private HarkenAudioSystem owningRuntime;
        private HarkenAudioSystem boundRuntime;
        private Vector3 lastPosition;
        private Quaternion lastRotation;
        private bool transformPublished;
        private bool runtimeBound;

        public int ShutdownOrder => 200;
        public Emitter Handle => IsCreated ? emitter : default;
        public bool IsCreated => emitter.IsValid && owningSession != null && owningSession.IsActive;
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
            if (runtime.IsReady)
            {
                Create();
            }
            return true;
        }

        public VnResult Create()
        {
            if (runtime == null || runtime.Session == null || !runtime.Session.IsActive)
                return Observe("Emitter.Create", VnResult.VN_NOT_INITIALIZED);
            if (!ReferenceEquals(owningSession, runtime.Session))
            {
                if (owningSession != null && owningSession.IsActive && emitter.IsValid)
                    return Observe("Emitter.Create", VnResult.VN_IN_USE);
                emitter = default;
                transformPublished = false;
                owningSession = runtime.Session;
                owningRuntime = runtime;
            }
            if (emitter.IsValid)
            {
                runtime.RegisterSpatialEmitter(this);
                return Observe("Emitter.Create", VnResult.VN_OK);
            }
            VnResult result = Emitter.Create(out emitter);
            if (result == VnResult.VN_OK)
            {
                runtime.RegisterSpatialEmitter(this);
            }
            return Observe("Emitter.Create", result);
        }

        public VnResult Stop()
        {
            return !IsCreated
                ? Observe("Emitter.Stop", VnResult.VN_OK)
                : Observe("Emitter.Stop", emitter.Stop(stopFadeMs));
        }

        /// <summary>Changes this identity's range without changing its physical direction.</summary>
        public VnResult SetDistanceScale(float scale)
        {
            HarkenAudioSystem owner = owningRuntime;
            if (owner != null && owner.IsPublishingSpatial)
                return LastResult = VnResult.VN_IN_USE;
            Emitter handle = emitter;
            if (owner == null || !ReferenceEquals(owner.Session, owningSession) ||
                !Matches(owner, owningSession, handle))
                return Observe("Emitter.SetDistanceScale", VnResult.VN_STALE_HANDLE);
            return Observe("Emitter.SetDistanceScale", handle.SetDistanceScale(scale));
        }

        public VnResult DestroyEmitter()
        {
            if (!IsCreated)
            {
                emitter = default;
                owningSession = null;
                owningRuntime = null;
                return Observe("Emitter.Destroy", VnResult.VN_OK);
            }
            VnResult result = emitter.Stop(stopFadeMs);
            if (result == VnResult.VN_OK)
            {
                result = emitter.Destroy();
            }
            if (result == VnResult.VN_OK)
            {
                emitter = default;
                transformPublished = false;
            }
            return Observe("Emitter.Destroy", result);
        }

        public void BeforeRuntimeShutdown()
        {
            TransferEmitterCleanup();
        }

        private void TransferEmitterCleanup()
        {
            Emitter detached = emitter;
            PackBootstrapSession session = owningSession;
            HarkenAudioSystem owner = owningRuntime;
            emitter = default;
            owningSession = null;
            owningRuntime = null;
            transformPublished = false;
            if (detached.IsValid && owner != null && session != null && session.IsActive)
                owner.TakeEmitterCleanup(detached, session, stopFadeMs);
        }

        private void HandleRuntimeReady(HarkenAudioSystem owner)
        {
            // An earlier subscriber may disable or destroy us during dispatch.
            if (!this || !isActiveAndEnabled || owner == null || !runtimeBound ||
                !ReferenceEquals(boundRuntime, owner) || !ReferenceEquals(runtime, owner) ||
                !owner.IsReady)
                return;
            owner.RegisterParticipant(this);
            Create();
        }

        internal bool Matches(HarkenAudioSystem owner, PackBootstrapSession session, Emitter handle)
            => this && isActiveAndEnabled && ReferenceEquals(owningRuntime, owner) &&
                ReferenceEquals(owningSession, session) && IsCreated && emitter.Handle == handle.Handle;

        internal VnResult PublishPose(in UnitySpatialPose pose, out bool changed)
        {
            changed = !transformPublished || pose.Position != lastPosition || pose.Rotation != lastRotation;
            if (!changed) return LastResult = VnResult.VN_OK;
            VnResult result = UnitySpatial.SetEmitterTransform(emitter, pose.Position, pose.Forward);
            if (result == VnResult.VN_OK)
            {
                lastPosition = pose.Position;
                lastRotation = pose.Rotation;
                transformPublished = true;
            }
            return LastResult = result;
        }

        internal void RecordSpatialResult(VnResult result) => LastResult = result;

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
                bound.UnregisterParticipant(this);
                bound.UnregisterSpatialEmitter(this);
            }
            TransferEmitterCleanup();
        }
    }
}
