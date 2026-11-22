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
        internal PackArchive ownedArchive;
        private SharedPack[] companionOwners = Array.Empty<SharedPack>();
        private VnCanonicalPackSetMemberV2[] companionKeepMembers = Array.Empty<VnCanonicalPackSetMemberV2>();
        private VnCanonicalPackSetMemberResultV1[] companionKeepResults = Array.Empty<VnCanonicalPackSetMemberResultV1>();
        private Dictionary<string, SharedPack> companionNames = new Dictionary<string, SharedPack>(StringComparer.Ordinal);
        private GameplayNames.Table initNames;
        internal PackDirectorySource.ResidentSelection initSelection;
        private ulong nameRevision;
        private Dictionary<(ulong Id, ulong Handle), RawCompanionLease> rawCompanionLeases =
            new Dictionary<(ulong, ulong), RawCompanionLease>();

        private readonly struct RawCompanionLease
        {
            internal readonly SharedPack Owner;
            internal readonly int Count;
            internal RawCompanionLease(SharedPack owner, int count) { Owner = owner; Count = count; }
        }

        internal void BindCompanionSource(PackSource source, GameplayNames.Table names)
        {
            companionSource = source;
            ProjectId = names.Project.ToString();
            initNames = names;
        }

        internal void CompleteCompanionStartup(VnCanonicalPackSetActivationReportV1 report, GameplayNames.Table names)
        {
            InitPack = new Pack(report.initHandle);
            SetRevision = report.resultSetRevision;
            StartupResult = VnResult.VN_OK;
            GameplayNames.Publish(names);
        }

        private GameplayNames.Table PrepareNames(Dictionary<string, SharedPack> names,
            Dictionary<SharedPack, CompanionInput> changes, ulong revision)
        {
            var table = GameplayNames.PrepareActions(initNames, names.Count);
            foreach (var row in names)
            {
                SharedPack owner = row.Value;
                GameplayNames.Action action;
                if (changes.TryGetValue(owner, out CompanionInput input))
                    action = new GameplayNames.Action(input.Names.RootId, ++revision,
                        input.Facts.executionFingerprint, owner);
                else
                    action = new GameplayNames.Action(owner.Names.RootId, owner.NameRevision,
                        owner.CallableFingerprint, owner);
                table.Actions.Add(row.Key, action);
            }
            return table;
        }

        /// <summary>
        /// Explicitly refreshes a previously discovered Action, including a rename, and returns
        /// one resource lease. Release that lease when no longer needed, as with PrepareActions.
        /// </summary>
        public VnResult RefreshAction(string name, string projectId, out ActionPreparation preparation)
        {
            preparation = null;
            if (!active) return VnResult.VN_NOT_INITIALIZED;
            if (companionSource == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            return PrepareCompanions(new[] { name }, projectId, true, out preparation);
        }

        private VnResult PrepareCompanions(string[] names, string project, bool refresh, out ActionPreparation preparation)
        {
            preparation = null;
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            VnResult valid = ValidateRequestProject(names, project);
            if (valid != VnResult.VN_OK) return valid;
            try
            {
                string[] requested = UniqueNames(names);
                if (requested == null) return VnResult.VN_INVALID_ARG;
                var candidate = new ActionPreparation(this, requested, refresh);
                VnResult result = candidate.Prepare();
                preparation = candidate;
                return result;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
        }

        private const int SmallBatchLimit = 32;
        private int KnownOwnerCount => packs?.Count ?? companionOwners.Length;

        private bool FindOwner(ulong stableId, out SharedPack owner)
        {
            if (packs != null) return packs.TryGetValue(stableId, out owner);
            foreach (var candidate in companionOwners)
                if (candidate.StableId == stableId) { owner = candidate; return true; }
            owner = null;
            return false;
        }

        private bool FindOwner(string name, out SharedPack owner)
        {
            if (companionNames != null) return companionNames.TryGetValue(name, out owner);
            foreach (var candidate in companionOwners)
                if (candidate.Names.Name == name) { owner = candidate; return true; }
            owner = null;
            return false;
        }

        private void EnsureOwnerIndexes()
        {
            ExpandDirectoryLease();
            if (packs != null) return;
            var byId = new Dictionary<ulong, SharedPack>(companionOwners.Length);
            var byName = new Dictionary<string, SharedPack>(companionOwners.Length, StringComparer.Ordinal);
            foreach (var owner in companionOwners)
            {
                byId.Add(owner.StableId, owner);
                byName.Add(owner.Names.Name, owner);
            }
            packs = byId;
            companionNames = byName;
        }

        private VnResult ValidateRequestProject(string[] names, string project)
        {
            if (names == null) return VnResult.VN_INVALID_ARG;
            if (string.Equals(project, ProjectId, StringComparison.Ordinal)) return VnResult.VN_OK;
            if (!Guid.TryParseExact(project, "D", out Guid id) || id == Guid.Empty) return VnResult.VN_INVALID_ARG;
            return id.ToString("D") == ProjectId ? VnResult.VN_OK : VnResult.VN_GAMEPLAY_WRONG_PROJECT;
        }

        private static string[] UniqueNames(string[] names)
        {
            if (names.Length > SmallBatchLimit) return UniqueLargeRequest(names);
            var result = new string[names.Length];
            int count = 0;
            foreach (string name in names)
            {
                if (!PackCompanion.IsName(name) || name == "Init") return null;
                int index = 0;
                while (index < count && result[index] != name) ++index;
                if (index == count) result[count++] = name;
            }
            if (count != result.Length) Array.Resize(ref result, count);
            return result;
        }

        private static string[] UniqueLargeRequest(string[] names)
        {
            var seen = new HashSet<string>(StringComparer.Ordinal);
            var ordered = new List<string>(names.Length);
            foreach (string name in names)
            {
                if (!PackCompanion.IsName(name) || name == "Init") return null;
                if (seen.Add(name)) ordered.Add(name);
            }
            return ordered.ToArray();
        }

        private VnResult CompanionMutationAllowed()
        {
            if (!active) return VnResult.VN_NOT_INITIALIZED;
            VnResult thread = RequireOwnerThread();
            if (thread != VnResult.VN_OK) return thread;
            return mutationPhase == MutationPhase.Idle && !LanguageSwitchPending && !inCompanionReadCallback.Value
                ? VnResult.VN_OK : VnResult.VN_IN_USE;
        }

        internal VnResult AcquireCompanions(string[] names, SharedPack[] acquired, bool refresh, ulong expectedId = 0)
            => AcquireCompanions(names, acquired, refresh, expectedId, null);

        private VnResult AcquireCompanions(string[] names, SharedPack[] acquired, bool refresh, ulong expectedId,
            Dictionary<string, CompanionInput> captured, CancellationToken cancellation = default)
        {
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            bool resident = captured == null && initSelection != null && Language.Length == 0 && expectedId == 0 &&
                !refresh && names.Length != 0;
            if (resident && HarkenEngine.NativeAbiMinor >= 2 && !directEntryMissing && !cancellation.CanBeCanceled &&
                names.Length <= SmallBatchLimit && (directoryLease == null ? KnownOwnerCount == 0 :
                    directoryLease.Scope == null && directoryLease.Names.Length == names.Length))
            {
                var direct = ActivateSmallResidentSet(names, acquired);
                if (direct != VnResult.VN_CAPABILITY_UNAVAILABLE) return direct;
            }
            try { ExpandDirectoryLease(); }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            if (resident && (KnownOwnerCount == 0 || KnownOwnerCount == names.Length))
            {
                bool empty = true;
                foreach (SharedPack owner in companionOwners) if (owner.Handle.IsValid) { empty = false; break; }
                if (empty)
                {
                    VnResult batch = VisitBatch(names, acquired, cancellation, out bool consumed);
                    // ABI 1.1 reports BAD_PACK for an HRP2 envelope before visiting its contents.
                    // The managed reader validates the whole set again under its publication guards.
                    bool legacyReadFailure = !consumed && batch == VnResult.VN_BAD_PACK &&
                        HarkenEngine.NativeAbiMinor < 2 && HasBlockEnvelope(names);
                    if (!legacyReadFailure && (consumed || batch != VnResult.VN_CAPABILITY_UNAVAILABLE)) return batch;
                }
            }
            return AcquireCompanionInputs(names, acquired, refresh, expectedId, captured, cancellation);
        }

        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
        private VnResult AcquireCompanionInputs(string[] names, SharedPack[] acquired, bool refresh, ulong expectedId,
            Dictionary<string, CompanionInput> captured, CancellationToken cancellation)
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
                    warm = FindOwner(names[i], out SharedPack known) && known.Handle.IsValid;
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
                EnsureOwnerIndexes();
                var nextPacks = new Dictionary<ulong, SharedPack>(packs);
                var nextNames = new Dictionary<string, SharedPack>(companionNames, StringComparer.Ordinal);
                var changes = new Dictionary<SharedPack, CompanionInput>();
                VnObjectId project = initNames.Project;
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
                    VnResult locked = companionSource.LockPublication(initSelection, changes.Values, out var guards);
                    if (locked != VnResult.VN_OK) return locked;
                    using var held = guards;
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
                        owner.ByteCount == input.ByteCount
                        ? VnConstants.VN_PACK_MEMBER_KEEP : VnConstants.VN_PACK_MEMBER_CONTENT;
                    members[i + 1] = new CompanionMember(ids[i], VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                        expected, operation, input.Bytes, input.Names);
                }
                else members[i + 1] = new CompanionMember(ids[i], VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT, expected);
            }
            ReserveKeepBuffers(members.Length);
            GameplayNames.Table names = PrepareNames(nextNames, changes, nameRevision);
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
                    owners[i].ByteCount = input.ByteCount;
                    owners[i].Names = input.Names;
                }
            }
            foreach (var action in names.Actions.Values)
                if (changes.ContainsKey(action.Owner))
                {
                    action.Owner.NameRevision = action.Revision;
                    action.Owner.CallableFingerprint = action.Fingerprint;
                    nameRevision = Math.Max(nameRevision, action.Revision);
                }
            GameplayNames.Publish(names);
            companionOwners = owners;
            packs = nextPacks;
            companionNames = nextNames;
            InitPack = new Pack(report.initHandle);
            SetRevision = report.resultSetRevision;
            return VnResult.VN_OK;
        }

        private void ReserveKeepBuffers(int count)
        {
            if (companionKeepMembers.Length >= count) return;
            var members = new VnCanonicalPackSetMemberV2[count];
            var results = new VnCanonicalPackSetMemberResultV1[count];
            companionKeepMembers = members;
            companionKeepResults = results;
        }

        internal bool TryReleaseWholeSet(SharedPack[] owners, int count)
        {
            if (count != companionOwners.Length || CompanionMutationAllowed() != VnResult.VN_OK) return false;
            // A request is counted for every acquired occurrence. A count of one rules out aliases.
            for (int i = 0; i < count; ++i)
            {
                var owner = owners[i];
                if (owner == null || owner.Requests != 1 || !owner.Handle.IsValid ||
                    !FindOwner(owner.StableId, out var current) || !ReferenceEquals(owner, current))
                    return false;
            }
            try
            {
                SetKeepMember(0, Inspection.PackStableId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT, InitPack);
                mutationPhase = MutationPhase.ApplyingNative;
                if (CompanionNative.ActivatePrepared(companionKeepMembers, companionKeepResults, 1, InitPack,
                    SetRevision, out var report) != VnResult.VN_OK) return false;
                for (int i = 0; i < count; ++i)
                {
                    owners[i].Handle = default;
                    owners[i].Requests = 0;
                }
                InitPack = new Pack(report.initHandle);
                SetRevision = report.resultSetRevision;
                return true;
            }
            catch (OutOfMemoryException) { return false; }
            finally { mutationPhase = MutationPhase.Idle; }
        }

        internal unsafe VnResult ReleaseCompanion(SharedPack owner)
        {
            VnResult allowed = CompanionMutationAllowed();
            if (allowed != VnResult.VN_OK) return allowed;
            if (owner == null || !owner.Handle.IsValid || owner.Requests == 0 ||
                !FindOwner(owner.StableId, out SharedPack current) || !ReferenceEquals(owner, current))
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
                    FindOwner(row.packStableId, out var retained);
                    retained.Handle = new Pack(row.handle);
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
            try { ExpandDirectoryLease(); }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            if (!FindOwner(id, out SharedPack owner))
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
            => StartDirectory(platformDirectory, language, ref config, out session, 0);

        internal static VnResult StartDirectory(string platformDirectory, string language, ref VnConfig config,
            out PackBootstrapSession session, ulong outputTicket)
        {
            session = null;
            if (string.IsNullOrWhiteSpace(platformDirectory) || !PackDirectorySource.ValidLanguage(language))
                return VnResult.VN_INVALID_ARG;
            try
            {
                return StartLocal(PackSource.FromDirectory(platformDirectory, language), ref config,
                    out session, CancellationToken.None, outputTicket);
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (ArgumentException) { return VnResult.VN_INVALID_ARG; }
            catch (IOException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
        }

        /// <summary>Starts from a published platform ZIP. The session owns the open archive.</summary>
        public static VnResult StartArchive(string archivePath, string language, ref VnConfig config,
            out PackBootstrapSession session)
            => StartArchive(archivePath, language, ref config, out session, CancellationToken.None);

        private static VnResult StartArchive(string archivePath, string language, ref VnConfig config,
            out PackBootstrapSession session, CancellationToken cancellation, ulong outputTicket = 0)
        {
            session = null;
            cancellation.ThrowIfCancellationRequested();
            if (string.IsNullOrWhiteSpace(archivePath) || !PackDirectorySource.ValidLanguage(language))
                return VnResult.VN_INVALID_ARG;
            PackArchive archive = null;
            try
            {
                archive = new PackArchive(archivePath);
                VnResult result = CaptureAndStart(PackSource.FromArchive(archive, language), ref config,
                    out session, cancellation, outputTicket);
                if (session != null)
                {
                    session.ownedArchive = archive;
                    archive = null;
                }
                return result;
            }
            catch (InvalidDataException) { return VnResult.VN_BAD_PACK; }
            catch (FileNotFoundException) { return VnResult.VN_CONTENT_NOT_LOADED; }
            catch (DirectoryNotFoundException) { return VnResult.VN_CONTENT_NOT_LOADED; }
            catch (UnauthorizedAccessException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
            catch (IOException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (ArgumentException) { return VnResult.VN_INVALID_ARG; }
            finally { archive?.Dispose(); }
        }

        /// <summary>Starts an explicitly selected companion source with Init only.</summary>
        public static VnResult Start(PackSource source, ref VnConfig config, out PackBootstrapSession session)
        {
            session = null;
            if (source == null) return VnResult.VN_INVALID_ARG;
            return source.IsDirectory ? StartLocal(source, ref config, out session, CancellationToken.None)
                : CaptureAndStart(source, ref config, out session, CancellationToken.None);
        }

        internal static VnResult StartLocal(PackSource source, ref VnConfig config,
            out PackBootstrapSession session, CancellationToken cancellation, ulong outputTicket = 0)
        {
            session = null;
            cancellation.ThrowIfCancellationRequested();
            ulong ownedTicket = outputTicket == 0 && config.deviceMode == (uint)VnDeviceMode.VN_DEVICE_REAL
                ? HarkenEngine.BeginOutputPreparation(config) : 0;
            try
            {
                if (ownedTicket != 0) outputTicket = ownedTicket;
                string archive = Path.Combine(source.Root, PackArchive.FileName);
                if (File.Exists(archive) && !File.Exists(Path.Combine(source.Root, "Init.hrp")) && !File.Exists(Path.Combine(source.Root, "Init.bytes")) &&
                    !File.Exists(Path.Combine(source.Root, "Init.json")))
                {
                    if (File.Exists(Path.Combine(source.Root, ".harken-pack-publishing")))
                        return VnResult.VN_PACK_STALE_REVISION;
                    return StartArchive(archive, source.Language, ref config, out session, cancellation, outputTicket);
                }
                return CaptureAndStart(source, ref config, out session, cancellation, outputTicket);
            }
            finally { HarkenEngine.CancelOutputPreparation(ownedTicket); }
        }

        private static VnResult CaptureAndStart(PackSource source, ref VnConfig config,
            out PackBootstrapSession session, CancellationToken cancellation, ulong outputTicket = 0)
        {
            session = null;
            VnResult result = source.Capture(null, default, null, out CompanionInput init);
            cancellation.ThrowIfCancellationRequested();
            return result == VnResult.VN_OK ? StartCaptured(source, init, ref config, out session,
                cancellation, outputTicket: outputTicket) : result;
        }

        public static async Task<PackStartupResult> StartAsync(PackSource source, VnConfig config,
            CancellationToken cancellation = default)
        {
            if (source == null || (source.IsAsync && !PackSource.HasControlContext))
                return new PackStartupResult(VnResult.VN_INVALID_ARG);
            if (source.IsDirectory)
            {
                return await StartDirectoryLevelAsync(source, config, null, cancellation, null);
            }
            int thread = Environment.CurrentManagedThreadId;
            CompanionCapture captured = await source.CaptureAsync(null, default, null, cancellation);
            cancellation.ThrowIfCancellationRequested();
            if (Environment.CurrentManagedThreadId != thread) return new PackStartupResult(VnResult.VN_INVALID_ARG);
            if (captured.Result != VnResult.VN_OK) return new PackStartupResult(captured.Result);
            VnResult result = StartCaptured(source, captured.Input, ref config, out PackBootstrapSession session, cancellation);
            return new PackStartupResult(result, session);
        }

        /// <summary>Opens output and prepares the first level. On success the session can play
        /// immediately. Release Preparation when the level unloads. An optional onPrepared
        /// callback can set up emitters and post the opening cue before the output is ready.
        /// The session is borrowed during this callback; a failed callback rolls back startup.</summary>
        public static Task<PackStartupResult> StartLevelAsync(PackSource source, VnConfig config,
            string[] actionNames, CancellationToken cancellation = default,
            Func<PackBootstrapSession, VnResult> onPrepared = null)
        {
            if (source == null || !source.IsDirectory || actionNames == null || actionNames.Length == 0)
                return Task.FromResult(new PackStartupResult(VnResult.VN_INVALID_ARG));
            return StartDirectoryLevelAsync(source, config, actionNames, cancellation, onPrepared);
        }

        private static async Task<PackStartupResult> StartDirectoryLevelAsync(PackSource source, VnConfig config,
            string[] names, CancellationToken cancellation, Func<PackBootstrapSession, VnResult> onPrepared)
        {
            cancellation.ThrowIfCancellationRequested();
            bool background = config.deviceMode == (uint)VnDeviceMode.VN_DEVICE_REAL_BACKGROUND;
#if UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
            if (config.deviceMode == (uint)VnDeviceMode.VN_DEVICE_REAL && PackSource.HasControlContext)
            {
                if (HarkenEngine.TryGetAbi(out var abi) && abi.minor >= 4)
                {
                    config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_REAL_BACKGROUND;
                    background = true;
                }
            }
#endif
            VnResult result = StartLocal(source, ref config, out var session, cancellation);
            if (result != VnResult.VN_OK) return new PackStartupResult(result);
            ActionPreparation preparation = null;
            try
            {
                if (names != null) result = session.PrepareActions(names, session.ProjectId, out preparation);
                if (result == VnResult.VN_OK && onPrepared != null)
                {
                    cancellation.ThrowIfCancellationRequested();
                    result = onPrepared(session);
                    if (!session.IsActive) return new PackStartupResult(VnResult.VN_NOT_INITIALIZED);
                    if (result == VnResult.VN_OK) result = HarkenEngine.Update();
                    if (!session.IsActive) return new PackStartupResult(VnResult.VN_NOT_INITIALIZED);
                }
                if (result == VnResult.VN_OK && background)
                {
                    while ((result = HarkenEngine.Resume()) == VnResult.VN_IN_USE)
                    {
                        if (PackSource.HasControlContext) await Task.Yield();
                        else Thread.Sleep(1);
                        cancellation.ThrowIfCancellationRequested();
                    }
                }
                cancellation.ThrowIfCancellationRequested();
                if (result != VnResult.VN_OK) return new PackStartupResult(result);
                var ready = new PackStartupResult(result, session, preparation);
                session = null;
                return ready;
            }
            finally
            {
                if (session != null) { preparation?.Release(); session.Shutdown(); }
            }
        }

        internal static VnResult StartCaptured(PackSource source, CompanionInput init, ref VnConfig config,
            out PackBootstrapSession session, CancellationToken cancellation = default,
            ulong[] startupIds = null, IReadOnlyDictionary<ulong, string> locators = null, ulong outputTicket = 0)
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
                candidate.initSelection = init.Selection;
                candidate.inputBudget = config.maxPackDecodeStagingBytes;
                GameplayNames.Table names = GameplayNames.PrepareInit(init.Facts.projectId, init.Names);
                candidate.BindCompanionSource(source, names);
                var members = new[] { new CompanionMember(init.Facts.packStableId,
                    VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT, default, VnConstants.VN_PACK_MEMBER_CONTENT, init.Bytes, init.Names) };
                result = source.Validate(init);
                if (result != VnResult.VN_OK) return result;
                cancellation.ThrowIfCancellationRequested();
                result = source.LockPublication(init.Selection, null, out var guards);
                if (result != VnResult.VN_OK) return result;
                using var initGuard = guards;
                result = HarkenEngine.Init(config, outputTicket);
                if (result != VnResult.VN_OK) return result;
                result = CompanionNative.Activate(members, default, 0, out var report, out _);
                if (result != VnResult.VN_OK)
                {
                    VnResult terminated = HarkenEngine.Term();
                    return terminated == VnResult.VN_OK ? result : terminated;
                }
                candidate.CompleteCompanionStartup(report, names);
                session = candidate;
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (ArgumentException) { return VnResult.VN_INVALID_ARG; }
            catch (IOException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
        }
    }
}
