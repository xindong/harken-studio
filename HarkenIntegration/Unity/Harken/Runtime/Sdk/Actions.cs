using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Harken
{
    public readonly struct ActionSeekTicket : IEquatable<ActionSeekTicket>
    {
        private static readonly uint InfoSize = (uint)Marshal.SizeOf<VnActionSeekInfoV1>();

        public readonly ulong Handle;

        public ActionSeekTicket(ulong handle) { Handle = handle; }

        public bool IsValid => Handle != 0;

        public VnResult Query(out VnActionSeekInfoV1 info)
        {
            info = default;
            info.structSize = InfoSize;
            return HarkenNative.Vn_QueryActionSeek(Handle, ref info);
        }

        public VnResult Release() => HarkenNative.Vn_ReleaseActionSeek(Handle);

        public bool Equals(ActionSeekTicket other) => Handle == other.Handle;
        public override bool Equals(object obj) => obj is ActionSeekTicket ticket && Equals(ticket);
        public override int GetHashCode() => Handle.GetHashCode();
    }

    public readonly struct ActionFiring : IEquatable<ActionFiring>
    {
        private static readonly uint InfoSize =
            (uint)Marshal.SizeOf<VnActionFiringInfo>();
        private static readonly uint StepResultSize =
            (uint)Marshal.SizeOf<VnActionStepResult>();
        private static readonly uint SeekDescSize =
            (uint)Marshal.SizeOf<VnActionSeekDescV1>();

        public readonly ulong Handle;

        public ActionFiring(ulong handle)
        {
            Handle = handle;
        }

        public bool IsValid => Handle != 0;

        public VnResult Stop(uint fadeMs)
            => HarkenNative.Vn_StopActionFiring(Handle, fadeMs);

        /// <summary>VN_OK admits the seek; retain its ticket until applied or cancelled, then release it.</summary>
        public VnResult Seek(double positionMs, VnActionSeekMode mode, out ActionSeekTicket ticket)
        {
            VnActionSeekDescV1 desc = default;
            desc.structSize = SeekDescSize;
            desc.firing = Handle;
            desc.positionMs = positionMs;
            desc.mode = (uint)mode;
            VnResult result = HarkenNative.Vn_SeekActionFiring(in desc, out ulong handle);
            ticket = new ActionSeekTicket(handle);
            return result;
        }

        public VnResult Query(out VnActionFiringInfo info)
        {
            info = default;
            info.structSize = InfoSize;
            return HarkenNative.Vn_QueryActionFiring(Handle, ref info);
        }

        public VnResult QueryStep(uint authoredEventIndex,
                                  out VnActionStepResult result)
        {
            result = default;
            result.structSize = StepResultSize;
            return HarkenNative.Vn_QueryActionStep(
                Handle, authoredEventIndex, ref result);
        }

        public VnResult Release()
            => HarkenNative.Vn_ReleaseActionFiring(Handle);

        public bool Equals(ActionFiring other) => Handle == other.Handle;

        public override bool Equals(object obj)
            => obj is ActionFiring firing && Equals(firing);

        public override int GetHashCode() => Handle.GetHashCode();
    }

    public struct ActionReference
    {
        public byte[] ProjectGuid;
        public VnObjectId ActionId;
        public string ExpectedName;
        public ulong NameBindingRevision;
        public ulong CallableFingerprint;
    }

    public readonly struct ActionResolved
    {
        readonly VnActionResolved native;

        internal ActionResolved(in VnActionResolved native)
        {
            this.native = native;
        }

        public VnObjectId ActionId => native.actionId;
        public ulong NameBindingRevision => native.nameBindingRevision;
        public ulong CallableFingerprint => native.callableFingerprint;
        public ulong PublicationGeneration => native.publicationGeneration;

        internal VnActionResolved Native => native;
    }

    public static class Actions
    {
        private static readonly uint PostDescSize =
            (uint)Marshal.SizeOf<VnPostActionDesc>();
        private static readonly uint ByNameDescSize =
            (uint)Marshal.SizeOf<VnPostActionByNameDesc>();
        private static readonly uint PinnedDescSize =
            (uint)Marshal.SizeOf<VnActionPinnedDesc>();
        private static readonly uint ResolvedSize =
            (uint)Marshal.SizeOf<VnActionResolved>();

        public static VnResult Post(VnObjectId actionId, Emitter emitter,
                                    out ActionFiring firing)
            => Post(actionId, emitter, 0, out firing);

        public static VnResult Post(VnObjectId actionId, Emitter emitter,
                                    ulong callerSeed,
                                    out ActionFiring firing)
        {
            VnPostActionDesc desc = default;
            desc.structSize = PostDescSize;
            desc.actionId = actionId;
            desc.emitter = emitter.Handle;
            desc.callerSeed = callerSeed;
            VnResult result = HarkenNative.Vn_PostAction(desc, out ulong handle);
            firing = new ActionFiring(handle);
            return result;
        }

        public static VnResult Post(string actionName, Emitter emitter,
                                    out ActionFiring firing)
            => Post(actionName, emitter, 0, out firing);

        public static unsafe VnResult Post(string actionName, Emitter emitter,
                                           ulong callerSeed,
                                           out ActionFiring firing)
        {
            firing = default;
            if (actionName != null && actionName.IndexOf('\0') >= 0)
                return VnResult.VN_INVALID_ARG;
            Span<byte> stackBuf = stackalloc byte[256];
            byte[] heap = EncodeName(actionName, stackBuf);
            fixed (byte* stackP = stackBuf)
            fixed (byte* heapP = heap)
            {
                VnPostActionByNameDesc desc = default;
                desc.structSize = ByNameDescSize;
                desc.utf8Name = actionName == null
                    ? IntPtr.Zero
                    : (IntPtr)(heap != null ? heapP : stackP);
                desc.emitter = emitter.Handle;
                desc.callerSeed = callerSeed;
                VnResult result = HarkenNative.Vn_PostActionByName(
                    desc, out ulong handle);
                firing = new ActionFiring(handle);
                return result;
            }
        }

        public static unsafe VnResult Post(in ActionReference pinned,
                                           Emitter emitter, ulong callerSeed,
                                           out ActionFiring firing)
        {
            firing = default;
            if (pinned.ExpectedName == null || pinned.ExpectedName.IndexOf('\0') >= 0 ||
                pinned.ProjectGuid == null || pinned.ProjectGuid.Length != 16 || pinned.ActionId.IsNull)
            {
                return VnResult.VN_INVALID_ARG;
            }

            Span<byte> stackBuf = stackalloc byte[256];
            byte[] heap = EncodeName(pinned.ExpectedName, stackBuf);
            fixed (byte* stackP = stackBuf)
            fixed (byte* heapP = heap)
            {
                VnActionPinnedDesc desc = default;
                desc.structSize = PinnedDescSize;
                for (int i = 0; i < 16; i++)
                {
                    desc.projectGuid[i] = pinned.ProjectGuid[i];
                }
                desc.actionId = pinned.ActionId;
                desc.utf8ExpectedName = (IntPtr)(heap != null ? heapP : stackP);
                desc.nameBindingRevision = pinned.NameBindingRevision;
                desc.callableFingerprint = pinned.CallableFingerprint;
                desc.emitter = emitter.Handle;
                desc.callerSeed = callerSeed;
                VnResult result = HarkenNative.Vn_PostActionPinned(
                    desc, out ulong handle);
                firing = new ActionFiring(handle);
                return result;
            }
        }

        public static unsafe VnResult Resolve(string actionName,
                                              out ActionResolved resolved)
            => Resolve(actionName, Guid.Empty, out resolved);

        public static VnResult Resolve(string actionName, string projectId,
                                       out ActionResolved resolved)
        {
            resolved = default;
            if (!Guid.TryParseExact(projectId, "D", out Guid project) ||
                project == Guid.Empty)
                return VnResult.VN_INVALID_ARG;
            return Resolve(actionName, project, out resolved);
        }

        private static unsafe VnResult Resolve(string actionName, Guid project,
                                               out ActionResolved resolved)
        {
            resolved = default;
            if (actionName != null && actionName.IndexOf('\0') >= 0)
                return VnResult.VN_INVALID_ARG;
            Span<byte> stackBuf = stackalloc byte[256];
            byte[] heap = EncodeName(actionName, stackBuf);
            fixed (byte* stackP = stackBuf)
            fixed (byte* heapP = heap)
            {
                VnPostActionByNameDesc desc = default;
                desc.structSize = ByNameDescSize;
                Span<byte> projectBytes = stackalloc byte[16];
                project.TryWriteBytes(projectBytes);
                for (int i = 0; i < 16; ++i)
                {
                    int source = i < 4 ? 3 - i : i < 6 ? 9 - i : i < 8 ? 13 - i : i;
                    desc.projectGuid[i] = projectBytes[source];
                }
                desc.utf8Name = actionName == null
                    ? IntPtr.Zero
                    : (IntPtr)(heap != null ? heapP : stackP);
                VnActionResolved native = default;
                native.structSize = ResolvedSize;
                VnResult result = HarkenNative.Vn_ResolveActionByName(
                    desc, ref native);
                if (result == VnResult.VN_OK)
                {
                    resolved = new ActionResolved(native);
                }
                return result;
            }
        }

        public static VnResult Post(in ActionResolved resolved, Emitter emitter,
                                    ulong callerSeed, out ActionFiring firing)
        {
            VnActionResolved native = resolved.Native;
            native.structSize = ResolvedSize;
            VnResult result = HarkenNative.Vn_PostActionResolved(
                native, emitter.Handle, callerSeed, out ulong handle);
            firing = new ActionFiring(handle);
            return result;
        }

        public static string Describe(VnResult result, string actionName)
        {
            string quoted = "'" + (actionName ?? string.Empty) + "'";
            string code = result + "/" + (int)result;
            switch (result)
            {
                case VnResult.VN_OK:
                    return "Action " + quoted + " posted (" + code + ").";
                case VnResult.VN_UNKNOWN_ID:
                    return "Action " + quoted + " is unknown (" + code + ").";
                case VnResult.VN_GAMEPLAY_NAME_RETIRED:
                    return "Action " + quoted +
                           " is retired or renamed (" + code + ").";
                case VnResult.VN_GAMEPLAY_WRONG_PROJECT:
                    return "Action " + quoted +
                           " belongs to a different project (" + code + ").";
                case VnResult.VN_GAMEPLAY_CONTRACT_MISMATCH:
                    return "Action " + quoted +
                           " has an incompatible contract (" + code + ").";
                case VnResult.VN_CONTENT_NOT_LOADED:
                    return "Action " + quoted +
                           " is missing content (" + code + ").";
                case VnResult.VN_STALE_HANDLE:
                    return "Action " + quoted +
                           " resolution is stale (" + code + ").";
                case VnResult.VN_PACK_STALE_REVISION:
                case VnResult.VN_PACK_HASH_MISMATCH:
                    return "Action " + quoted +
                           " publication pairing failed (" + code + ").";
                case VnResult.VN_CAPABILITY_UNAVAILABLE:
                    return "Action " + quoted +
                           " name lookup capability is unavailable (" +
                           code + ").";
                case VnResult.VN_INVALID_ARG:
                    return "Action " + quoted +
                           " name is invalid (" + code + ").";
                case VnResult.VN_NO_ACTION:
                case VnResult.VN_BACKPRESSURE:
                case VnResult.VN_NO_VOICE:
                case VnResult.VN_NO_EMITTER:
                case VnResult.VN_TRIGGER_CYCLE:
                    return "Action " + quoted +
                           " was refused by admission (" + code + ").";
                default:
                    return "Action " + quoted + " failed (" + code + ").";
            }
        }

        static byte[] EncodeName(string name, Span<byte> stackBuf)
        {
            if (name == null)
            {
                return null;
            }
            return Utf8.TryEncode(name, stackBuf, out _)
                ? null
                : Encoding.UTF8.GetBytes(name + "\0");
        }
    }
}
