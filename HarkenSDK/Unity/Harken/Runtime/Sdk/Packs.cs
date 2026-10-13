using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Harken
{
    public readonly struct InitPackInspection
    {
        public readonly uint Status;
        public readonly VnResult Result;
        public readonly ulong PackStableId;
        public readonly ulong CatalogEntries;
        public readonly ulong CatalogBytes;
        public readonly uint RecommendedMaxCatalogEntries;
        public readonly uint RecommendedMaxActions;
        public readonly ulong RecommendedCatalogBytes;
        public readonly ulong RecommendedResidentDefinitionBytes;
        public readonly ulong RequiredCapabilityMask;
        public readonly ulong SemanticFingerprint;

        internal InitPackInspection(VnInitPackInspectReport report)
        {
            Status = report.status;
            Result = (VnResult)report.result;
            PackStableId = report.packStableId;
            CatalogEntries = report.catalogEntries;
            CatalogBytes = report.catalogBytes;
            RecommendedMaxCatalogEntries = report.recommendedMaxCatalogEntries;
            RecommendedMaxActions = report.recommendedMaxActions;
            RecommendedCatalogBytes = report.recommendedCatalogBytes;
            RecommendedResidentDefinitionBytes = report.recommendedResidentDefinitionBytes;
            RequiredCapabilityMask = report.requiredCapabilityMask;
            SemanticFingerprint = report.semanticFingerprint;
        }
    }

    /// <summary>Ergonomic handle over a loaded .pack (an SDK-facing name).</summary>
    public readonly struct Pack
    {
        public readonly ulong Handle;

        public Pack(ulong handle)
        {
            Handle = handle;
        }

        public bool IsValid => Handle != 0;

        public VnResult Unload() => Packs.UnloadPack(this);
    }

    public readonly struct CanonicalPackMember
    {
        public readonly ulong StableId;
        public readonly uint Kind;
        public readonly Pack Expected;
        public readonly string Path;
        public readonly string ContainmentRoot;
        public readonly PackByteProvider.Handler Provider;
        public readonly object ProviderState;

        public CanonicalPackMember(ulong stableId, uint kind, string path = null,
            string containmentRoot = null, PackByteProvider.Handler provider = null,
            object providerState = null, Pack expected = default)
        {
            StableId = stableId;
            Kind = kind;
            Expected = expected;
            Path = path;
            ContainmentRoot = containmentRoot;
            Provider = provider;
            ProviderState = providerState;
        }
    }

    /// <summary>
    /// Pack load/unload (the SDK-facing names <c>LoadPack</c> / <c>UnloadPack</c>).
    /// Loading marshals a UTF-8 path into a blittable <see cref="VnPackDesc"/>;
    /// this is a load-time call, not the hot path.
    /// </summary>
    public static class Packs
    {
        /// <summary>SDK mutation stamp for cache invalidation, not native readiness or identity.</summary>
        public static ulong ChangeVersion { get; private set; }

        internal static VnResult ObserveMutation(VnResult result)
        {
            if (result == VnResult.VN_OK) ChangeVersion = unchecked(ChangeVersion + 1);
            return result;
        }

        private static readonly uint PackDescSize = (uint)Marshal.SizeOf<VnPackDesc>();
        private static readonly uint PackLoadDescSize =
            (uint)Marshal.SizeOf<VnPackLoadDesc>();
        private static readonly uint InspectReportSize =
            (uint)Marshal.SizeOf<VnInitPackInspectReport>();

        public static unsafe VnResult ActivateCanonicalPackSet(
            CanonicalPackMember[] members, ulong setRevision,
            out VnCanonicalPackSetActivationReportV1 report,
            out VnCanonicalPackSetMemberResultV1[] results,
            Pack expectedCurrentInit = default, ulong baseSetRevision = 0)
        {
            report = default;
            results = Array.Empty<VnCanonicalPackSetMemberResultV1>();
            if (members == null || members.Length == 0 || members.Length > int.MaxValue / 2)
                return VnResult.VN_INVALID_ARG;
            GCHandle[] pins = null;
            int[] providerKeys = null;
            try
            {
                var native = new VnCanonicalPackSetMemberV1[members.Length];
                var rows = new VnCanonicalPackSetMemberResultV1[members.Length];
                pins = new GCHandle[members.Length * 2];
                providerKeys = new int[members.Length];
                var encoding = new UTF8Encoding(false, true);
                for (int i = 0; i < members.Length; ++i)
                {
                    CanonicalPackMember source = members[i];
                    if (source.StableId == 0 || source.Kind != (i == 0
                            ? VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT
                            : VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT) ||
                        (i > 0 && source.StableId == members[0].StableId) ||
                        (i > 1 && source.StableId <= members[i - 1].StableId) ||
                        (source.Provider != null && (source.Path != null || source.ContainmentRoot != null)) ||
                        (source.Provider == null && string.IsNullOrEmpty(source.Path)))
                        return VnResult.VN_INVALID_ARG;
                    ref VnCanonicalPackSetMemberV1 member = ref native[i];
                    member.structSize = (uint)sizeof(VnCanonicalPackSetMemberV1);
                    member.packStableId = source.StableId;
                    member.packKind = source.Kind;
                    member.expectedHandle = source.Expected.Handle;
                    rows[i].structSize = (uint)sizeof(VnCanonicalPackSetMemberResultV1);
                    if (source.Provider != null)
                    {
                        providerKeys[i] = PackByteProvider.Register(source.Provider, source.ProviderState);
                        if (providerKeys[i] == 0) return VnResult.VN_BACKPRESSURE;
                        PackByteProvider.BindMember(ref member, providerKeys[i]);
                        continue;
                    }
                    string[] text = { source.Path, source.ContainmentRoot };
                    for (int j = 0; j < text.Length; ++j)
                    {
                        if (text[j] == null) continue;
                        if (text[j].Length == 0 || text[j].IndexOf('\0') >= 0 ||
                            encoding.GetByteCount(text[j]) >= 32768)
                            return VnResult.VN_INVALID_ARG;
                        byte[] bytes = encoding.GetBytes(text[j] + "\0");
                        int pinIndex = i * 2 + j;
                        pins[pinIndex] = GCHandle.Alloc(bytes, GCHandleType.Pinned);
                        if (j == 0) member.utf8Path = pins[pinIndex].AddrOfPinnedObject();
                        else
                        {
                            member.utf8ContainmentRoot = pins[pinIndex].AddrOfPinnedObject();
                            member.flags = VnConstants.VN_CANONICAL_PACK_SET_MEMBER_ROOT;
                        }
                    }
                }
                fixed (VnCanonicalPackSetMemberV1* memberPtr = native)
                fixed (VnCanonicalPackSetMemberResultV1* rowPtr = rows)
                {
                    VnCanonicalPackSetActivationDescV1 desc = default;
                    desc.structSize = (uint)sizeof(VnCanonicalPackSetActivationDescV1);
                    desc.expectedCurrentInitHandle = expectedCurrentInit.Handle;
                    desc.baseSetRevision = baseSetRevision;
                    desc.setRevision = setRevision;
                    desc.memberCount = (uint)members.Length;
                    desc.memberStructSize = (uint)sizeof(VnCanonicalPackSetMemberV1);
                    desc.members = (IntPtr)memberPtr;
                    desc.outputMemberCapacity = (uint)members.Length;
                    desc.outputMemberStructSize = (uint)sizeof(VnCanonicalPackSetMemberResultV1);
                    desc.outputMembers = (IntPtr)rowPtr;
                    VnCanonicalPackSetActivationReportV1 nativeReport = default;
                    nativeReport.structSize = (uint)sizeof(VnCanonicalPackSetActivationReportV1);
                    VnResult result = HarkenNative.Vn_ActivateCanonicalPackSetV1(desc, ref nativeReport);
                    if (result == VnResult.VN_OK)
                    {
                        report = nativeReport;
                        results = rows;
                    }
                    return ObserveMutation(result);
                }
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (EncoderFallbackException) { return VnResult.VN_INVALID_ARG; }
            finally
            {
                if (providerKeys != null)
                    foreach (int key in providerKeys)
                        if (key != 0) PackByteProvider.Unregister(key);
                if (pins != null)
                    foreach (GCHandle pin in pins)
                        if (pin.IsAllocated) pin.Free();
            }
        }

        public static VnResult UnloadCanonicalPackSet(Pack initPack, ulong baseSetRevision,
            ulong unloadRevision)
        {
            VnCanonicalPackSetUnloadDescV1 desc = default;
            desc.structSize = (uint)Marshal.SizeOf<VnCanonicalPackSetUnloadDescV1>();
            desc.expectedInitHandle = initPack.Handle;
            desc.baseSetRevision = baseSetRevision;
            desc.unloadRevision = unloadRevision;
            return ObserveMutation(HarkenNative.Vn_UnloadCanonicalPackSetV1(desc));
        }

        public static unsafe VnResult LoadPack(string utf8Path, out Pack pack)
        {
            pack = default;
            Span<byte> stackBuf = stackalloc byte[1024];
            byte[] heap = Utf8.TryEncode(utf8Path, stackBuf, out _)
                ? null
                : Encoding.UTF8.GetBytes((utf8Path ?? string.Empty) + "\0");

            fixed (byte* stackP = stackBuf)
            fixed (byte* heapP = heap)
            {
                VnPackDesc desc = default;
                desc.structSize = PackDescSize;
                desc.utf8Path = (IntPtr)(heap != null ? heapP : stackP);
                VnResult r = HarkenNative.Vn_LoadPack(desc, out ulong h);
                pack = new Pack(h);
                return ObserveMutation(r);
            }
        }

        public static VnResult UnloadPack(Pack pack) => ObserveMutation(HarkenNative.Vn_UnloadPack(pack.Handle));

        public static unsafe VnResult InspectInitPack(
            string utf8Path, out InitPackInspection inspection)
        {
            inspection = default;
            Span<byte> stackBuf = stackalloc byte[1024];
            byte[] heap = Utf8.TryEncode(utf8Path, stackBuf, out _)
                ? null
                : utf8Path == null
                    ? null
                    : Encoding.UTF8.GetBytes(utf8Path + "\0");

            fixed (byte* stackP = stackBuf)
            fixed (byte* heapP = heap)
            {
                VnPackLoadDesc desc = default;
                desc.structSize = PackLoadDescSize;
                desc.utf8Path = utf8Path == null
                    ? IntPtr.Zero
                    : (IntPtr)(heap != null ? heapP : stackP);
                VnInitPackInspectReport native = default;
                native.structSize = InspectReportSize;
                VnResult result = HarkenNative.Vn_InspectInitPack(
                    desc, ref native);
                inspection = new InitPackInspection(native);
                return result;
            }
        }

        public static unsafe VnResult LoadInitPack(
            string utf8Path, string utf8ContainmentRoot, out Pack pack)
        {
            return LoadCanonicalPack(
                0, utf8Path, utf8ContainmentRoot, out pack);
        }

        public static unsafe VnResult LoadPack(
            ulong packStableId, string utf8ContainmentRoot, out Pack pack)
            => LoadPack(packStableId, utf8ContainmentRoot, null, out pack);

        public static unsafe VnResult LoadPack(
            ulong packStableId, string utf8ContainmentRoot, string language, out Pack pack)
        {
            pack = default;
            if (!PackManifest.TryResolvePackPath(
                    utf8ContainmentRoot, packStableId, language, out string packPath))
            {
                return VnResult.VN_UNKNOWN_ID;
            }
            return LoadCanonicalPack(
                packStableId, packPath, utf8ContainmentRoot, out pack);
        }

        public static VnResult ReplacePack(
            Pack pack, string utf8Path, string utf8ContainmentRoot)
        {
            if (!pack.IsValid || string.IsNullOrEmpty(utf8Path) ||
                string.IsNullOrEmpty(utf8ContainmentRoot))
            {
                return VnResult.VN_INVALID_ARG;
            }
            return VnResult.VN_PACK_INIT_LIFECYCLE;
        }

        private static unsafe VnResult LoadCanonicalPack(
            ulong packStableId, string utf8Path,
            string utf8ContainmentRoot, out Pack pack)
        {
            pack = default;
            if (packStableId != 0 && string.IsNullOrEmpty(utf8ContainmentRoot))
            {
                return VnResult.VN_INVALID_ARG;
            }
            if (packStableId == 0 && string.IsNullOrEmpty(utf8Path))
            {
                return VnResult.VN_INVALID_ARG;
            }

            Span<byte> pathStack = stackalloc byte[1024];
            Span<byte> rootStack = stackalloc byte[1024];
            byte[] pathHeap = utf8Path == null ||
                Utf8.TryEncode(utf8Path, pathStack, out _)
                ? null
                : Encoding.UTF8.GetBytes(utf8Path + "\0");
            byte[] rootHeap = utf8ContainmentRoot == null ||
                Utf8.TryEncode(utf8ContainmentRoot, rootStack, out _)
                ? null
                : Encoding.UTF8.GetBytes(utf8ContainmentRoot + "\0");

            fixed (byte* pathStackP = pathStack)
            fixed (byte* pathHeapP = pathHeap)
            fixed (byte* rootStackP = rootStack)
            fixed (byte* rootHeapP = rootHeap)
            {
                VnPackLoadDesc desc = default;
                desc.structSize = PackLoadDescSize;
                desc.packStableId = packStableId;
                if (utf8Path != null)
                {
                    desc.utf8Path =
                        (IntPtr)(pathHeap != null ? pathHeapP : pathStackP);
                }
                if (utf8ContainmentRoot != null)
                {
                    desc.flags = VnConstants.VN_PACK_LOAD_FLAG_ROOT;
                    desc.utf8ContainmentRoot =
                        (IntPtr)(rootHeap != null ? rootHeapP : rootStackP);
                }
                VnResult result = HarkenNative.Vn_LoadPackV2(
                    desc, out ulong handle);
                pack = new Pack(handle);
                return ObserveMutation(result);
            }
        }
    }
}
