using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Harken
{
    /// <summary>
    /// AOT-safe pack byte-provider adapter. Provide/release run on the
    /// logic/game thread during load. The managed buffer is pinned until
    /// release; failure leaves the native span zero and returns a VnResult.
    /// Thunks never throw across the C ABI.
    /// </summary>
    public static class PackByteProvider
    {
        public delegate VnResult Handler(
            ulong packStableId,
            string relativeLocatorUtf8,
            uint variantKey,
            ulong expectedContentHash,
            out byte[] payload,
            object state);

        internal static int OutstandingPinCount { get; private set; }
        internal static int LastProvideCalls { get; private set; }
        internal static int LastReleaseCalls { get; private set; }
        internal static bool LastProvideLeftSpanZero { get; private set; }

        static readonly VnPackByteProviderFn ProvideDelegate = ProvideThunk;
        static readonly VnPackByteReleaseFn ReleaseDelegate = ReleaseThunk;
        static readonly IntPtr ProvideFnPtr =
            Marshal.GetFunctionPointerForDelegate(ProvideDelegate);
        static readonly IntPtr ReleaseFnPtr =
            Marshal.GetFunctionPointerForDelegate(ReleaseDelegate);
        static readonly uint PackLoadDescSize =
            (uint)Marshal.SizeOf<VnPackLoadDesc>();
        static readonly uint SpanSize =
            (uint)Marshal.SizeOf<VnPackByteSpan>();
        static readonly uint InspectReportSize = (uint)Marshal.SizeOf<VnInitPackInspectReport>();

        struct Entry
        {
            public Handler Handler;
            public object State;
            public GCHandle Pin;
        }

        static readonly Dictionary<int, Entry> Entries = new Dictionary<int, Entry>();
        static int _nextKey = 1;

        internal static int Register(Handler handler, object state)
        {
            if (_nextKey == int.MaxValue) return 0;
            int key = _nextKey++;
            Entries.Add(key, new Entry { Handler = handler, State = state });
            return key;
        }

        internal static void BindMember(ref VnCanonicalPackSetMemberV1 member, int key)
        {
            member.flags = VnConstants.VN_CANONICAL_PACK_SET_MEMBER_PROVIDER;
            member.providerUserData = (IntPtr)key;
            member.provide = ProvideFnPtr;
            member.release = ReleaseFnPtr;
        }

        internal static void Unregister(int key)
        {
            FreePin(key);
            Entries.Remove(key);
        }

        public static VnResult LoadPack(ulong packStableId, Handler handler,
                                        object state, out Pack pack)
            => Invoke(packStableId, handler, state, false, out pack, out _);

        public static VnResult InspectInitPack(ulong packStableId, Handler handler,
            object state, out InitPackInspection inspection)
            => Invoke(packStableId, handler, state, true, out _, out inspection);

        internal static VnResult PreparedBytes(ulong packStableId, string locator, uint variantKey,
            ulong expectedContentHash, out byte[] payload, object state)
        {
            payload = state as byte[];
            return payload != null && payload.Length != 0 ? VnResult.VN_OK : VnResult.VN_CONTENT_NOT_LOADED;
        }

        private static VnResult Invoke(ulong packStableId, Handler handler, object state, bool inspect,
            out Pack pack, out InitPackInspection inspection)
        {
            pack = default;
            inspection = default;
            LastProvideCalls = 0;
            LastReleaseCalls = 0;
            LastProvideLeftSpanZero = false;
            if (handler == null)
            {
                return VnResult.VN_INVALID_ARG;
            }

            int key = _nextKey++;
            Entries[key] = new Entry { Handler = handler, State = state };
            try
            {
                VnPackLoadDesc desc = default;
                desc.structSize = PackLoadDescSize;
                desc.flags = VnConstants.VN_PACK_LOAD_FLAG_PROVIDER;
                desc.packStableId = packStableId;
                desc.providerUserData = (IntPtr)key;
                desc.provide = ProvideFnPtr;
                desc.release = ReleaseFnPtr;
                if (inspect)
                {
                    VnInitPackInspectReport report = default;
                    report.structSize = InspectReportSize;
                    VnResult inspected = HarkenNative.Vn_InspectInitPack(desc, ref report);
                    inspection = new InitPackInspection(report);
                    return inspected;
                }
                VnResult result = HarkenNative.Vn_LoadPackV2(desc, out ulong handle);
                pack = new Pack(handle);
                return Packs.ObserveMutation(result);
            }
            finally
            {
                FreePin(key);
                Entries.Remove(key);
            }
        }

        static void FreePin(int key)
        {
            if (!Entries.TryGetValue(key, out Entry entry))
            {
                return;
            }
            if (entry.Pin.IsAllocated)
            {
                entry.Pin.Free();
                OutstandingPinCount--;
                entry.Pin = default;
                Entries[key] = entry;
            }
        }

        static unsafe void ZeroSpan(IntPtr outSpan)
        {
            if (outSpan == IntPtr.Zero)
            {
                return;
            }
            VnPackByteSpan* span = (VnPackByteSpan*)outSpan.ToPointer();
            span->structSize = SpanSize;
            span->reserved0 = 0;
            span->bytes = IntPtr.Zero;
            span->byteCount = 0;
        }

        [AOT.MonoPInvokeCallback(typeof(VnPackByteProviderFn))]
        static unsafe VnResult ProvideThunk(
            IntPtr userData, ulong packStableId, IntPtr relativeLocatorUtf8,
            uint variantKey, ulong expectedContentHash, IntPtr outSpan)
        {
            LastProvideCalls++;
            LastProvideLeftSpanZero = true;
            try
            {
                ZeroSpan(outSpan);
                if (outSpan == IntPtr.Zero)
                {
                    return VnResult.VN_INVALID_ARG;
                }
                int key = (int)userData;
                if (!Entries.TryGetValue(key, out Entry entry) ||
                    entry.Handler == null)
                {
                    return VnResult.VN_PACK_PROVIDER_FAILED;
                }

                string locator = relativeLocatorUtf8 == IntPtr.Zero
                    ? string.Empty
                    : Marshal.PtrToStringUTF8(relativeLocatorUtf8);
                byte[] payload = null;
                VnResult provided = entry.Handler(
                    packStableId, locator, variantKey, expectedContentHash,
                    out payload, entry.State);
                if (provided != VnResult.VN_OK)
                {
                    return provided;
                }
                if (payload == null || payload.Length == 0)
                {
                    return VnResult.VN_PACK_PROVIDER_FAILED;
                }

                if (entry.Pin.IsAllocated)
                {
                    entry.Pin.Free();
                    OutstandingPinCount--;
                }
                entry.Pin = GCHandle.Alloc(payload, GCHandleType.Pinned);
                OutstandingPinCount++;
                Entries[key] = entry;

                VnPackByteSpan* span = (VnPackByteSpan*)outSpan.ToPointer();
                span->structSize = SpanSize;
                span->reserved0 = 0;
                span->bytes = entry.Pin.AddrOfPinnedObject();
                span->byteCount = (ulong)payload.Length;
                LastProvideLeftSpanZero = false;
                return VnResult.VN_OK;
            }
            catch
            {
                ZeroSpan(outSpan);
                LastProvideLeftSpanZero = true;
                return VnResult.VN_PACK_PROVIDER_FAILED;
            }
        }

        [AOT.MonoPInvokeCallback(typeof(VnPackByteReleaseFn))]
        static void ReleaseThunk(IntPtr userData, ulong packStableId,
                                 VnPackByteSpan span)
        {
            LastReleaseCalls++;
            try
            {
                FreePin((int)userData);
            }
            catch
            {
            }
        }
    }
}
