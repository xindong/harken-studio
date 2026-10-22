using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;

namespace Harken
{
    public sealed class ActionPreparation
    {
        private readonly PackBootstrapSession owner;
        private readonly ulong[] ids;
        private readonly Pack[] packs;
        private readonly string[] names;
        private readonly PackBootstrapSession.SharedPack[] owners;
        private bool refresh;
        private int count;
        private bool prepared;
        private bool preparing;

        internal ActionPreparation(PackBootstrapSession owner, ulong[] ids)
        {
            this.owner = owner;
            this.ids = ids;
            packs = new Pack[ids.Length];
        }

        internal ActionPreparation(PackBootstrapSession owner, string[] names, bool refresh = false)
        {
            this.owner = owner;
            this.names = names;
            owners = new PackBootstrapSession.SharedPack[names.Length];
            this.refresh = refresh;
        }

        public bool IsPrepared => prepared && owner.IsActive;
        public bool IsReleased => !owner.IsActive || count == 0;
        public VnResult LastReleaseResult { get; private set; } = VnResult.VN_OK;

        /// <summary>Reacquires a released scope using its original session and dependency plan.</summary>
        public VnResult Prepare()
        {
            if (!owner.IsActive) return VnResult.VN_NOT_INITIALIZED;
            if (preparing) return VnResult.VN_IN_USE;
            if (prepared) return VnResult.VN_OK;
            if (count != 0) return VnResult.VN_IN_USE;
            preparing = true;
            try
            {
                if (names != null)
                {
                    VnResult result = owner.AcquireCompanions(names, owners, refresh);
                    if (result == VnResult.VN_OK)
                    {
                        count = owners.Length;
                        prepared = true;
                        refresh = false;
                    }
                    return result;
                }
                while (count < ids.Length)
                {
                    VnResult result = owner.AcquirePack(ids[count], out Pack pack);
                    if (result != VnResult.VN_OK)
                    {
                        ReleaseCore();
                        return result;
                    }
                    packs[count++] = pack;
                }
                prepared = true;
                return VnResult.VN_OK;
            }
            finally { preparing = false; }
        }

        /// <summary>Releases this scope's references. Retain the scope and retry if unloading fails.</summary>
        public VnResult Release()
        {
            if (preparing) return VnResult.VN_IN_USE;
            return ReleaseCore();
        }

        private VnResult ReleaseCore()
        {
            prepared = false;
            if (!owner.IsActive) count = 0;
            while (count > 0)
            {
                int index = count - 1;
                VnResult result = owners == null ? owner.ReleasePack(ids[index], packs[index])
                    : owner.ReleaseCompanion(owners[index]);
                // Keep dependencies alive while their dependent Pack still belongs to this scope.
                if (result != VnResult.VN_OK) return LastReleaseResult = result;
                if (owners == null) packs[index] = default;
                else owners[index] = null;
                --count;
            }
            return LastReleaseResult = VnResult.VN_OK;
        }
    }

    public sealed partial class PackBootstrapSession : IDisposable
    {
        private bool active;
        private enum MutationPhase { Idle, ReadingContent, ApplyingNative }
        private MutationPhase mutationPhase;
        private readonly int ownerThreadId;
        private Dictionary<ulong, SharedPack> packs = new Dictionary<ulong, SharedPack>();
        private readonly HashSet<ulong> acquiringPacks = new HashSet<ulong>();
        private readonly ulong[] startupIds;
        private readonly Pack[] startupPacks;
        private readonly PackByteProvider.Handler contentProvider;
        private readonly object providerState;
        private readonly byte[] initBytes;
        private readonly Dictionary<ulong, byte[]> contentBytes = new Dictionary<ulong, byte[]>();

        internal sealed class SharedPack
        {
            internal ulong StableId;
            internal PackCompanion Names;
            public Pack Handle;
            public int Requests;
            public string Path;
            public byte[] Bytes;
        }

