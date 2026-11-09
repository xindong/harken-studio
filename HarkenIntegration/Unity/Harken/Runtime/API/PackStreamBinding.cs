using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Harken
{
    /// <summary>
    /// Opens a fresh immutable lease on a native I/O thread. The path is relative to
    /// the platform source and includes the captured language scope. Return null
    /// for an unavailable file. A returned lease transfers ownership to the SDK,
    /// including on cancellation or validation failure. Calls may overlap; do not
    /// call Runtime or Unity APIs. Preserve old content revisions until leases drain.
    /// </summary>
    public delegate StreamByteSource PackStreamOpen(string path, ulong expectedContentRevision,
        ulong expectedByteLength, StreamReadCancellation cancellation);

    // Capture the actual source scope, including generic fallback, with each Pack generation.
    internal sealed class PackStreamBinding
    {
        readonly object source;
        readonly string scope, root;
        readonly ulong revision;
        readonly PackStreamOpen open;

        internal PackStreamBinding(object source, string scope, ulong revision, string root, PackStreamOpen open)
        { this.source = source; this.scope = scope; this.revision = revision; this.root = root; this.open = open; }

        internal static bool Same(PackStreamBinding a, PackStreamBinding b)
            => ReferenceEquals(a, b) || a != null && b != null && (a.root != null && b.root != null ? a.root == b.root : ReferenceEquals(a.source, b.source)) &&
                a.scope == b.scope && a.revision == b.revision;

        string RelativePath(IntPtr locator)
        {
            string value = Marshal.PtrToStringUTF8(locator);
            if (string.IsNullOrEmpty(value) || value[0] == '/' || value.IndexOfAny(new[] { '\\', ':', '\0' }) >= 0)
                throw new ArgumentException("Invalid stream locator.");
            foreach (string part in value.Split('/'))
                if (part.Length == 0 || part == "." || part == "..") throw new ArgumentException("Invalid stream locator.");
            return scope.Length == 0 ? value : scope + "/" + value;
        }

        internal sealed class NativeLease : IDisposable
        {
            static readonly VnPackStreamRetainFn RetainDelegate = Retain;
            static readonly VnByteSourceReleaseFn ReleaseDelegate = Release;
            static readonly VnPackStreamPathFn PathDelegate = ResolvePath;
            static readonly VnPackStreamOpenFn OpenDelegate = Open;
            static readonly IntPtr RetainPointer = Marshal.GetFunctionPointerForDelegate(RetainDelegate);
            static readonly IntPtr ReleasePointer = Marshal.GetFunctionPointerForDelegate(ReleaseDelegate);
            static readonly IntPtr PathPointer = Marshal.GetFunctionPointerForDelegate(PathDelegate);
            static readonly IntPtr OpenPointer = Marshal.GetFunctionPointerForDelegate(OpenDelegate);
            readonly PackStreamBinding binding;
            GCHandle handle;
            IntPtr path;
            int references = 1;
            int disposed;
            internal VnPackStreamResolverV1 Descriptor { get; }

            internal NativeLease(PackStreamBinding binding)
            {
                this.binding = binding;
                handle = GCHandle.Alloc(this);
                Descriptor = new VnPackStreamResolverV1
                {
                    structSize = (uint)Marshal.SizeOf<VnPackStreamResolverV1>(),
                    userData = GCHandle.ToIntPtr(handle), retain = RetainPointer, release = ReleasePointer,
                    path = binding.root == null ? IntPtr.Zero : PathPointer,
                    open = binding.root == null ? OpenPointer : IntPtr.Zero
                };
            }

            public void Dispose()
            {
                if (Interlocked.Exchange(ref disposed, 1) == 0) Drop();
            }

            void Drop()
            {
                if (Interlocked.Decrement(ref references) != 0) return;
                if (path != IntPtr.Zero) Marshal.FreeHGlobal(path);
                handle.Free();
            }

            [AOT.MonoPInvokeCallback(typeof(VnPackStreamRetainFn))]
            static VnResult Retain(IntPtr context)
            {
                try
                {
                    var lease = (NativeLease)GCHandle.FromIntPtr(context).Target;
                    Interlocked.Increment(ref lease.references);
                    return VnResult.VN_OK;
                }
                catch { return VnResult.VN_PACK_PROVIDER_FAILED; }
            }

            [AOT.MonoPInvokeCallback(typeof(VnByteSourceReleaseFn))]
            static void Release(IntPtr context)
            {
                try { ((NativeLease)GCHandle.FromIntPtr(context).Target).Drop(); } catch { }
            }

            [AOT.MonoPInvokeCallback(typeof(VnPackStreamPathFn))]
            static VnResult ResolvePath(IntPtr context, ulong pack, IntPtr locator, ulong revision, ulong bytes, IntPtr output)
            {
                try
                {
                    if (output == IntPtr.Zero) return VnResult.VN_INVALID_ARG;
                    Marshal.WriteIntPtr(output, IntPtr.Zero);
                    var lease = (NativeLease)GCHandle.FromIntPtr(context).Target;
                    string relative = lease.binding.RelativePath(locator);
                    string full = Path.GetFullPath(Path.Combine(lease.binding.root, relative));
                    byte[] utf8 = new UTF8Encoding(false, true).GetBytes(full + "\0");
                    IntPtr next = Marshal.AllocHGlobal(utf8.Length);
                    try { Marshal.Copy(utf8, 0, next, utf8.Length); }
                    catch { Marshal.FreeHGlobal(next); throw; }
                    if (lease.path != IntPtr.Zero) Marshal.FreeHGlobal(lease.path);
                    lease.path = next;
                    Marshal.WriteIntPtr(output, next);
                    return VnResult.VN_OK;
                }
                catch { return VnResult.VN_PACK_PROVIDER_FAILED; }
            }

            [AOT.MonoPInvokeCallback(typeof(VnPackStreamOpenFn))]
            static unsafe VnResult Open(IntPtr context, ulong pack, IntPtr locator, ulong revision,
                ulong bytes, IntPtr cancellation, IntPtr output)
            {
                StreamReadCancellation view = null;
                StreamByteSource source = null;
                GCHandle sourceRoot = default;
                bool claimed = false, transferred = false;
                try
                {
                    if (output == IntPtr.Zero) return VnResult.VN_INVALID_ARG;
                    var lease = (NativeLease)GCHandle.FromIntPtr(context).Target;
                    view = new StreamReadCancellation(cancellation);
                    if (view.IsCancellationRequested) return VnResult.VN_PACK_PROVIDER_FAILED;
                    source = lease.binding.open(lease.binding.RelativePath(locator), revision, bytes, view);
                    if (source == null) return VnResult.VN_PACK_PROVIDER_FAILED;
                    claimed = source.TryClaim();
                    // An already transferred lease belongs to its previous consumer.
                    if (!claimed) return VnResult.VN_INVALID_ARG;
                    if (view.IsCancellationRequested || source.ByteLength != bytes) return VnResult.VN_PACK_PROVIDER_FAILED;
                    sourceRoot = GCHandle.Alloc(source);
                    var destination = (VnByteSourceDescV1*)output.ToPointer();
                    destination[0] = new VnByteSourceDescV1
                    {
                        structSize = (uint)sizeof(VnByteSourceDescV1), byteLength = bytes,
                        userData = GCHandle.ToIntPtr(sourceRoot),
                        readAt = StreamByteSourceCallbacks.ReadPointer, release = StreamByteSourceCallbacks.ReleasePointer
                    };
                    transferred = true;
                    return VnResult.VN_OK;
                }
                catch { return VnResult.VN_PACK_PROVIDER_FAILED; }
                finally
                {
                    view?.Invalidate();
                    if (!transferred)
                    {
                        if (sourceRoot.IsAllocated) sourceRoot.Free();
                        if (claimed) { try { source.Dispose(); } catch { } }
                    }
                }
            }
        }
    }
}
