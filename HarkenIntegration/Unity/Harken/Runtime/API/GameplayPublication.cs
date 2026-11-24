using System;
using System.Collections.Generic;
using System.Linq;

namespace Harken
{
    [Obsolete("Use ActionCatalog.")]
    public readonly struct PackManifestEntry
    {
        internal PackManifestEntry(string name, ulong packStableId, string path, ulong contentHash)
        {
            Name = name;
            PackStableId = packStableId;
            Path = path;
            ContentHash = contentHash;
        }

        public string Name { get; }
        public ulong PackStableId { get; }
        public string Path { get; }
        public string PlatformId => string.Empty;
        public ulong ContentHash { get; }
        public string[] Dependencies => Array.Empty<string>();
        public uint VariantKey => 0;
    }

    [Obsolete("Use ActionCatalog.")]
    public readonly struct PackIndexEntry
    {
        public string Id => null;
        public string Name => null;
        public string BasePlatform => null;
        public string Path => null;
    }

    [Obsolete("Use ActionCatalogEntry.")]
    public sealed class GameplayAction
    {
        internal GameplayAction(ActionCatalogEntry entry)
        {
            Name = entry.Name;
            ActionStableId = entry.ActionId;
            OwnerPackStableId = entry.PackStableId;
        }
        public string Name { get; }
        public VnObjectId ActionStableId { get; }
        public ulong NameBindingRevision => 0;
        public ulong CallableFingerprint => 0;
        public ulong OwnerPackStableId { get; }
        public ulong ProviderClosureFingerprint => 0;
    }

    [Obsolete("Use ActionCatalog.")]
    public sealed class RetiredGameplayAction
    {
        private RetiredGameplayAction() { }
        public string Name => null;
        public VnObjectId LastActionStableId => default;
        public ulong LastNameBindingRevision => 0;
        public uint Reason => 0;
    }

    [Obsolete("Use ActionCatalog.")]
    public sealed class GameplayPublication
    {
        private readonly ActionCatalog catalog;
        private readonly Dictionary<string, GameplayAction> actionsByName;
        private readonly Dictionary<VnObjectId, GameplayAction> actionsById;
        private readonly Dictionary<ulong, PackManifestEntry> packsById;

        internal GameplayPublication(ActionCatalogSnapshot snapshot)
        {
            catalog = snapshot.Catalog;
            InitContentRevision = snapshot.Init.Facts.contentRevision;
            GameplayAction[] actions = catalog.Actions.Select(entry => new GameplayAction(entry)).ToArray();
            var packs = new List<PackManifestEntry>
            {
                new PackManifestEntry("Init", snapshot.Init.Facts.packStableId, "Init.bytes", snapshot.Init.Facts.contentHash)
            };
            packs.AddRange(snapshot.Actions.Select(row => new PackManifestEntry(row.Key,
                row.Value.Input.Facts.packStableId, row.Value.Path + ".bytes", row.Value.Input.Facts.contentHash)));
            Actions = Array.AsReadOnly(actions);
            Packs = packs.AsReadOnly();
            actionsByName = actions.ToDictionary(row => row.Name, StringComparer.Ordinal);
            actionsById = actions.ToDictionary(row => row.ActionStableId);
            packsById = packs.ToDictionary(row => row.PackStableId);
        }

        public PackIndexEntry Platform => default;
        public string Language => catalog.Language;
        public string ProjectId => catalog.ProjectId;
        public ulong InitPackStableId => catalog.InitPackStableId;
        public ulong InitContentRevision { get; }
        public ulong Digest => 0;
        public IReadOnlyList<GameplayAction> Actions { get; }
        public IReadOnlyList<RetiredGameplayAction> RetiredActions => Array.Empty<RetiredGameplayAction>();
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
            => catalog.GetPreparationOrder(names, projectId, out ids);
    }
}