        internal PackBootstrapSession(
            Pack initPack, ulong[] startupIds, InitPackInspection inspection,
            string containmentRoot, string language, GameplayPublication publication = null,
            PackByteProvider.Handler contentProvider = null, object providerState = null,
            byte[] initBytes = null)
        {
            InitPack = initPack;
            this.startupIds = (ulong[])startupIds.Clone();
            startupPacks = new Pack[startupIds.Length];
            Inspection = inspection;
            ContainmentRoot = containmentRoot;
            Language = language;
            Publication = publication;
            ProjectId = publication?.ProjectId;
            this.contentProvider = contentProvider;
            this.providerState = providerState;
            this.initBytes = initBytes;
            ownerThreadId = Environment.CurrentManagedThreadId;
            active = true;
        }

        internal ulong SetRevision;
        public Pack InitPack { get; private set; }
        public Pack[] StartupPacks => (Pack[])startupPacks.Clone();
        public InitPackInspection Inspection { get; }
        public bool IsActive => active;
        public string ContainmentRoot { get; }
        public string Language { get; }
        public GameplayPublication Publication { get; }
        /// <summary>The actual project identified by the session's validated Init Pack.</summary>
        public string ProjectId { get; private set; }
        public VnResult StartupResult { get; private set; } = VnResult.VN_NOT_INITIALIZED;
        public ulong FailedStartupPackStableId { get; private set; }
        public bool IsStartupComplete
        {
            get
            {
                if (!active || StartupResult != VnResult.VN_OK) return false;
                foreach (Pack pack in startupPacks)
                    if (!pack.IsValid) return false;
                return true;
            }
        }

        private VnResult RequireOwnerThread()
        {
            return Environment.CurrentManagedThreadId == ownerThreadId
                ? VnResult.VN_OK
                : VnResult.VN_INVALID_ARG;
        }

        internal void BindStartupPack(int index, Pack pack, string path, byte[] bytes)
        {
            if (index < 0 || index >= startupPacks.Length) return;
            startupPacks[index] = pack;
            if (!pack.IsValid || startupIds[index] == 0) return;
            packs[startupIds[index]] = new SharedPack
            {
                Handle = pack,
                Requests = 1,
                Path = path,
                Bytes = bytes
            };
            if (bytes != null) contentBytes[startupIds[index]] = bytes;
        }

        public VnResult AcquirePack(ulong packStableId, out Pack pack)
        {
            if (companionSource != null) return AcquireCompanionPack(packStableId, out pack);
            pack = default;
            if (!active) return VnResult.VN_NOT_INITIALIZED;
            VnResult owner = RequireOwnerThread();
            if (owner != VnResult.VN_OK) return owner;
            if (packStableId == 0 || packStableId == Inspection.PackStableId)
                return VnResult.VN_INVALID_ARG;
            if (packs.TryGetValue(packStableId, out SharedPack shared))
            {
                if (shared.Requests == int.MaxValue) return VnResult.VN_BACKPRESSURE;
                ++shared.Requests;
                pack = shared.Handle;
                return VnResult.VN_OK;
            }
            if (mutationPhase != MutationPhase.Idle || !acquiringPacks.Add(packStableId)) return VnResult.VN_IN_USE;
            mutationPhase = MutationPhase.ReadingContent;
            try
            {
                VnResult prepared = PrepareContentSource(packStableId, out string path, out byte[] bytes);
                if (prepared != VnResult.VN_OK) return prepared;
                if (!active) return VnResult.VN_NOT_INITIALIZED;
                mutationPhase = MutationPhase.ApplyingNative;
                CanonicalPackMember[] members;
                VnResult built = BuildReplacementMembers(packStableId, path, bytes, out members);
                if (built != VnResult.VN_OK) return built;
                VnResult result = Packs.ActivateCanonicalPackSet(
                    members, SetRevision + 1, out var report, out var rows, InitPack, SetRevision);
                if (result != VnResult.VN_OK) return result;
                ApplyActivation(report, rows);
                if (!packs.TryGetValue(packStableId, out shared)) return VnResult.VN_UNKNOWN_ID;
                shared.Path = path;
                shared.Bytes = bytes;
                shared.Requests = 1;
                if (bytes != null) contentBytes[packStableId] = bytes;
                pack = shared.Handle;
                return VnResult.VN_OK;
            }
            finally
            {
                mutationPhase = MutationPhase.Idle;
                acquiringPacks.Remove(packStableId);
            }
        }

