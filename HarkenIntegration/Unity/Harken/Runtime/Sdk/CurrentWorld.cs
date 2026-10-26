using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Harken
{
    public readonly struct CurrentWorldLoadReport
    {
        public readonly VnCurrentWorldLoadStatus Status;
        public readonly VnResult Result;
        public readonly ulong PriorGeneration;
        public readonly ulong ResultGeneration;
        public readonly ulong RuntimeSemanticFingerprint;
        public readonly ulong CapabilityMask;
        public readonly ulong BodyBytes;
        public readonly ulong BodyChecksum;

        internal CurrentWorldLoadReport(VnCurrentWorldLoadReport report)
        {
            Status = (VnCurrentWorldLoadStatus)report.status;
            Result = (VnResult)report.result;
            PriorGeneration = report.priorGeneration;
            ResultGeneration = report.resultGeneration;
            RuntimeSemanticFingerprint = report.runtimeSemanticFingerprint;
            CapabilityMask = report.capabilityMask;
            BodyBytes = report.bodyBytes;
            BodyChecksum = report.bodyChecksum;
        }
    }

    // Studio / debug DirectWorld envelope. Games load .pack.
    public static class CurrentWorld
    {
        private const int StackPathBytes = 1024;
        private static readonly uint DescSize =
            (uint)Marshal.SizeOf<VnCurrentWorldLoadDesc>();
        private static readonly uint ReportSize =
            (uint)Marshal.SizeOf<VnCurrentWorldLoadReport>();

        public static unsafe VnResult Load(
            string path, out CurrentWorldLoadReport report)
        {
            Span<byte> stackBuffer = stackalloc byte[StackPathBytes];
            bool encodedOnStack =
                path != null && Utf8.TryEncode(path, stackBuffer, out _);
            byte[] heap = path != null && !encodedOnStack
                ? Encoding.UTF8.GetBytes(path + "\0")
                : null;

            fixed (byte* stackPointer = stackBuffer)
            fixed (byte* heapPointer = heap)
            {
                byte* pathPointer = path == null
                    ? null
                    : heap != null ? heapPointer : stackPointer;
                VnCurrentWorldLoadDesc desc = default;
                desc.structSize = DescSize;
                desc.utf8Path = (IntPtr)pathPointer;
                VnCurrentWorldLoadReport nativeReport = default;
                nativeReport.structSize = ReportSize;
                VnResult result = HarkenNative.Vn_LoadCurrentWorld(
                    desc, ref nativeReport);
                report = new CurrentWorldLoadReport(nativeReport);
                return result;
            }
        }
    }
}
