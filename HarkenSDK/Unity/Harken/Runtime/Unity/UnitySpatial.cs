using UnityEngine;

namespace Harken.Unity
{
    internal struct UnitySpatialPose
    {
        internal Vector3 Position;
        internal Quaternion Rotation;
        internal Vector3 Forward;
        internal Vector3 Up;

        internal static VnResult Capture(Transform target, out UnitySpatialPose pose)
        {
            pose = default;
            if (target == null) return VnResult.VN_INVALID_ARG;
            pose.Position = target.position;
            pose.Rotation = target.rotation;
            pose.Forward = pose.Rotation * Vector3.forward;
            pose.Up = pose.Rotation * Vector3.up;
            return UnityCoordinates.IsFinite(pose.Position.x, pose.Position.y, pose.Position.z) &&
                UnityCoordinates.IsValidDirection(pose.Forward.x, pose.Forward.y, pose.Forward.z) &&
                UnityCoordinates.IsValidDirection(pose.Up.x, pose.Up.y, pose.Up.z) &&
                Vector3.Cross(pose.Forward, pose.Up).sqrMagnitude > 0f
                ? VnResult.VN_OK : VnResult.VN_INVALID_ARG;
        }
    }

    /// <summary>
    /// Unity-facing spatial adapter. Callers pass Unity world-space values;
    /// the adapter converts them to the Harken right-handed coordinate system.
    /// </summary>
    public static class UnitySpatial
    {
        public static VnVec3 ToHarken(Vector3 value)
        {
            return UnityCoordinates.ToHarken(value.x, value.y, value.z);
        }

        public static VnResult SetListener(Transform listener)
        {
            if (listener == null)
            {
                return VnResult.VN_INVALID_ARG;
            }

            return SetListener(listener.position, listener.forward, listener.up);
        }

        public static VnResult SetListener(Vector3 position, Vector3 forward, Vector3 up)
        {
            return Listener.Set(ToHarken(position), ToHarken(forward), ToHarken(up));
        }

        public static VnResult SetEmitterTransform(Emitter emitter, Transform source)
        {
            if (source == null)
            {
                return VnResult.VN_INVALID_ARG;
            }

            return SetEmitterTransform(emitter, source.position, source.forward);
        }

        public static VnResult SetEmitterTransform(Emitter emitter, Vector3 position, Vector3 forward)
        {
            if (!UnityCoordinates.IsFinite(position.x, position.y, position.z) ||
                !UnityCoordinates.IsValidDirection(forward.x, forward.y, forward.z))
            {
                return VnResult.VN_INVALID_ARG;
            }

            VnResult result = emitter.SetPosition(ToHarken(position));
            if (result != VnResult.VN_OK)
            {
                return result;
            }

            return emitter.SetOrientation(ToHarken(forward));
        }
    }
}
