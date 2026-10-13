using System;

namespace Harken
{
    /// <summary>
    /// Ergonomic handle over a playing voice. Owns no engine state; just the
    /// generational 64-bit handle. Every setter is a single C call with zero
    /// managed allocation (per-frame hot path).
    /// </summary>
    public readonly struct Voice : IEquatable<Voice>
    {
        public readonly ulong Handle;

        public Voice(ulong handle)
        {
            Handle = handle;
        }

        public bool IsValid => Handle != 0;

        public VnResult SetVolume(float volumeDb, uint rampMs)
            => HarkenNative.Vn_SetVoiceVolume(Handle, volumeDb, rampMs);

        public VnResult SetFilter(uint filterKind, float cutoffHz, float q,
            uint enable01, uint rampMs)
            => HarkenNative.Vn_SetVoiceFilter(Handle, filterKind, cutoffHz, q,
                enable01, rampMs);

        public VnResult SetPitch(float ratio) => HarkenNative.Vn_SetVoicePitch(Handle, ratio);

        public VnResult SetPosition(VnVec3 position)
            => HarkenNative.Vn_SetVoicePosition(Handle, position);

        public VnResult SetOrientation(VnVec3 forward)
            => HarkenNative.Vn_SetVoiceOrientation(Handle, forward);

        public VnResult SetPan(float pan)
            => HarkenNative.Vn_SetVoicePan(Handle, pan);

        public VnResult SetWidth(float width)
            => HarkenNative.Vn_SetVoiceWidth(Handle, width);

        public VnResult SetSpatializationMix(float mix)
            => HarkenNative.Vn_SetVoiceSpatializationMix(Handle, mix);

        public VnResult SetDistanceScale(float scale)
            => HarkenNative.Vn_SetVoiceDistanceScale(Handle, scale);

        public VnResult SetOutputVolume(float volumeDb, uint rampMs)
            => HarkenNative.Vn_SetVoiceOutputVolume(Handle, volumeDb, rampMs);

        public VnResult SetSend(uint sendSlot, VnObjectId toBus, float levelDb)
            => HarkenNative.Vn_SetVoiceSend(Handle, sendSlot, toBus, levelDb);

        public VnResult SetSendLevel(uint sendSlot, float levelDb, uint rampMs)
            => HarkenNative.Vn_SetVoiceSendLevel(Handle, sendSlot, levelDb, rampMs);

        public VnResult ClearSend(uint sendSlot)
            => HarkenNative.Vn_ClearVoiceSend(Handle, sendSlot);

        public VnResult Stop(uint fadeMs) => HarkenNative.Vn_Stop(Handle, fadeMs);

        public bool Equals(Voice other) => Handle == other.Handle;

        public override bool Equals(object obj) => obj is Voice v && Equals(v);

        public override int GetHashCode() => Handle.GetHashCode();
    }

    public static class Playback
    {
        /// <summary>
        /// Stops every Voice and Action firing after one atomic capacity
        /// preflight. Registered content and authored state remain loaded.
        /// </summary>
        public static VnResult StopAll(uint fadeMs)
            => HarkenNative.Vn_StopAll(fadeMs);
    }
}
