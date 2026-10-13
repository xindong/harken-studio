namespace Harken.Unity
{
    /// <summary>
    /// Converts Unity coordinates into the Harken spatial contract:
    /// right-handed, +X right, +Y up, -Z forward, with positions in meters.
    /// </summary>
    public static class UnityCoordinates
    {
        public static VnVec3 ToHarken(float x, float y, float z)
        {
            return new VnVec3
            {
                x = x,
                y = y,
                z = -z
            };
        }

        internal static bool IsFinite(float x, float y, float z)
        {
            return
                !float.IsNaN(x) && !float.IsInfinity(x) &&
                !float.IsNaN(y) && !float.IsInfinity(y) &&
                !float.IsNaN(z) && !float.IsInfinity(z);
        }

        internal static bool IsValidDirection(float x, float y, float z)
        {
            if (!IsFinite(x, y, z))
            {
                return false;
            }

            double lengthSquared =
                (double)x * x +
                (double)y * y +
                (double)z * z;
            return !double.IsInfinity(lengthSquared) &&
                   !double.IsNaN(lengthSquared) &&
                   lengthSquared > 0.0;
        }
    }
}
