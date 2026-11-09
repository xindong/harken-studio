using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using Harken;

namespace Harken.Test
{
    internal static class StreamSourceContract
    {
        static readonly VnObjectId Sound = new VnObjectId(0x3da91a64b9474560UL, 7004);
        sealed class Probe
        {
            internal int Reads, Releases, Entered, AllowRead, ThrowRead, ThrowRelease;
            internal int AllowRelease = 1;
            internal StreamReadCancellation LastView;
        }
        sealed class Provider : StreamByteSource
        {
            readonly byte[] bytes;
            readonly Probe probe;
            internal Provider(byte[] input, Probe observation) { bytes = input; probe = observation; }
            public override ulong ByteLength => (ulong)bytes.Length;
            public override int ReadAt(ulong offset, IntPtr destination, uint count,
                out uint bytesRead, StreamReadCancellation cancellation)
            {
                bytesRead = 0;
                Interlocked.Increment(ref probe.Reads);
                Volatile.Write(ref probe.LastView, cancellation);
                Volatile.Write(ref probe.Entered, 1);
                while (Volatile.Read(ref probe.AllowRead) == 0 && !cancellation.IsCancellationRequested)
                    Thread.Sleep(1);
                if (cancellation.IsCancellationRequested) return VnConstants.VN_BYTE_READ_CANCELLED;
                if (Volatile.Read(ref probe.ThrowRead) != 0) throw new IOException("injected provider read");
                if (offset > (ulong)bytes.Length || count > (ulong)bytes.Length - offset)
                    return VnConstants.VN_BYTE_READ_IO_ERROR;
                bytesRead = Math.Min(count, 17);
                Marshal.Copy(bytes, checked((int)offset), destination, checked((int)bytesRead));
                return VnConstants.VN_BYTE_READ_OK;
            }
            public override void Dispose()
            {
                Interlocked.Increment(ref probe.Releases);
                while (Volatile.Read(ref probe.AllowRelease) == 0) Thread.Sleep(1);
                if (Volatile.Read(ref probe.ThrowRelease) != 0) throw new IOException("injected release");
            }
        }
        static void Check(bool condition, string message)
        { if (!condition) throw new InvalidOperationException(message); }
        static void Ok(VnResult result, string message) => Check(result == VnResult.VN_OK, message + ": " + result);
        static void Until(Func<bool> done, string message)
        {
            var time = Stopwatch.StartNew();
            while (!done() && time.ElapsedMilliseconds < 4000) Thread.Sleep(1);
            Check(done(), message);
        }
        static void Collect()
        { GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect(); }
        static void UntilCollected(WeakReference reference, string message)
        { Until(() => { Collect(); return !reference.IsAlive; }, message); }
        [MethodImpl(MethodImplOptions.NoInlining)]
        static WeakReference Begin(byte[] bytes, Probe probe, bool replace, out StreamSourceRequest request)
        {
            var provider = new Provider(bytes, probe);
            Ok(replace ? StreamSources.BeginReplace(Sound, provider, out request)
                       : StreamSources.BeginRegister(Sound, provider, out request), "provider begin");
            Check(StreamSources.BeginRegister(Sound, provider, out _) == VnResult.VN_INVALID_ARG,
                "one provider cannot transfer the same lease twice");
            return new WeakReference(provider);
        }
        [MethodImpl(MethodImplOptions.NoInlining)]
        static WeakReference Refused(byte[] bytes, Probe probe)
        {
            var provider = new Provider(bytes, probe);
            Check(StreamSources.BeginRegister(Sound, provider, out var request) == VnResult.VN_IN_USE && !request.IsValid,
                "published namespace must refuse without a request");
            Check(probe.Reads == 0 && probe.Releases == 0, "refused begin must not call or dispose provider");
            return new WeakReference(provider);
        }
        static void Ready(StreamSourceRequest request)
        {
            Until(() => request.Query(out var info) == VnResult.VN_OK && info.state == VnConstants.VN_STREAM_SOURCE_READY,
                "source did not become Ready");
        }
        static readonly VnReadIsCancelledFn CancellationQuery = context => (uint)Marshal.ReadInt32(context);

