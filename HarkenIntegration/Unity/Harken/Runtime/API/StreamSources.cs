using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Text;

namespace Harken
{
    /// <summary>
    /// One immutable, positional byte-source lease. ReadAt calls may overlap.
    /// Return a VN_BYTE_READ_* status and never retain destination. Callbacks
    /// must not use Runtime or Unity main-thread APIs. After accepted Begin or return from a Pack opener,
    /// native owns Dispose; the caller must not dispose or transfer it again.
    /// A refused Begin preserves caller ownership and allows another attempt.
    /// Use a distinct lease object when sharing an underlying resource.
    /// </summary>
    public abstract class StreamByteSource : IDisposable
    {
        int claimed;
        internal bool TryClaim() => Interlocked.CompareExchange(ref claimed, 1, 0) == 0;
        internal void ReturnRejectedLease() => Interlocked.Exchange(ref claimed, 0);
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

    /// <summary>
    /// Control-thread request value. Query once per normal engine tick; Ready
    /// is validated but unpublished. Commit consumes success. Dispose/Cancel
    /// abandons the request, never frees a provider root or unregisters an
    /// already committed source. Copies and old-engine values become stale.
    /// </summary>
    public readonly struct StreamSourceRequest : IDisposable
    {
        public readonly ulong Handle;
        internal StreamSourceRequest(ulong handle) { Handle = handle; }
        public bool IsValid => Handle != 0;
        public VnResult Query(out VnStreamSourceInfoV1 info)
        {
            info = default;
            info.structSize = (uint)Marshal.SizeOf<VnStreamSourceInfoV1>();
            return HarkenNative.Vn_QueryStreamSource(Handle, ref info);
        }
        public VnResult Commit() => HarkenNative.Vn_CommitStreamSource(Handle);
        public VnResult Cancel() => HarkenNative.Vn_CancelStreamSource(Handle);
        public void Dispose() { Cancel(); }
    }

    /// <summary>
    /// Nonblocking engine-thread source registration. Continue normal Update,
    /// Query each tick, then Commit Ready or Cancel an abandoned/failed request.
    /// Accepted providers remain rooted until native Release after all reads
    /// drain. Dispose can run on the native cleanup thread; Term waits for it.
    /// Provider waits must be bounded and poll their per-call cancellation.
    /// </summary>
    public static class StreamSources
    {
        static readonly VnByteSourceReadAtFn ReadDelegate = ReadThunk;
        static readonly VnByteSourceReleaseFn ReleaseDelegate = ReleaseThunk;
        internal static readonly IntPtr ReadPointer = Marshal.GetFunctionPointerForDelegate(ReadDelegate);
        internal static readonly IntPtr ReleasePointer = Marshal.GetFunctionPointerForDelegate(ReleaseDelegate);

        public static VnResult BeginRegister(VnObjectId soundId, string path, out StreamSourceRequest request)
            => BeginFile(soundId, VnConstants.VN_STREAM_SOURCE_REGISTER, path, out request);
        public static VnResult BeginReplace(VnObjectId soundId, string path, out StreamSourceRequest request)
            => BeginFile(soundId, VnConstants.VN_STREAM_SOURCE_REPLACE, path, out request);
        public static VnResult BeginRegister(VnObjectId soundId, StreamByteSource source, out StreamSourceRequest request)
            => BeginSource(soundId, VnConstants.VN_STREAM_SOURCE_REGISTER, source, out request);
        public static VnResult BeginReplace(VnObjectId soundId, StreamByteSource source, out StreamSourceRequest request)
            => BeginSource(soundId, VnConstants.VN_STREAM_SOURCE_REPLACE, source, out request);
        public static VnResult Unregister(VnObjectId soundId) => HarkenNative.Vn_UnregisterStreamFile(soundId);

        static unsafe VnResult BeginFile(VnObjectId id, uint mode, string path, out StreamSourceRequest request)
        {
            Span<byte> stack = stackalloc byte[1024];
            byte[] heap = path != null && !Utf8.TryEncode(path, stack, out _)
                ? Encoding.UTF8.GetBytes(path + "\0") : null;
            fixed (byte* local = stack)
            fixed (byte* allocated = heap)
            {
                byte* pointer = path == null ? null : heap != null ? allocated : local;
                var result = HarkenNative.Vn_BeginStreamFile(id, mode, pointer, out ulong handle);
                request = result == VnResult.VN_OK ? new StreamSourceRequest(handle) : default;
                return result;
            }
        }
        static VnResult BeginSource(VnObjectId id, uint mode, StreamByteSource source, out StreamSourceRequest request)
        {
            request = default;
            if (source == null || !source.TryClaim()) return VnResult.VN_INVALID_ARG;
            GCHandle root = default;
            bool accepted = false;
            try
            {
                ulong length = source.ByteLength;
                root = GCHandle.Alloc(source);
                VnByteSourceDescV1 desc = default;
                desc.structSize = (uint)Marshal.SizeOf<VnByteSourceDescV1>();
                desc.byteLength = length;
                desc.userData = GCHandle.ToIntPtr(root);
                desc.readAt = ReadPointer;
                desc.release = ReleasePointer;
                VnResult result = HarkenNative.Vn_BeginStreamSource(id, mode, desc, out ulong handle);
                accepted = result == VnResult.VN_OK;
                if (accepted) request = new StreamSourceRequest(handle);
                return result;
            }
            catch { return VnResult.VN_DEVICE_ERROR; }
            finally
            {
                // Native may already have released an accepted root. Never
                // inspect/free it here after success, even if callbacks raced.
                if (!accepted)
                {
                    if (root.IsAllocated) root.Free();
                    source.ReturnRejectedLease();
                }
            }
        }

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
