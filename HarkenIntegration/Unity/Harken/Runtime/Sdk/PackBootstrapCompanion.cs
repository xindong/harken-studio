using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace Harken
{
    public sealed partial class PackBootstrapSession
    {
        internal IReadOnlyDictionary<ulong, string> companionLocators;
        private PackSource companionSource;
        private SharedPack[] companionOwners = Array.Empty<SharedPack>();
        private VnCanonicalPackSetMemberV2[] companionKeepMembers = Array.Empty<VnCanonicalPackSetMemberV2>();
        private VnCanonicalPackSetMemberResultV1[] companionKeepResults = Array.Empty<VnCanonicalPackSetMemberResultV1>();
        private Dictionary<string, SharedPack> companionNames = new Dictionary<string, SharedPack>(StringComparer.Ordinal);
        private Dictionary<(ulong Id, ulong Handle), RawCompanionLease> rawCompanionLeases =
            new Dictionary<(ulong, ulong), RawCompanionLease>();

        private readonly struct RawCompanionLease
        {
            internal readonly SharedPack Owner;
            internal readonly int Count;
            internal RawCompanionLease(SharedPack owner, int count) { Owner = owner; Count = count; }
        }

        internal void BindCompanionSource(PackSource source, string project)
        {
            companionSource = source;
            ProjectId = project;
        }

        internal void CompleteCompanionStartup(VnCanonicalPackSetActivationReportV1 report)
        {
            InitPack = new Pack(report.initHandle);
            SetRevision = report.resultSetRevision;
            StartupResult = VnResult.VN_OK;
        }

        /// <summary>
        /// Explicitly refreshes a previously discovered Action, including a rename, and returns
        /// one resource lease. Release that lease when no longer needed, as with PrepareActions.
        /// </summary>
        public VnResult RefreshAction(string name, string projectId, out ActionPreparation preparation)
        {
            preparation = null;
            if (companionSource == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            return PrepareCompanions(new[] { name }, projectId, true, out preparation);
        }

        private VnResult PrepareCompanions(string[] names, string project, bool refresh, out ActionPreparation preparation)
        {
            preparation = null;
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            if (names == null || !Guid.TryParseExact(project, "D", out Guid projectGuid) || projectGuid == Guid.Empty)
                return VnResult.VN_INVALID_ARG;
            if (projectGuid.ToString("D") != ProjectId) return VnResult.VN_GAMEPLAY_WRONG_PROJECT;
            try
            {
                var unique = new HashSet<string>(StringComparer.Ordinal);
                var requested = new List<string>(names.Length);
                foreach (string name in names)
                {
                    if (!PackCompanion.IsName(name) || name == "Init") return VnResult.VN_INVALID_ARG;
                    if (unique.Add(name)) requested.Add(name);
                }
                var candidate = new ActionPreparation(this, requested.ToArray(), refresh);
                VnResult result = candidate.Prepare();
                preparation = candidate;
                return result;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
        }

        private VnResult CompanionMutationAllowed()
        {
            if (!active) return VnResult.VN_NOT_INITIALIZED;
            VnResult thread = RequireOwnerThread();
            if (thread != VnResult.VN_OK) return thread;
            return mutationPhase == MutationPhase.Idle && !inCompanionReadCallback.Value
                ? VnResult.VN_OK : VnResult.VN_IN_USE;
        }

        internal VnResult AcquireCompanions(string[] names, SharedPack[] acquired, bool refresh, ulong expectedId = 0)
            => AcquireCompanions(names, acquired, refresh, expectedId, null);

        private VnResult AcquireCompanions(string[] names, SharedPack[] acquired, bool refresh, ulong expectedId,
            Dictionary<string, CompanionInput> captured, CancellationToken cancellation = default)
        {
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            mutationPhase = MutationPhase.ReadingContent;
            try
            {
                if (captured != null)
                    foreach (CompanionInput input in captured.Values)
                    {
                        VnResult current = companionSource.Validate(input);
                        if (!active) return VnResult.VN_NOT_INITIALIZED;
                        if (current != VnResult.VN_OK) return current;
                    }
                bool warm = !refresh;
                for (int i = 0; warm && i < names.Length; ++i)
                {
                    warm = companionNames.TryGetValue(names[i], out SharedPack known) && known.Handle.IsValid;
                    if (warm)
                    {
                        if (known.Requests == int.MaxValue) return VnResult.VN_BACKPRESSURE;
                        acquired[i] = known;
                    }
                }
                if (warm)
                {
                    cancellation.ThrowIfCancellationRequested();
                    foreach (SharedPack owner in acquired) ++owner.Requests;
                    return VnResult.VN_OK;
                }
                var nextPacks = new Dictionary<ulong, SharedPack>(packs);
                var nextNames = new Dictionary<string, SharedPack>(companionNames, StringComparer.Ordinal);
                var changes = new Dictionary<SharedPack, CompanionInput>();
                VnObjectId project = VnObjectId.Parse(ProjectId);
                for (int i = 0; i < names.Length; ++i)
                {
                    string name = names[i];
                    nextNames.TryGetValue(name, out SharedPack existing);
                    if (existing != null && existing.Handle.IsValid && !refresh)
                    {
                        if (existing.Requests == int.MaxValue) return VnResult.VN_BACKPRESSURE;
                        acquired[i] = existing;
                        continue;
                    }
                    CompanionInput input;
                    VnResult result;
                    if (captured != null && captured.TryGetValue(name, out input))
                        result = companionSource.Validate(input);
                    else result = companionSource.Capture(name, project, initBytes, out input);
                    if (!active) return VnResult.VN_NOT_INITIALIZED;
                    if (result != VnResult.VN_OK) return result;
                    if (expectedId != 0 && input.Facts.packStableId != expectedId) return VnResult.VN_PACK_STALE_REVISION;
                    SharedPack owner = null;
                    foreach (SharedPack known in nextPacks.Values)
                    {
                        VnObjectId root = changes.TryGetValue(known, out CompanionInput changed)
                            ? changed.Names.RootId : known.Names.RootId;
                        if (root == input.Names.RootId) { owner = known; break; }
                    }
                    if (owner == null && refresh) return VnResult.VN_UNKNOWN_ID;
                    if (existing != null && !ReferenceEquals(existing, owner)) return VnResult.VN_PACK_STALE_REVISION;
                    if (owner != null)
                    {
                        string acceptedName = changes.TryGetValue(owner, out CompanionInput changed)
                            ? changed.Names.Name : owner.Names.Name;
                        if (acceptedName != name && !refresh) return VnResult.VN_GAMEPLAY_NAME_RETIRED;
                        if (owner.Requests == int.MaxValue) return VnResult.VN_BACKPRESSURE;
                        if (changes.ContainsKey(owner)) return VnResult.VN_PACK_STALE_REVISION;
                        nextPacks.Remove(owner.StableId);
                        nextNames.Remove(owner.Names.Name);
                    }
                    else owner = new SharedPack();
                    if (nextPacks.TryGetValue(input.Facts.packStableId, out SharedPack collision) &&
                        !ReferenceEquals(collision, owner)) return VnResult.VN_BAD_PACK;
                    nextPacks[input.Facts.packStableId] = owner;
                    nextNames.Add(name, owner);
                    changes.Add(owner, input);
                    acquired[i] = owner;
                }
                if (changes.Count != 0)
                {
                    foreach (CompanionInput input in changes.Values)
                    {
                        VnResult current = companionSource.Validate(input);
                        if (!active) return VnResult.VN_NOT_INITIALIZED;
                        if (current != VnResult.VN_OK) return current;
                    }
                    cancellation.ThrowIfCancellationRequested();
                    VnResult result = CommitCompanions(nextPacks, nextNames, changes);
                    if (result != VnResult.VN_OK) return result;
                }
                // Everything which can allocate or reject preceded native publication.
                foreach (SharedPack owner in acquired) ++owner.Requests;
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            finally { mutationPhase = MutationPhase.Idle; }
        }

        private VnResult CommitCompanions(Dictionary<ulong, SharedPack> nextPacks,
            Dictionary<string, SharedPack> nextNames, Dictionary<SharedPack, CompanionInput> changes)
        {
            var ids = new List<ulong>();
            foreach (var pair in nextPacks)
                if (pair.Value.Handle.IsValid || changes.ContainsKey(pair.Value))
                    ids.Add(pair.Key);
            ids.Sort();
            var members = new CompanionMember[ids.Count + 1];
            var owners = new SharedPack[ids.Count];
            members[0] = new CompanionMember(Inspection.PackStableId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT, InitPack);
            for (int i = 0; i < ids.Count; ++i)
            {
                SharedPack owner = owners[i] = nextPacks[ids[i]];
                Pack expected = ids[i] == owner.StableId ? owner.Handle : default;
                if (changes.TryGetValue(owner, out CompanionInput input))
                {
                    uint operation = expected.IsValid && owner.ContentHash == input.Facts.contentHash &&
                        owner.ByteCount == input.Bytes.Length
                        ? VnConstants.VN_PACK_MEMBER_NAME_ONLY : VnConstants.VN_PACK_MEMBER_CONTENT;
                    members[i + 1] = new CompanionMember(ids[i], VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                        expected, operation, input.Bytes, input.Names);
                }
                else members[i + 1] = new CompanionMember(ids[i], VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT, expected);
            }
            // Reserve release buffers before publication so firing retirement never allocates.
            if (companionKeepMembers.Length < members.Length)
            {
                var keepMembers = new VnCanonicalPackSetMemberV2[members.Length];
                var keepResults = new VnCanonicalPackSetMemberResultV1[members.Length];
                companionKeepMembers = keepMembers;
                companionKeepResults = keepResults;
            }
            mutationPhase = MutationPhase.ApplyingNative;
            VnResult result = CompanionNative.Activate(members, InitPack, SetRevision, out var report, out var rows);
            if (result != VnResult.VN_OK) return result;
            for (int i = 0; i < owners.Length; ++i)
            {
                owners[i].Handle = new Pack(rows[i + 1].handle);
                owners[i].StableId = ids[i];
                if (changes.TryGetValue(owners[i], out CompanionInput input))
                {
                    owners[i].ContentHash = input.Facts.contentHash;
                    owners[i].ByteCount = input.Bytes.Length;
                    owners[i].Names = input.Names;
                }
            }
            companionOwners = owners;
            packs = nextPacks;
            companionNames = nextNames;
            InitPack = new Pack(report.initHandle);
            SetRevision = report.resultSetRevision;
            return VnResult.VN_OK;
        }

        internal unsafe VnResult ReleaseCompanion(SharedPack owner)
        {
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            if (owner == null || !owner.Handle.IsValid || owner.Requests == 0 ||
                !packs.TryGetValue(owner.StableId, out SharedPack current) || !ReferenceEquals(owner, current))
                return VnResult.VN_STALE_HANDLE;
            if (owner.Requests > 1) { --owner.Requests; return VnResult.VN_OK; }
            try
            {
                int count = 0;
                SetKeepMember(count++, Inspection.PackStableId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT, InitPack);
                foreach (SharedPack retained in companionOwners)
                    if (!ReferenceEquals(retained, owner) && retained.Handle.IsValid)
                        SetKeepMember(count++, retained.StableId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT, retained.Handle);
                mutationPhase = MutationPhase.ApplyingNative;
                VnResult result = CompanionNative.ActivatePrepared(companionKeepMembers, companionKeepResults,
                    count, InitPack, SetRevision, out var report);
                if (result != VnResult.VN_OK) return result;
                for (int i = 1; i < count; ++i)
                {
                    ref VnCanonicalPackSetMemberResultV1 row = ref companionKeepResults[i];
                    packs[row.packStableId].Handle = new Pack(row.handle);
                }
                owner.Handle = default;
                --owner.Requests;
                InitPack = new Pack(report.initHandle);
                SetRevision = report.resultSetRevision;
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            finally { mutationPhase = MutationPhase.Idle; }
        }

        private unsafe void SetKeepMember(int index, ulong id, uint kind, Pack expected)
        {
            companionKeepMembers[index] = new VnCanonicalPackSetMemberV2
            {
                structSize = (uint)sizeof(VnCanonicalPackSetMemberV2),
                operation = VnConstants.VN_PACK_MEMBER_KEEP,
                source = new VnCanonicalPackSetMemberV1
                {
                    structSize = (uint)sizeof(VnCanonicalPackSetMemberV1),
                    packStableId = id, packKind = kind, expectedHandle = expected.Handle
                }
            };
            companionKeepResults[index] = new VnCanonicalPackSetMemberResultV1
                { structSize = (uint)sizeof(VnCanonicalPackSetMemberResultV1) };
        }

        private VnResult AcquireCompanionPack(ulong id, out Pack pack)
        {
            pack = default;
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            if (!packs.TryGetValue(id, out SharedPack owner))
            {
                if (companionLocators == null || !companionLocators.TryGetValue(id, out string name))
                    return VnResult.VN_CAPABILITY_UNAVAILABLE;
                try
                {
                    // Reserve bookkeeping before acquiring a reference; no allocation follows publication.
                    var next = new Dictionary<(ulong, ulong), RawCompanionLease>(rawCompanionLeases.Count + 1);
                    foreach (var row in rawCompanionLeases) next.Add(row.Key, row.Value);
                    var acquired = new SharedPack[1];
                    VnResult result = AcquireCompanions(new[] { name }, acquired, false, id);
                    if (result != VnResult.VN_OK) return result;
                    owner = acquired[0];
                    next.Add((id, owner.Handle.Handle), new RawCompanionLease(owner, 1));
                    rawCompanionLeases = next;
                    pack = owner.Handle;
                    return VnResult.VN_OK;
                }
                catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            }
            if (!owner.Handle.IsValid)
            {
                try
                {
                    var nextLeases = new Dictionary<(ulong, ulong), RawCompanionLease>(rawCompanionLeases.Count + 1);
                    foreach (var entry in rawCompanionLeases) nextLeases.Add(entry.Key, entry.Value);
                    var acquired = new SharedPack[1];
                    VnResult result = AcquireCompanions(new[] { owner.Names.Name }, acquired, false, id);
                    if (result != VnResult.VN_OK) return result;
                    nextLeases.Add((id, owner.Handle.Handle), new RawCompanionLease(owner, 1));
                    rawCompanionLeases = nextLeases;
                    pack = owner.Handle;
                    return VnResult.VN_OK;
                }
                catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            }
            if (owner.Requests == int.MaxValue) return VnResult.VN_BACKPRESSURE;
            var key = (id, owner.Handle.Handle);
            rawCompanionLeases.TryGetValue(key, out RawCompanionLease lease);
            try { rawCompanionLeases[key] = new RawCompanionLease(owner, lease.Count + 1); }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            ++owner.Requests;
            pack = owner.Handle;
            return VnResult.VN_OK;
        }

        private VnResult ReleaseCompanionPack(ulong id, Pack pack)
        {
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            var key = (id, pack.Handle);
            if (!rawCompanionLeases.TryGetValue(key, out RawCompanionLease lease)) return VnResult.VN_STALE_HANDLE;
            VnResult result = ReleaseCompanion(lease.Owner);
            if (result == VnResult.VN_OK)
            {
                if (lease.Count == 1) rawCompanionLeases.Remove(key);
                else rawCompanionLeases[key] = new RawCompanionLease(lease.Owner, lease.Count - 1);
            }
            return result;
        }

        private VnResult ReplaceCompanionPack(ulong id, Pack pack)
        {
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            if (!rawCompanionLeases.TryGetValue((id, pack.Handle), out RawCompanionLease lease))
                return VnResult.VN_STALE_HANDLE;
            VnResult result = RefreshAction(lease.Owner.Names.Name, ProjectId, out ActionPreparation preparation);
            if (result == VnResult.VN_OK) result = preparation.Release();
            return result;
        }
    }

    public static partial class PackBootstrap
    {
        /// <summary>Starts from one explicit platform directory containing Init.bytes and Init.json.</summary>
        public static VnResult StartDirectory(string platformDirectory, string language, ref VnConfig config,
            out PackBootstrapSession session)
        {
            session = null;
            if (string.IsNullOrWhiteSpace(platformDirectory) || !PackDirectorySource.ValidLanguage(language))
                return VnResult.VN_INVALID_ARG;
            try
            {
                return Start(PackSource.FromDirectory(platformDirectory, language), ref config, out session);
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (ArgumentException) { return VnResult.VN_INVALID_ARG; }
            catch (IOException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
        }

        /// <summary>Starts an explicitly selected companion source with Init only.</summary>
        public static VnResult Start(PackSource source, ref VnConfig config, out PackBootstrapSession session)
        {
            session = null;
            if (source == null) return VnResult.VN_INVALID_ARG;
            VnResult result = source.Capture(null, default, null, out CompanionInput init);
            return result == VnResult.VN_OK ? StartCaptured(source, init, ref config, out session) : result;
        }

        public static async Task<PackStartupResult> StartAsync(PackSource source, VnConfig config,
            CancellationToken cancellation = default)
        {
            if (source == null || (source.IsAsync && !PackSource.HasControlContext))
                return new PackStartupResult(VnResult.VN_INVALID_ARG);
            int thread = Environment.CurrentManagedThreadId;
            CompanionCapture captured = await source.CaptureAsync(null, default, null, cancellation);
            cancellation.ThrowIfCancellationRequested();
            if (Environment.CurrentManagedThreadId != thread) return new PackStartupResult(VnResult.VN_INVALID_ARG);
            if (captured.Result != VnResult.VN_OK) return new PackStartupResult(captured.Result);
            VnResult result = StartCaptured(source, captured.Input, ref config, out PackBootstrapSession session, cancellation);
            return new PackStartupResult(result, session);
        }

        internal static VnResult StartCaptured(PackSource source, CompanionInput init, ref VnConfig config,
            out PackBootstrapSession session, CancellationToken cancellation = default,
            ulong[] startupIds = null, IReadOnlyDictionary<ulong, string> locators = null)
        {
            session = null;
            try
            {
                VnResult result = PackByteProvider.InspectInitPack(init.Facts.packStableId, PackByteProvider.PreparedBytes,
                    init.Bytes, out InitPackInspection inspection);
                if (result != VnResult.VN_OK) return result;
                result = ApplyRecommendations(ref config, inspection, 0);
                if (result != VnResult.VN_OK) return result;
                var candidate = new PackBootstrapSession(default, startupIds ?? Array.Empty<ulong>(), inspection,
                    source.Root, source.Language, initBytes: init.Bytes);
                candidate.companionLocators = locators;
                candidate.BindCompanionSource(source, init.Facts.projectId.ToString());
                var members = new[] { new CompanionMember(init.Facts.packStableId,
                    VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT, default, VnConstants.VN_PACK_MEMBER_CONTENT, init.Bytes, init.Names) };
                result = source.Validate(init);
                if (result != VnResult.VN_OK) return result;
                cancellation.ThrowIfCancellationRequested();
                result = HarkenEngine.Init(config);
                if (result != VnResult.VN_OK) return result;
                result = CompanionNative.Activate(members, default, 0, out var report, out _);
                if (result != VnResult.VN_OK)
                {
                    VnResult terminated = HarkenEngine.Term();
                    return terminated == VnResult.VN_OK ? result : terminated;
                }
                candidate.CompleteCompanionStartup(report);
                session = candidate;
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (ArgumentException) { return VnResult.VN_INVALID_ARG; }
            catch (IOException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
        }
    }
}
