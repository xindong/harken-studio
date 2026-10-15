using UnityEngine;

namespace Harken.Unity
{
    [DefaultExecutionOrder(-7000)]
    public sealed class HarkenListener : MonoBehaviour
    {
        [SerializeField] private HarkenAudioSystem runtime;

        private Vector3 lastPosition;
        private Quaternion lastRotation;
        private bool published;
        private PackBootstrapSession publishedSession;

        public VnResult LastResult { get; private set; } = VnResult.VN_OK;

        private void OnEnable()
        {
            published = false;
        }

        private void LateUpdate()
        {
            runtime = runtime != null ? runtime : HarkenAudioSystem.Instance;
            if (runtime == null || !runtime.IsReady)
            {
                return;
            }
            if (!ReferenceEquals(publishedSession, runtime.Session))
            {
                published = false;
                publishedSession = runtime.Session;
            }
            Vector3 position = transform.position;
            Quaternion rotation = transform.rotation;
            if (published && position == lastPosition && rotation == lastRotation)
            {
                return;
            }
            VnResult result = UnitySpatial.SetListener(position, rotation * Vector3.forward, rotation * Vector3.up);
            if (result == VnResult.VN_OK)
            {
                lastPosition = position;
                lastRotation = rotation;
                published = true;
            }
            LastResult = result;
            runtime.ObserveResult("UnitySpatial.SetListener", result);
        }
    }
}
