using UnityEngine;

namespace Harken.Unity
{
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