        /// <summary>On failure, a non-null scope still owns any references whose rollback could not finish.</summary>
        public VnResult PrepareActions(string[] names, string projectId, out ActionPreparation preparation)
        {
            if (companionSource != null) return PrepareCompanions(names, projectId, false, out preparation);
            preparation = null;
            if (!active) return VnResult.VN_NOT_INITIALIZED;
            VnResult owner = RequireOwnerThread();
            if (owner != VnResult.VN_OK) return owner;
            if (Publication == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            VnResult result = Publication.GetActionPreparationOrder(names, projectId, out ulong[] ids);
            if (result != VnResult.VN_OK) return result;
            var candidate = new ActionPreparation(this, ids);
            result = candidate.Prepare();
            preparation = candidate;
            return result;
        }

        private VnResult PrepareContentSource(ulong packStableId, out string path, out byte[] bytes)
        {
            path = null;
            bytes = null;
            if (Publication != null)
            {
                if (!Publication.TryFindPack(packStableId, out PackManifestEntry entry)) return VnResult.VN_UNKNOWN_ID;
                if (contentProvider != null)
                {
                    try
                    {
                        VnResult result = contentProvider(packStableId, entry.Path, entry.VariantKey,
                            entry.ContentHash, out bytes, providerState);
                        if (!active) return VnResult.VN_NOT_INITIALIZED;
                        if (result != VnResult.VN_OK) return result;
                        if (bytes == null || bytes.Length == 0) return VnResult.VN_CONTENT_NOT_LOADED;
                        bytes = (byte[])bytes.Clone();
                    }
                    catch { return VnResult.VN_PACK_PROVIDER_FAILED; }
                }
                else
                {
                    if (!PackManifest.TryJoinContainedPath(ContainmentRoot, entry.Path, out path))
                        return VnResult.VN_PACK_PATH_ESCAPE;
                    try { bytes = File.ReadAllBytes(path); }
                    catch (FileNotFoundException) { return VnResult.VN_CONTENT_NOT_LOADED; }
                    catch (DirectoryNotFoundException) { return VnResult.VN_CONTENT_NOT_LOADED; }
                    catch (IOException) { return VnResult.VN_BAD_PACK; }
                    catch (UnauthorizedAccessException) { return VnResult.VN_BAD_PACK; }
                }
                if (!PackManifest.VerifyPackBytes(bytes, entry)) return VnResult.VN_PACK_HASH_MISMATCH;
                if (contentProvider != null) path = null;
                return VnResult.VN_OK;
            }
            return PackBootstrap.ReadContentFile(ContainmentRoot, Language, packStableId, out path, out bytes);
        }

        private VnResult BuildReplacementMembers(ulong addId, string addPath, byte[] addBytes,
            out CanonicalPackMember[] members, ulong removeId = 0)
        {
            members = null;
            var built = new List<CanonicalPackMember>(packs.Count + 2)
            {
                MakeInitMember()
            };
            var retained = new List<ulong>(packs.Count + 1);
            foreach (var pair in packs)
            {
                if (pair.Key == removeId) continue;
                retained.Add(pair.Key);
            }
            if (addId != 0 && !retained.Contains(addId)) retained.Add(addId);
            retained.Sort();
            for (int i = 0; i < retained.Count; ++i)
            {
                ulong id = retained[i];
                if (id == addId)
                {
                    Pack expected = default;
                    if (packs.TryGetValue(id, out SharedPack existingAdd))
                        expected = existingAdd.Handle;
                    built.Add(MakeContentMember(id, addPath, addBytes, expected));
                    continue;
                }
                SharedPack existing = packs[id];
                built.Add(MakeContentMember(id, existing.Path, existing.Bytes, existing.Handle));
            }
            members = built.ToArray();
            return VnResult.VN_OK;
        }

        private CanonicalPackMember MakeInitMember()
        {
            if (initBytes != null)
            {
                return new CanonicalPackMember(Inspection.PackStableId,
                    VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT,
                    provider: PackByteProvider.PreparedBytes, providerState: initBytes,
                    expected: InitPack);
            }
            string initPath = null;
            PackManifest.TryResolvePackPath(ContainmentRoot, Inspection.PackStableId, Language, out initPath);
            return new CanonicalPackMember(Inspection.PackStableId,
                VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT, initPath, ContainmentRoot,
                expected: InitPack);
        }

        private CanonicalPackMember MakeContentMember(ulong id, string path, byte[] bytes, Pack expected)
        {
            if (bytes != null)
            {
                return new CanonicalPackMember(id, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                    provider: PackByteProvider.PreparedBytes, providerState: bytes, expected: expected);
            }
            return new CanonicalPackMember(id, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                path, ContainmentRoot, expected: expected);
        }

        private void ApplyActivation(VnCanonicalPackSetActivationReportV1 report,
            VnCanonicalPackSetMemberResultV1[] rows)
        {
            InitPack = new Pack(report.initHandle);
            SetRevision = report.resultSetRevision;
            if (rows == null) return;
            for (int i = 0; i < rows.Length; ++i)
            {
                ulong id = rows[i].packStableId;
                if (id == 0 || id == Inspection.PackStableId) continue;
                if (!packs.TryGetValue(id, out SharedPack shared))
                {
                    shared = new SharedPack();
                    packs[id] = shared;
                }
                shared.Handle = new Pack(rows[i].handle);
            }
        }

        public VnResult ReleasePack(ulong packStableId, Pack pack)
        {
            if (companionSource != null) return ReleaseCompanionPack(packStableId, pack);
            if (!active) return VnResult.VN_NOT_INITIALIZED;
            VnResult owner = RequireOwnerThread();
            if (owner != VnResult.VN_OK) return owner;
            if (!packs.TryGetValue(packStableId, out SharedPack shared) ||
                !pack.IsValid || shared.Handle.Handle != pack.Handle)
                return VnResult.VN_STALE_HANDLE;
            if (shared.Requests > 1)
            {
                --shared.Requests;
                return VnResult.VN_OK;
            }
            if (mutationPhase != MutationPhase.Idle) return VnResult.VN_IN_USE;
            if (Publication != null)
            {
                foreach (var retained in packs)
                {
                    if (retained.Key == packStableId ||
                        !Publication.TryFindPack(retained.Key, out PackManifestEntry entry) ||
                        entry.Dependencies == null) continue;
                    foreach (string dependency in entry.Dependencies)
                    {
                        if (ulong.TryParse(dependency, NumberStyles.None, CultureInfo.InvariantCulture,
                            out ulong dependencyId) && dependencyId == packStableId)
                            return VnResult.VN_IN_USE;
                    }
                }
            }
            mutationPhase = MutationPhase.ApplyingNative;
            try
            {
                CanonicalPackMember[] members;
                VnResult built = BuildReplacementMembers(0, null, null, out members, packStableId);
                if (built != VnResult.VN_OK) return built;
                VnResult result = Packs.ActivateCanonicalPackSet(
                    members, SetRevision + 1, out var report, out var rows, InitPack, SetRevision);
                if (result != VnResult.VN_OK) return result;
                packs.Remove(packStableId);
                contentBytes.Remove(packStableId);
                ApplyActivation(report, rows);
                return VnResult.VN_OK;
            }
            finally { mutationPhase = MutationPhase.Idle; }
        }

        public VnResult ReplacePack(ulong packStableId, Pack pack)
        {
            if (companionSource != null) return ReplaceCompanionPack(packStableId, pack);
            if (!active) return VnResult.VN_NOT_INITIALIZED;
            VnResult owner = RequireOwnerThread();
            if (owner != VnResult.VN_OK) return owner;
            if (!packs.TryGetValue(packStableId, out SharedPack shared) ||
                !pack.IsValid || shared.Handle.Handle != pack.Handle)
                return VnResult.VN_STALE_HANDLE;
            if (mutationPhase != MutationPhase.Idle) return VnResult.VN_IN_USE;
            mutationPhase = MutationPhase.ReadingContent;
            try
            {
                VnResult prepared = PrepareContentSource(packStableId, out string path, out byte[] bytes);
                if (prepared != VnResult.VN_OK) return prepared;
                if (!active) return VnResult.VN_NOT_INITIALIZED;
                mutationPhase = MutationPhase.ApplyingNative;
                CanonicalPackMember[] members;
                VnResult built = BuildReplacementMembers(packStableId, path, bytes, out members);
                if (built != VnResult.VN_OK) return built;
                VnResult result = Packs.ActivateCanonicalPackSet(
                    members, SetRevision + 1, out var report, out var rows, InitPack, SetRevision);
                if (result != VnResult.VN_OK) return result;
                shared.Path = path;
                shared.Bytes = bytes;
                if (bytes != null) contentBytes[packStableId] = bytes;
                else contentBytes.Remove(packStableId);
                ApplyActivation(report, rows);
                return VnResult.VN_OK;
            }
            finally { mutationPhase = MutationPhase.Idle; }
        }

        public VnResult PrepareStartupPacks()
        {
            if (!active) return VnResult.VN_NOT_INITIALIZED;
            VnResult owner = RequireOwnerThread();
            if (owner != VnResult.VN_OK) return owner;
            if (mutationPhase != MutationPhase.Idle) return VnResult.VN_IN_USE;
            FailedStartupPackStableId = 0;
            for (int i = 0; i < startupIds.Length; ++i)
            {
                if (startupPacks[i].IsValid) continue;
                VnResult acquired = AcquirePack(startupIds[i], out Pack pack);
                if (acquired != VnResult.VN_OK)
                {
                    FailedStartupPackStableId = startupIds[i];
                    return StartupResult = acquired;
                }
                startupPacks[i] = pack;
            }
            return StartupResult = VnResult.VN_OK;
        }

        public VnResult ReleaseStartupPacks()
        {
            if (!active) return VnResult.VN_NOT_INITIALIZED;
            VnResult owner = RequireOwnerThread();
            if (owner != VnResult.VN_OK) return owner;
            VnResult firstFailure = VnResult.VN_OK;
            for (int i = startupPacks.Length - 1; i >= 0; --i)
            {
                if (!startupPacks[i].IsValid) continue;
                VnResult result = ReleasePack(startupIds[i], startupPacks[i]);
                if (result == VnResult.VN_OK) startupPacks[i] = default;
                else if (firstFailure == VnResult.VN_OK) firstFailure = result;
            }
            return firstFailure;
        }

        public VnResult Shutdown()
        {
            if (!active)
            {
                return VnResult.VN_NOT_INITIALIZED;
            }
            VnResult owner = RequireOwnerThread();
            if (owner != VnResult.VN_OK) return owner;
            if (mutationPhase == MutationPhase.ApplyingNative) return VnResult.VN_IN_USE;
            MutationPhase previousPhase = mutationPhase;
            mutationPhase = MutationPhase.ApplyingNative;
            try
            {
                // Ending the owned session tears down the entire native engine.
                VnResult term = HarkenEngine.Term();
                if (term == VnResult.VN_OK)
                {
                    active = false;
                    packs.Clear();
                    contentBytes.Clear();
                    companionNames.Clear();
                    rawCompanionLeases.Clear();
                    Array.Clear(startupPacks, 0, startupPacks.Length);
                }
                return term;
            }
            finally { mutationPhase = previousPhase; }
        }

        public void Dispose()
        {
            if (active)
            {
                Shutdown();
            }
        }
    }

