using UnityEngine;

namespace Harken.Unity
{
    [DefaultExecutionOrder(-7000)]
    public sealed class HarkenListener : MonoBehaviour
    {
        [SerializeField] private HarkenAudioSystem runtime;
        private HarkenAudioSystem boundRuntime;
        private PackBootstrapSession registeredSession;
        private Vector3 lastPosition;
        private Quaternion lastRotation;
        private bool published;

        public VnResult LastResult { get; private set; } = VnResult.VN_OK;

        private void OnEnable() => TryBindRuntime();
        private void Update() => TryBindRuntime();

        private void TryBindRuntime()
        {
            if (boundRuntime != null) return;
            runtime = runtime != null ? runtime : HarkenAudioSystem.Instance;
            if (runtime == null) return;
            boundRuntime = runtime;
            runtime.BecameReady += HandleRuntimeReady;
            if (runtime.IsReady) HandleRuntimeReady(runtime);
        }

        private void HandleRuntimeReady(HarkenAudioSystem owner)
        {
            if (!this || !isActiveAndEnabled || owner == null || !owner.IsReady ||
                !ReferenceEquals(boundRuntime, owner) || !ReferenceEquals(runtime, owner)) return;
            if (ReferenceEquals(registeredSession, owner.Session)) return;
            registeredSession = owner.Session;
            published = false;
            owner.RegisterListener(this);
        }

        internal bool Matches(HarkenAudioSystem owner, PackBootstrapSession session)
            => this && isActiveAndEnabled && ReferenceEquals(boundRuntime, owner) &&
                ReferenceEquals(registeredSession, session) && session != null && session.IsActive;

        internal VnResult PublishPose(in UnitySpatialPose pose, bool force, out bool changed)
        {
            changed = force || !published || pose.Position != lastPosition || pose.Rotation != lastRotation;
            if (!changed) return LastResult = VnResult.VN_OK;
            VnResult result = UnitySpatial.SetListener(pose.Position, pose.Forward, pose.Up);
            if (result == VnResult.VN_OK)
            {
                lastPosition = pose.Position;
                lastRotation = pose.Rotation;
                published = true;
            }
            return LastResult = result;
        }

        internal void RecordSpatialResult(VnResult result) => LastResult = result;

        private void OnDisable()
        {
            HarkenAudioSystem bound = boundRuntime;
            boundRuntime = null;
            registeredSession = null;
            published = false;
            if (bound == null) return;
            bound.BecameReady -= HandleRuntimeReady;
            bound.UnregisterListener(this);
        }
    }
}
