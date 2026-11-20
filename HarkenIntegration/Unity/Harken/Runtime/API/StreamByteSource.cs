using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace Harken
{
    /// <summary>
    /// One immutable, positional byte-source lease. ReadAt calls may overlap.
    /// Return a VN_BYTE_READ_* status and never retain destination. Callbacks
    /// must not use Runtime or Unity main-thread APIs. After return from a Pack opener,
    /// the SDK owns Dispose; the caller must not dispose or transfer it again.
    /// Use a distinct lease object when sharing an underlying resource.
    /// </summary>
    public abstract class StreamByteSource : IDisposable
    {
        int claimed;
        internal bool TryClaim() => Interlocked.CompareExchange(ref claimed, 1, 0) == 0;
        public abstract ulong ByteLength { get; }
        public abstract int ReadAt(ulong offset, IntPtr destination, uint count,
            out uint bytesRead, StreamReadCancellation cancellation);
        public abstract void Dispose();
    }

    /// <summary>
    /// Poll on entry and around bounded waits. The native view is borrowed for
    /// one ReadAt only. Retained managed views report cancellation after return
    /// and cannot call the expired native context, even from another thread.
    /// </summary>
    public sealed class StreamReadCancellation
    {
        readonly object gate = new object();
        IntPtr context;
        VnReadIsCancelledFn query;
        internal unsafe StreamReadCancellation(IntPtr pointer)
        {
            var view = (VnReadCancellationViewV1*)pointer.ToPointer();
            if (view == null || view->structSize != sizeof(VnReadCancellationViewV1) ||
                view->reserved0 != 0 || view->isCancelled == IntPtr.Zero)
                throw new ArgumentException("Invalid cancellation view.");
            context = view->context;
            query = Marshal.GetDelegateForFunctionPointer<VnReadIsCancelledFn>(view->isCancelled);
        }
        public bool IsCancellationRequested
        {
            get { lock (gate) return query == null || query(context) != 0; }
        }
        internal void Invalidate()
        {
            lock (gate) { query = null; context = IntPtr.Zero; }
        }
    }

    // Shared lease callbacks for Pack-owned external byte sources.
    internal static class StreamByteSourceCallbacks
    {
        static readonly VnByteSourceReadAtFn ReadDelegate = ReadThunk;
        static readonly VnByteSourceReleaseFn ReleaseDelegate = ReleaseThunk;
        internal static readonly IntPtr ReadPointer = Marshal.GetFunctionPointerForDelegate(ReadDelegate);
        internal static readonly IntPtr ReleasePointer = Marshal.GetFunctionPointerForDelegate(ReleaseDelegate);

        [AOT.MonoPInvokeCallback(typeof(VnByteSourceReadAtFn))]
        static unsafe int ReadThunk(IntPtr userData, ulong offset, IntPtr destination,
            uint count, IntPtr outRead, IntPtr cancellation)
        {
            if (outRead == IntPtr.Zero) return VnConstants.VN_BYTE_READ_IO_ERROR;
            uint* written = (uint*)outRead.ToPointer();
            *written = 0;
            StreamReadCancellation view = null;
            try
            {
                var source = (StreamByteSource)GCHandle.FromIntPtr(userData).Target;
                view = new StreamReadCancellation(cancellation);
                if (view.IsCancellationRequested) return VnConstants.VN_BYTE_READ_CANCELLED;
                int result = source.ReadAt(offset, destination, count, out uint read, view);
                if (view.IsCancellationRequested) return VnConstants.VN_BYTE_READ_CANCELLED;
                if (result != VnConstants.VN_BYTE_READ_OK || read > count)
                    return result == VnConstants.VN_BYTE_READ_CANCELLED ? result : VnConstants.VN_BYTE_READ_IO_ERROR;
                *written = read;
                return result;
            }
            catch { return VnConstants.VN_BYTE_READ_IO_ERROR; }
            finally { view?.Invalidate(); }
        }

        [AOT.MonoPInvokeCallback(typeof(VnByteSourceReleaseFn))]
        static void ReleaseThunk(IntPtr userData)
        {
            var root = GCHandle.FromIntPtr(userData);
            try { ((StreamByteSource)root.Target)?.Dispose(); }
            catch { }
            finally
            {
                try { root.Free(); } catch { }
            }
        }
    }
}