    public static partial class PackBootstrap
    {
        private const uint DefaultMaxPacks = 256;

        public static VnResult ApplyRecommendations(
            ref VnConfig config, in InitPackInspection inspection,
            int startupPackCount)
        {
            if (startupPackCount < 0 || inspection.Result != VnResult.VN_OK)
            {
                return VnResult.VN_INVALID_ARG;
            }
            uint configSize = (uint)Marshal.SizeOf<VnConfig>();
            if (config.structSize == 0)
            {
                config.structSize = configSize;
            }
            if (config.structSize != configSize)
            {
                return VnResult.VN_INVALID_ARG;
            }

            ulong requiredPacks = (ulong)startupPackCount + 1;
            ulong configuredPacks = config.maxPacks == 0
                ? DefaultMaxPacks
                : config.maxPacks;
            if (requiredPacks > configuredPacks)
            {
                return VnResult.VN_INVALID_CONFIG;
            }
            if (config.maxCatalogEntries != 0 &&
                config.maxCatalogEntries < inspection.CatalogEntries)
            {
                return VnResult.VN_PACK_CATALOG_CAPACITY;
            }
            if (config.maxCatalogBytes != 0 &&
                config.maxCatalogBytes < inspection.CatalogBytes)
            {
                return VnResult.VN_PACK_CATALOG_CAPACITY;
            }
            if (config.maxActions != 0 &&
                inspection.RecommendedMaxActions != 0 &&
                config.maxActions < inspection.RecommendedMaxActions)
            {
                return VnResult.VN_PACK_RESIDENT_CAPACITY;
            }
            if (config.maxResidentDefinitionBytes != 0 &&
                config.maxResidentDefinitionBytes <
                    inspection.RecommendedResidentDefinitionBytes)
            {
                return VnResult.VN_PACK_RESIDENT_CAPACITY;
            }

            if (config.maxCatalogEntries == 0)
            {
                config.maxCatalogEntries =
                    inspection.RecommendedMaxCatalogEntries;
            }
            if (config.maxActions == 0 &&
                inspection.RecommendedMaxActions != 0)
            {
                config.maxActions = inspection.RecommendedMaxActions;
            }
            if (config.maxCatalogBytes == 0)
            {
                config.maxCatalogBytes =
                    inspection.RecommendedCatalogBytes;
            }
            if (config.maxResidentDefinitionBytes == 0)
            {
                config.maxResidentDefinitionBytes =
                    inspection.RecommendedResidentDefinitionBytes;
            }
            return VnResult.VN_OK;
        }

