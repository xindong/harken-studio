namespace Harken
{
    /// <summary>
    /// The listener (an SDK-facing name). A managed convenience over
    /// <c>Vn_SetListener</c> with no engine state of its own. Hot-path-safe:
    /// blittable value arguments, zero managed allocation. Values use the
    /// Harken right-handed coordinate system: +X right, +Y up, -Z forward,
    /// with positions in meters. Unity callers should use UnitySpatial.
    /// </summary>
    public static class Listener
    {
        public static VnResult Set(VnVec3 position, VnVec3 forward, VnVec3 up)
            => HarkenNative.Vn_SetListener(position, forward, up);

        /// <summary>Uses a live emitter for distance only; None restores the listener position.</summary>
        public static VnResult SetDistanceReference(Emitter emitter)
            => HarkenNative.Vn_SetListenerDistanceReference(emitter.Handle);
    }
}
