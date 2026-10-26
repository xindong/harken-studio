using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace Harken
{
    public sealed class ActionPreparation
    {
        private readonly PackBootstrapSession owner;
        private readonly string[] names;
        private readonly PackBootstrapSession.SharedPack[] owners;
        private bool refresh;
        private int count;
        private bool prepared;
        private bool preparing;

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
                VnResult result = owner.AcquireCompanions(names, owners, refresh);
                if (result == VnResult.VN_OK)
                {
                    count = owners.Length;
                    prepared = true;
                    refresh = false;
                }
                return result;
            }
            finally { preparing = false; }
        }

        /// <summary>Prepares on the session's control context. Cancellation never discards a committed lease.</summary>
        public async Task<VnResult> PrepareAsync(CancellationToken cancellation = default)
        {
            cancellation.ThrowIfCancellationRequested();
            if (!owner.IsActive) return VnResult.VN_NOT_INITIALIZED;
            if (preparing) return VnResult.VN_IN_USE;
            if (prepared) return VnResult.VN_OK;
            if (count != 0) return VnResult.VN_IN_USE;
            preparing = true;
            try
            {
                VnResult result = await owner.AcquireCompanionsAsync(names, owners, refresh, cancellation);
                if (result == VnResult.VN_OK)
                {
                    count = owners.Length;
                    prepared = true;
                    refresh = false;
                }
                return result;
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
                VnResult result = owner.ReleaseCompanion(owners[index]);
                // Keep dependencies alive while their dependent Pack still belongs to this scope.
                if (result != VnResult.VN_OK) return LastReleaseResult = result;
                owners[index] = null;
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
        private readonly ulong[] startupIds;
        private readonly Pack[] startupPacks;
        private readonly byte[] initBytes;

        internal sealed class SharedPack
        {
            internal ulong StableId;
            internal ulong ContentHash;
            internal int ByteCount;
            internal PackCompanion Names;
            public Pack Handle;
            public int Requests;
            public byte[] Bytes;
        }

        internal PackBootstrapSession(
            Pack initPack, ulong[] startupIds, InitPackInspection inspection,
            string containmentRoot, string language, byte[] initBytes)
        {
            InitPack = initPack;
            this.startupIds = (ulong[])startupIds.Clone();
            startupPacks = new Pack[startupIds.Length];
            Inspection = inspection;
            ContainmentRoot = containmentRoot;
            Language = language;
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
        public GameplayPublication Publication => null;
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

        public VnResult AcquirePack(ulong packStableId, out Pack pack)
        {
            return AcquireCompanionPack(packStableId, out pack);
        }

        /// <summary>On failure, a non-null scope still owns any references whose rollback could not finish.</summary>
        public VnResult PrepareActions(string[] names, string projectId, out ActionPreparation preparation)
        {
            return PrepareCompanions(names, projectId, false, out preparation);
        }

        public VnResult ReleasePack(ulong packStableId, Pack pack)
        {
            return ReleaseCompanionPack(packStableId, pack);
        }

        public VnResult ReplacePack(ulong packStableId, Pack pack)
        {
            return ReplaceCompanionPack(packStableId, pack);
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
                    companionNames.Clear();
                    rawCompanionLeases.Clear();
                    CancelCompanionReads();
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
            if (string.IsNullOrEmpty(initPackPath) || string.IsNullOrEmpty(containmentRoot) ||
                startupPackStableIds == null || startupPackStableIds.Length != 0)
                return VnResult.VN_INVALID_ARG;
            try
            {
                string expected = Path.GetFullPath(Path.Combine(containmentRoot, "Init.bytes"));
                StringComparison comparison = Path.DirectorySeparatorChar == '\\'
                    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                if (!string.Equals(expected, Path.GetFullPath(initPackPath), comparison))
                    return VnResult.VN_PACK_INCOMPATIBLE;
            }
            catch (ArgumentException) { return VnResult.VN_INVALID_ARG; }
            catch (NotSupportedException) { return VnResult.VN_INVALID_ARG; }
            return StartDirectory(containmentRoot, language, ref config, out session);
        }

    }
}