        public static VnResult Start(
            string initPackPath, string containmentRoot,
            ulong[] startupPackStableIds, ref VnConfig config,
            out PackBootstrapSession session)
            => Start(initPackPath, containmentRoot, startupPackStableIds, null, ref config, out session);

        public static VnResult Start(
            string initPackPath, string containmentRoot,
            ulong[] startupPackStableIds, string language, ref VnConfig config,
            out PackBootstrapSession session)
        {
            session = null;
            if (string.IsNullOrEmpty(initPackPath) ||
                string.IsNullOrEmpty(containmentRoot) ||
                startupPackStableIds == null)
            {
                return VnResult.VN_INVALID_ARG;
            }
            if (!ValidStartupIds(startupPackStableIds)) return VnResult.VN_INVALID_ARG;

            if (!PackManifest.TryReadManifest(containmentRoot, language, out _,
                    out PackManifestEntry[] manifestPacks, out string selectedLanguage, out ulong initId))
                return VnResult.VN_BAD_PACK;
            PackManifestEntry initEntry = Array.Find(manifestPacks, entry => entry.PackStableId == initId);
            if (!PackManifest.TryJoinContainedPath(containmentRoot, initEntry.Path, out string expectedInitPath))
                return VnResult.VN_INVALID_ARG;
            try
            {
                StringComparison comparison = Path.DirectorySeparatorChar == '\\'
                    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                if (!string.Equals(expectedInitPath, Path.GetFullPath(initPackPath), comparison))
                    return VnResult.VN_PACK_STALE_REVISION;
            }
            catch (ArgumentException) { return VnResult.VN_INVALID_ARG; }
            catch (NotSupportedException) { return VnResult.VN_INVALID_ARG; }
            catch (PathTooLongException) { return VnResult.VN_INVALID_ARG; }
            byte[] initBytes;
            try { initBytes = File.ReadAllBytes(expectedInitPath); }
            catch (FileNotFoundException) { return VnResult.VN_CONTENT_NOT_LOADED; }
            catch (DirectoryNotFoundException) { return VnResult.VN_CONTENT_NOT_LOADED; }
            catch (IOException) { return VnResult.VN_BAD_PACK; }
            catch (UnauthorizedAccessException) { return VnResult.VN_BAD_PACK; }
            if (!PackManifest.VerifyPackBytes(initBytes, initEntry))
                return VnResult.VN_PACK_HASH_MISMATCH;
            VnResult projection = GameplayPublication.Read(containmentRoot, selectedLanguage,
                out GameplayPublication publication, out _);
            if (projection != VnResult.VN_OK && projection != VnResult.VN_CAPABILITY_UNAVAILABLE) return projection;
            return StartPrepared(publication, initId, initBytes, startupPackStableIds,
                containmentRoot, selectedLanguage, null, null, ref config, out session);
        }

