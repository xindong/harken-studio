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
            if (published && transform.position == lastPosition &&
                transform.rotation == lastRotation)
            {
                return;
            }
            LastResult = UnitySpatial.SetListener(transform);
            runtime.ObserveResult("UnitySpatial.SetListener", LastResult);
            if (LastResult == VnResult.VN_OK)
            {
                lastPosition = transform.position;
                lastRotation = transform.rotation;
                published = true;
            }
        }
    }
}
