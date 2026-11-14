using System;
using System.Runtime.InteropServices;

namespace Harken
{
    /// <summary>A copied first-fault record, independent of native library lifetime.</summary>
    public readonly struct RuntimeFaultInfo
    {
        public readonly VnRuntimeFaultCategory Category;
        public readonly uint Line;
        public readonly ulong RepeatCount;
        public readonly string File;
        public readonly string Function;

        internal RuntimeFaultInfo(in VnRuntimeFaultInfoV1 native)
        {
            Category = (VnRuntimeFaultCategory)native.category;
            Line = native.line;
            RepeatCount = native.repeatCount;
            File = native.file == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(native.file);
            Function = native.function == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(native.function);
        }

        public override string ToString() => Category == VnRuntimeFaultCategory.VN_RUNTIME_FAULT_NONE
            ? "Runtime recreation required; fault details unavailable."
            : "Runtime recreation required: " + Category + " at " + File + ":" + Line +
                " in " + Function + " (repeated=" + RepeatCount + ").";
    }

    internal static class RuntimeFaultDiagnostics
    {
        private static bool reported;
        internal static event Action<RuntimeFaultInfo> Reported;
        internal static void Reset() => reported = false;

        internal static VnResult Query(out RuntimeFaultInfo info)
        {
            info = default;
            VnRuntimeFaultInfoV1 native = default;
            native.structSize = (uint)Marshal.SizeOf<VnRuntimeFaultInfoV1>();
            VnResult result;
            try { result = HarkenNative.Vn_GetRuntimeFaultInfo(ref native); }
            catch (EntryPointNotFoundException) { return VnResult.VN_CAPABILITY_UNAVAILABLE; }
            if (result == VnResult.VN_OK) info = new RuntimeFaultInfo(in native);
            return result;
        }

        internal static void ObserveResult(VnResult result)
        {
            if (result != VnResult.VN_RUNTIME_RECREATE_REQUIRED || reported) return;
            // Set before querying or invoking user code: either may reenter or end the session.
            reported = true;
            RuntimeFaultInfo info = default;
            try { Query(out info); } catch (Exception) { }
            // Optional diagnostics must never replace the original native result.
            try { Reported?.Invoke(info); } catch (Exception) { }
        }
    }
}