        public static VnResult Start(GameplayPublication publication, byte[] initBytes, ulong[] startupPackStableIds,
            PackByteProvider.Handler contentProvider, object providerState, ref VnConfig config,
            out PackBootstrapSession session)
        {
            session = null;
            if (publication == null || contentProvider == null || !ValidStartupIds(startupPackStableIds))
                return VnResult.VN_INVALID_ARG;
            if (initBytes == null || initBytes.Length == 0) return VnResult.VN_CONTENT_NOT_LOADED;
            return StartPrepared(publication, publication.InitPackStableId, (byte[])initBytes.Clone(),
                startupPackStableIds, null, publication.Language, contentProvider, providerState, ref config, out session);
        }

        internal static VnResult ReadContentFile(string containmentRoot, string language, ulong packStableId,
            out string path, out byte[] bytes)
        {
            path = null;
            bytes = null;
            if (!PackManifest.TryReadManifest(containmentRoot, language, out _, out PackManifestEntry[] entries,
                    out string selectedLanguage) || selectedLanguage != language)
                return VnResult.VN_PACK_STALE_REVISION;
            PackManifestEntry entry = Array.Find(entries, item => item.PackStableId == packStableId);
            if (entry.PackStableId == 0) return VnResult.VN_UNKNOWN_ID;
            if (!PackManifest.TryJoinContainedPath(containmentRoot, entry.Path, out path))
                return VnResult.VN_PACK_PATH_ESCAPE;
            try { bytes = File.ReadAllBytes(path); }
            catch (FileNotFoundException) { return VnResult.VN_CONTENT_NOT_LOADED; }
            catch (DirectoryNotFoundException) { return VnResult.VN_CONTENT_NOT_LOADED; }
            catch (IOException) { return VnResult.VN_BAD_PACK; }
            catch (UnauthorizedAccessException) { return VnResult.VN_BAD_PACK; }
            return PackManifest.VerifyPackBytes(bytes, entry) ? VnResult.VN_OK : VnResult.VN_PACK_HASH_MISMATCH;
        }

