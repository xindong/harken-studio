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
            if (!TryBindRuntime())
            {
                return;
            }
            if (!IsCreated || owningRuntime == null || !owningRuntime.IsReady)
            {
                return;
            }
            PublishTransformIfChanged();
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
                return Observe("Emitter.Create", VnResult.VN_OK);
            }
            VnResult result = Emitter.Create(out emitter);
            if (result == VnResult.VN_OK)
            {
                result = UnitySpatial.SetEmitterTransform(emitter, transform);
                if (result == VnResult.VN_OK)
                {
                    lastPosition = transform.position;
                    lastRotation = transform.rotation;
                    transformPublished = true;
                }
            }
            return Observe("Emitter.Create", result);
        }

        public VnResult Stop()
        {
            return !IsCreated
                ? Observe("Emitter.Stop", VnResult.VN_OK)
                : Observe("Emitter.Stop", emitter.Stop(stopFadeMs));
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

        private void PublishTransformIfChanged()
        {
            Vector3 position = transform.position;
            Quaternion rotation = transform.rotation;
            if (transformPublished && position == lastPosition && rotation == lastRotation)
            {
                return;
            }
            VnResult result = UnitySpatial.SetEmitterTransform(emitter, position, rotation * Vector3.forward);
            if (result == VnResult.VN_OK)
            {
                lastPosition = position;
                lastRotation = rotation;
                transformPublished = true;
            }
            Observe("UnitySpatial.SetEmitterTransform", result);
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
                bound.UnregisterParticipant(this);
            }
            TransferEmitterCleanup();
        }
    }
}
