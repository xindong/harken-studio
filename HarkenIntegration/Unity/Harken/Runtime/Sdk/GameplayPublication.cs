using System;
using System.Collections.Generic;
using System.Linq;

namespace Harken
{
    public readonly struct PackManifestEntry
    {
        public PackManifestEntry(string name, ulong packStableId, string path,
            string platformId, ulong contentHash, string[] dependencies, uint variantKey = 0)
        {
            Name = name;
            PackStableId = packStableId;
            Path = path;
            PlatformId = platformId;
            ContentHash = contentHash;
            Dependencies = dependencies;
            VariantKey = variantKey;
        }

        public string Name { get; }
        public ulong PackStableId { get; }
        public string Path { get; }
        public string PlatformId { get; }
        public ulong ContentHash { get; }
        public string[] Dependencies { get; }
        public uint VariantKey { get; }
    }

    public readonly struct PackIndexEntry
    {
        public PackIndexEntry(string id, string name, string basePlatform, string path)
        {
            Id = id;
            Name = name;
            BasePlatform = basePlatform;
            Path = path;
        }

        public string Id { get; }
        public string Name { get; }
        public string BasePlatform { get; }
        public string Path { get; }
    }

    public sealed class GameplayAction
    {
        internal GameplayAction(string name, VnObjectId id, ulong revision,
            ulong callable, ulong owner, ulong closure)
        {
            Name = name; ActionStableId = id; NameBindingRevision = revision;
            CallableFingerprint = callable; OwnerPackStableId = owner; ProviderClosureFingerprint = closure;
        }
        public string Name { get; }
        public VnObjectId ActionStableId { get; }
        public ulong NameBindingRevision { get; }
        public ulong CallableFingerprint { get; }
        public ulong OwnerPackStableId { get; }
        public ulong ProviderClosureFingerprint { get; }
    }

    public sealed class RetiredGameplayAction
    {
        internal RetiredGameplayAction(string name, VnObjectId id, ulong revision, uint reason)
        {
            Name = name; LastActionStableId = id;
            LastNameBindingRevision = revision; Reason = reason;
        }
        public string Name { get; }
        public VnObjectId LastActionStableId { get; }
        public ulong LastNameBindingRevision { get; }
        public uint Reason { get; }
    }

    public sealed class GameplayPublication
    {
        private readonly Dictionary<string, GameplayAction> actionsByName;
        private readonly Dictionary<VnObjectId, GameplayAction> actionsById;
        private readonly Dictionary<ulong, PackManifestEntry> packsById;

        internal GameplayPublication(PackIndexEntry platform, string language, PackManifestEntry[] packs,
            string projectId, ulong initId, ulong revision, ulong digest,
            GameplayAction[] actions, RetiredGameplayAction[] retired)
        {
            Platform = platform; Language = language; ProjectId = projectId;
            InitPackStableId = initId; InitContentRevision = revision; Digest = digest;
            Actions = Array.AsReadOnly(actions); RetiredActions = Array.AsReadOnly(retired);
            Packs = Array.AsReadOnly(packs);
            actionsByName = actions.ToDictionary(row => row.Name, StringComparer.Ordinal);
            actionsById = actions.ToDictionary(row => row.ActionStableId);
            packsById = packs.ToDictionary(row => row.PackStableId);
        }

        public PackIndexEntry Platform { get; }
        public string Language { get; }
        public string ProjectId { get; }
        public ulong InitPackStableId { get; }
        public ulong InitContentRevision { get; }
        public ulong Digest { get; }
        public IReadOnlyList<GameplayAction> Actions { get; }
        public IReadOnlyList<RetiredGameplayAction> RetiredActions { get; }
        public IReadOnlyList<PackManifestEntry> Packs { get; }
        public PackManifestEntry InitPack => packsById[InitPackStableId];
        public bool TryFindAction(string name, out GameplayAction action)
        {
            action = null;
            return name != null && actionsByName.TryGetValue(name, out action);
        }
        public bool TryFindAction(VnObjectId id, out GameplayAction action) => actionsById.TryGetValue(id, out action);
        public bool TryFindPack(ulong id, out PackManifestEntry pack) => packsById.TryGetValue(id, out pack);

        public VnResult GetActionPreparationOrder(string[] names, string projectId, out ulong[] ids)
        {
            ids = Array.Empty<ulong>();
            if (names == null) return VnResult.VN_INVALID_ARG;
            if (projectId != ProjectId) return VnResult.VN_GAMEPLAY_WRONG_PROJECT;
            var found = new List<ulong>();
            foreach (string name in names)
            {
                if (!TryFindAction(name, out GameplayAction action)) return VnResult.VN_UNKNOWN_ID;
                if (!found.Contains(action.OwnerPackStableId)) found.Add(action.OwnerPackStableId);
            }
            ids = found.ToArray();
            return VnResult.VN_OK;
        }

        public static VnResult Parse(string inventoryJson, string selectedJson, string language,
            out GameplayPublication publication, out string error)
        {
            publication = null;
            error = "Legacy Pack manifests are unsupported. Re-export with the current Studio and use PackBootstrap.StartDirectory or PackSource.";
            return VnResult.VN_PACK_INCOMPATIBLE;
        }

        public static VnResult Read(string platformDirectory, string language,
            out GameplayPublication publication, out string error)
        {
            return Parse(null, null, language, out publication, out error);
        }

        public VnResult ValidateInit(byte[] bytes, out string error)
        {
            error = "Legacy gameplay publications are unsupported. Re-export with the current Studio.";
            return VnResult.VN_PACK_INCOMPATIBLE;
        }
    }

}