        private static VnResult StartPrepared(GameplayPublication publication, ulong initId, byte[] initBytes,
            ulong[] startupIds, string containmentRoot, string language, PackByteProvider.Handler contentProvider,
            object providerState, ref VnConfig config, out PackBootstrapSession session)
        {
            session = null;
            if (publication != null)
            {
                VnResult paired = publication.ValidateInit(initBytes, out _);
                if (paired != VnResult.VN_OK) return paired;
            }
            VnResult result = PackByteProvider.InspectInitPack(initId, PackByteProvider.PreparedBytes,
                initBytes, out InitPackInspection inspection);
            if (result != VnResult.VN_OK)
            {
                return result;
            }
            result = ApplyRecommendations(
                ref config, inspection, startupIds.Length);
            if (result != VnResult.VN_OK)
            {
                return result;
            }
            byte[] ownedInitBytes = (byte[])initBytes.Clone();
            CanonicalPackMember[] members;
            string[] boundPaths;
            byte[][] boundBytes;
            try
            {
                var built = new List<CanonicalPackMember>(startupIds.Length + 1)
                {
                    new CanonicalPackMember(initId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT,
                        provider: PackByteProvider.PreparedBytes, providerState: ownedInitBytes)
                };
                boundPaths = new string[startupIds.Length];
                boundBytes = new byte[startupIds.Length][];
                ulong[] contentIds = (ulong[])startupIds.Clone();
                Array.Sort(contentIds);
                for (int i = 0; i < contentIds.Length; ++i)
                {
                    ulong contentId = contentIds[i];
                    if (contentId == initId) return VnResult.VN_INVALID_ARG;
                    string boundPath = null;
                    byte[] ownedBytes = null;
                    if (contentProvider != null)
                    {
                        string locator = null;
                        uint variantKey = 0;
                        ulong expectedHash = 0;
                        if (publication != null &&
                            publication.TryFindPack(contentId, out PackManifestEntry publishedEntry))
                        {
                            locator = publishedEntry.Path;
                            variantKey = publishedEntry.VariantKey;
                            expectedHash = publishedEntry.ContentHash;
                        }
                        VnResult provided;
                        try
                        {
                            provided = contentProvider(
                                contentId, locator, variantKey, expectedHash, out ownedBytes, providerState);
                        }
                        catch { return VnResult.VN_PACK_PROVIDER_FAILED; }
                        if (provided == VnResult.VN_CONTENT_NOT_LOADED ||
                            (provided == VnResult.VN_OK && (ownedBytes == null || ownedBytes.Length == 0)))
                            continue;
                        if (provided != VnResult.VN_OK) return provided;
                        ownedBytes = (byte[])ownedBytes.Clone();
                        if (publication != null &&
                            publication.TryFindPack(contentId, out PackManifestEntry publishedPack) &&
                            !PackManifest.VerifyPackBytes(ownedBytes, publishedPack))
                            return VnResult.VN_PACK_HASH_MISMATCH;
                        built.Add(new CanonicalPackMember(contentId,
                            VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                            provider: PackByteProvider.PreparedBytes, providerState: ownedBytes));
                    }
                    else if (publication != null &&
                             publication.TryFindPack(contentId, out PackManifestEntry entry) &&
                             PackManifest.TryJoinContainedPath(containmentRoot, entry.Path, out string publishedPath))
                    {
                        boundPath = publishedPath;
                        try { ownedBytes = File.ReadAllBytes(publishedPath); }
                        catch (FileNotFoundException) { continue; }
                        catch (DirectoryNotFoundException) { continue; }
                        catch (IOException) { return VnResult.VN_BAD_PACK; }
                        catch (UnauthorizedAccessException) { return VnResult.VN_BAD_PACK; }
                        if (ownedBytes == null || ownedBytes.Length == 0)
                            continue;
                        if (!PackManifest.VerifyPackBytes(ownedBytes, entry))
                            return VnResult.VN_PACK_HASH_MISMATCH;
                        built.Add(new CanonicalPackMember(contentId,
                            VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                            provider: PackByteProvider.PreparedBytes, providerState: ownedBytes));
                    }
                    else
                    {
                        VnResult read = ReadContentFile(containmentRoot, language, contentId,
                            out boundPath, out ownedBytes);
                        if (read == VnResult.VN_CONTENT_NOT_LOADED) continue;
                        if (read != VnResult.VN_OK) return read;
                        built.Add(new CanonicalPackMember(contentId,
                            VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                            provider: PackByteProvider.PreparedBytes, providerState: ownedBytes));
                    }
                    for (int s = 0; s < startupIds.Length; ++s)
                    {
                        if (startupIds[s] == contentId)
                        {
                            boundPaths[s] = boundPath;
                            boundBytes[s] = ownedBytes;
                            break;
                        }
                    }
                }
                members = built.ToArray();
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }

