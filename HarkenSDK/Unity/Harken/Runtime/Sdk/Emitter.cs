using System;

namespace Harken
{
    /// <summary>
    /// Ergonomic handle over an engine emitter (an SDK-facing name) — a
    /// position/orientation source many voices follow, plus a small local
    /// Variable table. It owns no engine state of its own; it is just the
    /// 64-bit handle. Every member is a single C call with zero managed
    /// allocation, so emitter updates are hot-path-safe. Position and
    /// orientation values use the Harken right-handed coordinate system:
    /// +X right, +Y up, -Z forward, with positions in meters.
    /// </summary>
    public readonly struct Emitter : IEquatable<Emitter>
    {
        public readonly ulong Handle;

        public Emitter(ulong handle)
        {
            Handle = handle;
        }

        public bool IsValid => Handle != 0;

        public static readonly Emitter None = default;

        public static VnResult Create(out Emitter emitter)
        {
            VnResult r = HarkenNative.Vn_CreateEmitter(out ulong h);
            emitter = new Emitter(h);
            return r;
        }

        public static VnResult CreateStateOnly(out Emitter emitter)
        {
            emitter = None;
            VnEmitterCreateDescV1 desc = default;
            desc.structSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<VnEmitterCreateDescV1>();
            desc.flags = VnConstants.VN_EMITTER_CREATE_STATE_ONLY;
            try
            {
                VnResult result = HarkenNative.Vn_CreateEmitterEx(in desc, out ulong handle);
                emitter = new Emitter(handle);
                return result;
            }
            catch (EntryPointNotFoundException)
            {
                // A mixed package cannot provide the required non-spatial state.
                return VnResult.VN_CAPABILITY_UNAVAILABLE;
            }
        }

        internal static VnResult CheckStateOnlySupport()
        {
            // An invalid descriptor checks the additive symbol without allocating state.
            VnEmitterCreateDescV1 desc = default;
            try
            {
                return HarkenNative.Vn_CreateEmitterEx(in desc, out _) == VnResult.VN_INVALID_ARG
                    ? VnResult.VN_OK : VnResult.VN_CAPABILITY_UNAVAILABLE;
            }
            catch (EntryPointNotFoundException) { return VnResult.VN_CAPABILITY_UNAVAILABLE; }
        }

        public VnResult Destroy() => HarkenNative.Vn_DestroyEmitter(Handle);

        public VnResult SetPosition(VnVec3 position)
            => HarkenNative.Vn_SetEmitterPosition(Handle, position);

        public VnResult SetOrientation(VnVec3 forward)
            => HarkenNative.Vn_SetEmitterOrientation(Handle, forward);

        public VnResult Stop(uint fadeMs) => HarkenNative.Vn_StopEmitter(Handle, fadeMs);

        [Obsolete("Use Values.SetValue(variable, value, emitter).")]
        internal VnResult SetValue(VnObjectId variableId, float value)
            => Values.SetValue(variableId, value, this);

        [Obsolete("Use Values.SetValue(variable, value, emitter).")]
        internal VnResult SetValue(HarkenVariableId variable, float value)
            => Values.SetValue(variable, value, this);

        [Obsolete("Use Values.SetValue(variable, value, emitter).")]
        internal VnResult SetValues(VnVariableWriteV1[] writes, int count,
            out VnVariableTicketV1 ticket)
        {
            ticket = default;
            return IsValid ? Values.DispatchRaw(Handle, false, writes, count, out ticket)
                           : VnResult.VN_INVALID_ARG;
        }

        public VnResult QueryVariable(HarkenVariableId variable,
            out VnVariableQueryResultV1 result)
        {
            result = default;
            result.structSize =
                (uint)System.Runtime.InteropServices.Marshal.SizeOf<VnVariableQueryResultV1>();
            VnResult guard = VariableCatalogGuard.QueryAndEvaluate();
            if (guard != VnResult.VN_OK)
            {
                return guard;
            }
            VnVariableQueryDescV1 desc = default;
            desc.structSize =
                (uint)System.Runtime.InteropServices.Marshal.SizeOf<VnVariableQueryDescV1>();
            desc.variableId = variable.stableId;
            desc.emitter = Handle;
            return HarkenNative.Vn_QueryVariable(in desc, ref result);
        }

        public bool Equals(Emitter other) => Handle == other.Handle;

        public override bool Equals(object obj) => obj is Emitter e && Equals(e);

        public override int GetHashCode() => Handle.GetHashCode();
    }
}