        static unsafe void PackResolvers()
        {
            byte[] bytes = new byte[64];
            var probe = new Probe { AllowRead = 1 };
            string opened = null;
            StreamReadCancellation openView = null;
            PackStreamOpen open = (path, revision, length, cancellation) =>
            {
                opened = path; openView = cancellation;
                Check(revision == 91 && length == 64, "expected media identity reaches opener");
                return new Provider(bytes, probe);
            };
            object identity = new object();
            var binding = new PackStreamBinding(identity, "French", 1, null, open);
            Check(PackStreamBinding.Same(binding, new PackStreamBinding(identity, "French", 1, null, open)),
                "identical capture source can keep its generation");
            Check(!PackStreamBinding.Same(binding, new PackStreamBinding(identity, "", 1, null, open)) &&
                !PackStreamBinding.Same(binding, new PackStreamBinding(identity, "French", 2, null, open)),
                "fallback scope and publication revisions replace source identity");
            var owner = new PackStreamBinding.NativeLease(binding);
            var descriptor = owner.Descriptor;
            var retain = Marshal.GetDelegateForFunctionPointer<VnPackStreamRetainFn>(descriptor.retain);
            var release = Marshal.GetDelegateForFunctionPointer<VnByteSourceReleaseFn>(descriptor.release);
            var invoke = Marshal.GetDelegateForFunctionPointer<VnPackStreamOpenFn>(descriptor.open);
            Ok(retain(descriptor.userData), "native retain");
            owner.Dispose(); owner.Dispose();
            Collect();
            int* cancellationFlag = stackalloc int[1];
            cancellationFlag[0] = 0;
            IntPtr cancellationPointer = (IntPtr)cancellationFlag;
            var view = new VnReadCancellationViewV1
            {
                structSize = (uint)sizeof(VnReadCancellationViewV1), context = cancellationPointer,
                isCancelled = Marshal.GetFunctionPointerForDelegate(CancellationQuery)
            };
            byte[] locator = System.Text.Encoding.UTF8.GetBytes("nested/Media.stream\0");
            VnByteSourceDescV1 source = default;
            fixed (byte* text = locator)
            {
                Ok(invoke(descriptor.userData, 7, (IntPtr)text, 91, 64, (IntPtr)(&view), (IntPtr)(&source)), "open retained Pack source");
                Check(opened == "French/nested/Media.stream" && openView.IsCancellationRequested,
                    "captured language scope and invalidated open cancellation");
                Check(probe.Releases == 0, "open lease remains live");
                var read = Marshal.GetDelegateForFunctionPointer<VnByteSourceReadAtFn>(source.readAt);
                var close = Marshal.GetDelegateForFunctionPointer<VnByteSourceReleaseFn>(source.release);
                byte* data = stackalloc byte[64];
                uint count = 0;
                Check(read(source.userData, 0, (IntPtr)data, 64, (IntPtr)(&count), (IntPtr)(&view)) == VnConstants.VN_BYTE_READ_OK && count == 17,
                    "Pack source reuses bounded byte reads");
                Check(probe.LastView.IsCancellationRequested, "read cancellation expires after callback");
                probe.ThrowRead = 1;
                Check(read(source.userData, 0, (IntPtr)data, 64, (IntPtr)(&count), (IntPtr)(&view)) == VnConstants.VN_BYTE_READ_IO_ERROR && count == 0,
                    "read exception cannot cross native boundary");
                probe.ThrowRelease = 1;
                close(source.userData);
                Check(probe.Releases == 1, "native close disposes once despite exception");
                Marshal.WriteInt32(cancellationPointer, 1);
                Check(invoke(descriptor.userData, 7, (IntPtr)text, 91, 64, (IntPtr)(&view), (IntPtr)(&source)) != VnResult.VN_OK && probe.Releases == 1,
                    "cancelled open never calls provider");
            }
            release(descriptor.userData);

            var rejectedProbe = new Probe { AllowRead = 1 };
            var rejected = new PackStreamBinding(identity, "", 1, null, (path, revision, length, cancellation) =>
            { Marshal.WriteInt32(cancellationPointer, 1); return new Provider(bytes, rejectedProbe); });
            using (var rejectedOwner = new PackStreamBinding.NativeLease(rejected))
            {
                descriptor = rejectedOwner.Descriptor;
                invoke = Marshal.GetDelegateForFunctionPointer<VnPackStreamOpenFn>(descriptor.open);
                Marshal.WriteInt32(cancellationPointer, 0);
                fixed (byte* text = locator)
                    Check(invoke(descriptor.userData, 7, (IntPtr)text, 91, 64, (IntPtr)(&view), (IntPtr)(&source)) != VnResult.VN_OK,
                        "cancellation after opener rejects lease");
                Check(rejectedProbe.Releases == 1, "late cancellation disposes returned lease");
            }
            string root = Path.GetFullPath(Path.GetTempPath());
            using (var paths = new PackStreamBinding.NativeLease(new PackStreamBinding(identity, "French", 0, root, null)))
            {
                descriptor = paths.Descriptor;
                var resolve = Marshal.GetDelegateForFunctionPointer<VnPackStreamPathFn>(descriptor.path);
                IntPtr output = IntPtr.Zero;
                fixed (byte* text = locator)
                    Ok(resolve(descriptor.userData, 7, (IntPtr)text, 91, 64, (IntPtr)(&output)), "resolve without opening file");
                Check(Marshal.PtrToStringUTF8(output) == Path.GetFullPath(Path.Combine(root, "French/nested/Media.stream")),
                    "directory source resolves captured scope");
                byte[] bad = System.Text.Encoding.UTF8.GetBytes("../Escape.stream\0");
                fixed (byte* text = bad)
                    Check(resolve(descriptor.userData, 7, (IntPtr)text, 91, 64, (IntPtr)(&output)) != VnResult.VN_OK && output == IntPtr.Zero,
                        "directory traversal refused");
            }
            Console.WriteLine("[Pack stream resolvers] scope, retention, cancellation, lease transfer and callback exceptions PASS.");
        }