            result = HarkenEngine.Init(config);
            if (result != VnResult.VN_OK)
            {
                return result;
            }

            result = Packs.ActivateCanonicalPackSet(members, 1, out var report, out var rows);
            if (result == VnResult.VN_OK && publication != null)
                result = Packs.SetGameplayCatalog(new Pack(report.initHandle), publication);
            if (result != VnResult.VN_OK)
            {
                VnResult term = HarkenEngine.Term();
                return term == VnResult.VN_OK ? result : term;
            }

            session = new PackBootstrapSession(
                new Pack(report.initHandle), startupIds, inspection, containmentRoot, language,
                publication, contentProvider, providerState, ownedInitBytes);
            session.SetRevision = report.resultSetRevision;
            if (rows != null)
            {
                for (int i = 0; i < startupIds.Length; ++i)
                {
                    for (int r = 0; r < rows.Length; ++r)
                    {
                        if (rows[r].packStableId == startupIds[i] && rows[r].handle != 0)
                        {
                            session.BindStartupPack(
                                i, new Pack(rows[r].handle), boundPaths[i], boundBytes[i]);
                            break;
                        }
                    }
                }
            }
            result = session.PrepareStartupPacks();
            if (result == VnResult.VN_CONTENT_NOT_LOADED) return result;
            if (result != VnResult.VN_OK)
            {
                session.Shutdown();
                session = null;
                HarkenEngine.Term();
                return result;
            }
            return result;
        }

        private static bool ValidStartupIds(ulong[] ids)
        {
            if (ids == null) return false;
            for (int i = 0; i < ids.Length; ++i)
            {
                if (ids[i] == 0) return false;
                for (int j = 0; j < i; ++j)
                    if (ids[j] == ids[i]) return false;
            }
            return true;
        }
    }
}
