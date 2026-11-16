using System;
using System.Runtime.InteropServices;

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
        readonly VnObjectId actionId;
        readonly ulong revision, fingerprint, generation;

        internal ActionResolved(VnObjectId actionId, ulong revision, ulong fingerprint, ulong generation)
        {
            this.actionId = actionId;
            this.revision = revision;
            this.fingerprint = fingerprint;
            this.generation = generation;
        }

        public VnObjectId ActionId => actionId;
        public ulong NameBindingRevision => revision;
        public ulong CallableFingerprint => fingerprint;
        public ulong PublicationGeneration => generation;
    }

    public static class Actions
    {
        private static readonly uint PostDescSize =
            (uint)Marshal.SizeOf<VnPostActionDesc>();

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
            VnResult result = Resolve(actionName, default(VnObjectId), out ActionResolved resolved);
            return result == VnResult.VN_OK
                ? Post(resolved.ActionId, emitter, callerSeed, out firing) : result;
        }

        public static unsafe VnResult Post(in ActionReference pinned,
                                           Emitter emitter, ulong callerSeed,
                                    out ActionFiring firing)
        {
            firing = default;
            if (!PackCompanion.IsName(pinned.ExpectedName) ||
                pinned.ProjectGuid == null || pinned.ProjectGuid.Length != 16 || pinned.ActionId.IsNull)
            {
                return VnResult.VN_INVALID_ARG;
            }
            ulong high = 0, low = 0;
            for (int i = 0; i < 8; ++i)
            {
                high = (high << 8) | pinned.ProjectGuid[i];
                low = (low << 8) | pinned.ProjectGuid[i + 8];
            }
            VnResult result = GameplayNames.FindAction(pinned.ExpectedName, new VnObjectId(high, low),
                out GameplayNames.Action action);
            if (result == VnResult.VN_UNKNOWN_ID &&
                GameplayNames.FindAction(pinned.ActionId, out _) == VnResult.VN_OK)
                return VnResult.VN_GAMEPLAY_NAME_RETIRED;
            if (result != VnResult.VN_OK) return result;
            if (action.Id != pinned.ActionId) return VnResult.VN_GAMEPLAY_NAME_RETIRED;
            if (pinned.NameBindingRevision != 0 && pinned.NameBindingRevision != action.Revision)
                return VnResult.VN_STALE_HANDLE;
            if (pinned.CallableFingerprint != 0 && pinned.CallableFingerprint != action.Fingerprint)
                return VnResult.VN_GAMEPLAY_CONTRACT_MISMATCH;
            return Post(action.Id, emitter, callerSeed, out firing);
        }

        public static unsafe VnResult Resolve(string actionName,
                                              out ActionResolved resolved)
            => Resolve(actionName, default(VnObjectId), out resolved);

        public static VnResult Resolve(string actionName, string projectId,
                                       out ActionResolved resolved)
        {
            resolved = default;
            if (!Guid.TryParseExact(projectId, "D", out Guid project) ||
                project == Guid.Empty)
                return VnResult.VN_INVALID_ARG;
            return Resolve(actionName, VnObjectId.FromGuid(project), out resolved);
        }

        private static VnResult Resolve(string actionName, VnObjectId project,
                                        out ActionResolved resolved)
        {
            resolved = default;
            if (!PackCompanion.IsName(actionName))
                return VnResult.VN_INVALID_ARG;
            VnResult result = GameplayNames.FindAction(actionName, project, out GameplayNames.Action action);
            if (result == VnResult.VN_OK)
                resolved = new ActionResolved(action.Id, action.Revision, action.Fingerprint,
                    GameplayNames.Generation);
            return result;
        }

        public static VnResult Post(in ActionResolved resolved, Emitter emitter,
                                    ulong callerSeed, out ActionFiring firing)
        {
            firing = default;
            if (resolved.ActionId.IsNull) return VnResult.VN_INVALID_ARG;
            VnResult result = GameplayNames.FindAction(resolved.ActionId, out GameplayNames.Action action);
            if (result != VnResult.VN_OK) return result;
            if (resolved.PublicationGeneration != GameplayNames.Generation)
                return VnResult.VN_STALE_HANDLE;
            if (action.Revision != resolved.NameBindingRevision ||
                action.Fingerprint != resolved.CallableFingerprint)
                return VnResult.VN_GAMEPLAY_CONTRACT_MISMATCH;
            return Post(action.Id, emitter, callerSeed, out firing);
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
    }
}