        internal static bool Run()
        {
            bool initialized = false;
            try
            {
                PackResolvers();
                // Produced by StreamRegister.SoundIdNamespaceIsSharedWithPCM
                // using the native StreamBuilder, without a second format writer.
                string path = Path.Combine(AppContext.BaseDirectory, "Fixtures", "StreamSource.stream");
                byte[] bytes = File.ReadAllBytes(path);
                var config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                Ok(HarkenEngine.Init(config), "Init"); initialized = true;
                Ok(StreamSources.BeginRegister(Sound, path, out var file), "file begin");
                Ready(file); Ok(file.Commit(), "file commit");
                var refused = new Probe { AllowRead = 1 };
                WeakReference rejected = Refused(bytes, refused);
                Collect(); Check(!rejected.IsAlive, "refused adapter root leaked");
                var blocked = new Probe();
                WeakReference retained = Begin(bytes, blocked, true, out var pending);
                Until(() => Volatile.Read(ref blocked.Entered) != 0, "held ReadAt not entered");
                Collect(); Check(retained.IsAlive, "accepted provider lost its GC root");
                Ok(HarkenEngine.Update(), "Update while source read is held");
                Check(pending.Commit() == VnResult.VN_BACKPRESSURE, "Preparing must not commit");
                int stopPoll = 0;
                var latePoll = new Thread(() => {
                    while (Volatile.Read(ref stopPoll) == 0) {
                        var view = Volatile.Read(ref blocked.LastView);
                        if (view != null) _ = view.IsCancellationRequested;
                        Thread.Yield();
                    }
                });
                latePoll.Start();
                try
                {
                    Ok(pending.Cancel(), "held cancel");
                    Check(pending.Query(out _) == VnResult.VN_STALE_HANDLE, "Cancel must abandon public handle");
                    pending.Dispose();
                    Until(() => Volatile.Read(ref blocked.Releases) == 1, "cancel release did not drain");
                    Check(blocked.LastView.IsCancellationRequested, "returned view must be safely invalidated");
                }
                finally { Volatile.Write(ref stopPoll, 1); latePoll.Join(); }
                UntilCollected(retained, "accepted adapter root leaked after Release");
                Check(StreamSources.Unregister(Sound) == VnResult.VN_OK, "cancelled replacement changed old source");

                var good = new Probe { AllowRead = 1 };
                WeakReference live = Begin(bytes, good, false, out var ready);
                Collect(); Ready(ready); Ok(ready.Commit(), "callback source commit");
                ready.Dispose();
                Check(good.Releases == 0 && live.IsAlive, "request disposal released committed source");
                Ok(StreamSources.Unregister(Sound), "callback unregister");
                Until(() => Volatile.Read(ref good.Releases) == 1, "unregister release");
                UntilCollected(live, "committed source root leaked");

                var broken = new Probe { AllowRead = 1, ThrowRead = 1, ThrowRelease = 1 };
                WeakReference failed = Begin(bytes, broken, false, out var failedRequest);
                Until(() => failedRequest.Query(out var info) == VnResult.VN_OK && info.state == VnConstants.VN_STREAM_SOURCE_FAILED,
                    "managed exception must become source failure");
                Check(failedRequest.Commit() == VnResult.VN_BAD_STREAM_FILE && broken.Releases == 0, "failed request must retain ownership");
                try
                {
                    Volatile.Write(ref broken.AllowRelease, 0);
                    failedRequest.Dispose();
                    Until(() => Volatile.Read(ref broken.Releases) == 1, "failed source Release");
                    Collect(); Check(failed.IsAlive, "provider must remain alive while Dispose is running");
                }
                finally { Volatile.Write(ref broken.AllowRelease, 1); }
                // Release entry is observable before the thunk frees its root.
                UntilCollected(failed, "throwing Dispose leaked GCHandle");

                var term = new Probe { ThrowRelease = 1 };
                WeakReference termRoot = Begin(bytes, term, false, out var oldRequest);
                Until(() => Volatile.Read(ref term.Entered) != 0, "Term source ReadAt");
                Ok(HarkenEngine.Term(), "Term cancels accepted provider"); initialized = false;
                Check(term.Releases == 1 && term.LastView.IsCancellationRequested, "Term must drain exactly once");
                Collect(); Check(!termRoot.IsAlive, "Term root leak");
                Ok(HarkenEngine.Init(config), "reinit"); initialized = true;
                Ok(StreamSources.BeginRegister(Sound, path, out var fresh), "new engine source");
                Check(oldRequest.Cancel() == VnResult.VN_STALE_HANDLE, "old request aliased new engine");
                Ready(fresh); Ok(fresh.Cancel(), "fresh cancel");
                Console.WriteLine("[Stream sources] native async calls, rooted AOT thunks, rejection, cancellation, GC and Term PASS.");
                return true;
            }
            catch (Exception error) { Console.WriteLine("[Stream sources] FAIL: " + error); return false; }
            finally { if (initialized) HarkenEngine.Term(); }
        }
    }
}
