using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace Harken
{
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
        private readonly Dictionary<string, RetiredGameplayAction> retiredByName;

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
            retiredByName = retired.ToDictionary(row => row.Name, StringComparer.Ordinal);
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
        internal bool TryFindRetired(string name, out RetiredGameplayAction row) => retiredByName.TryGetValue(name, out row);

        public VnResult GetActionPreparationOrder(string[] names, string projectId, out ulong[] ids)
        {
            ids = Array.Empty<ulong>();
            if (names == null || !PackManifest.IsPlatformId(projectId)) return VnResult.VN_INVALID_ARG;
            if (projectId != ProjectId) return VnResult.VN_GAMEPLAY_WRONG_PROJECT;
            var ordered = new List<ulong>();
            var visited = new HashSet<ulong>();
            var visiting = new HashSet<ulong>();
            foreach (string name in names)
            {
                if (string.IsNullOrEmpty(name)) return VnResult.VN_INVALID_ARG;
                if (!TryFindAction(name, out GameplayAction action))
                    return TryFindRetired(name, out _) ? VnResult.VN_GAMEPLAY_NAME_RETIRED : VnResult.VN_UNKNOWN_ID;
                if (!VisitPreparationPack(action.OwnerPackStableId, visited, visiting, ordered))
                    return VnResult.VN_BAD_PACK;
            }
            ids = ordered.ToArray();
            return VnResult.VN_OK;
        }

        private bool VisitPreparationPack(ulong id, HashSet<ulong> visited,
            HashSet<ulong> visiting, List<ulong> ordered)
        {
            if (id == InitPackStableId || visited.Contains(id)) return true;
            if (!visiting.Add(id) || !TryFindPack(id, out PackManifestEntry pack)) return false;
            foreach (string dependency in pack.Dependencies)
                if (!ulong.TryParse(dependency, NumberStyles.None, CultureInfo.InvariantCulture, out ulong dependencyId) ||
                    !VisitPreparationPack(dependencyId, visited, visiting, ordered)) return false;
            visiting.Remove(id);
            visited.Add(id);
            ordered.Add(id);
            return true;
        }

        public static VnResult Parse(string inventoryJson, string selectedJson, string language,
            out GameplayPublication publication, out string error)
            => PackManifest.ParseGameplayPublication(inventoryJson, selectedJson, language, out publication, out error);

        public static VnResult Read(string platformDirectory, string language,
            out GameplayPublication publication, out string error)
        {
            publication = null;
            error = "Cannot read the selected gameplay publication.";
            if (string.IsNullOrEmpty(platformDirectory)) return VnResult.VN_INVALID_ARG;
            try
            {
                if (File.Exists(Path.Combine(platformDirectory, ".harken-pack-publishing")))
                    return VnResult.VN_PACK_STALE_REVISION;
                string inventory = File.ReadAllText(Path.Combine(platformDirectory, PackManifest.ManifestFileName));
                if (!PackManifest.TryGetSelectedManifestPath(inventory, language, out string relativePath))
                    return VnResult.VN_BAD_PACK;
                string selected = null;
                if (relativePath != null)
                {
                    if (!PackManifest.TryJoinContainedPath(platformDirectory, relativePath, out string path))
                        return VnResult.VN_PACK_PATH_ESCAPE;
                    selected = File.ReadAllText(path);
                }
                return Parse(inventory, selected, language, out publication, out error);
            }
            catch (IOException exception) { error = exception.Message; return VnResult.VN_BAD_PACK; }
            catch (UnauthorizedAccessException exception) { error = exception.Message; return VnResult.VN_BAD_PACK; }
            catch (ArgumentException exception) { error = exception.Message; return VnResult.VN_INVALID_ARG; }
        }

        public VnResult ValidateInit(byte[] bytes, out string error)
            => GameplayCatalogPairing.Validate(this, bytes, out error);
    }

    public static partial class PackManifest
    {
        internal static VnResult ParseGameplayPublication(string inventoryJson, string selectedJson, string language,
            out GameplayPublication publication, out string error)
        {
            publication = null;
            error = "Invalid or inconsistent gameplay publication metadata.";
            if (!TrySelectManifest(inventoryJson, selectedJson, language, out PackIndexEntry platform,
                    out PackManifestEntry[] packs, out string selectedLanguage, out ulong initId) ||
                !TryRoot(selectedJson ?? inventoryJson, out var root)) return VnResult.VN_BAD_PACK;
            if (!root.TryGetValue("gameplayCatalog", out object value))
            {
                error = "The publication does not contain a gameplay catalog.";
                return VnResult.VN_CAPABILITY_UNAVAILABLE;
            }
            if (!(value is Dictionary<string, object> catalog) ||
                !OnlyKeys(catalog, "schema", "projectId", "digest", "initPackStableId", "initContentRevision", "actions", "retired") ||
                !Integer(catalog, "schema", out ulong schema)) return VnResult.VN_BAD_PACK;
            if (schema != 2) return VnResult.VN_PACK_INCOMPATIBLE;
            string project = Text(catalog, "projectId");
            if (!IsPlatformId(project) || !UInt64(Text(catalog, "digest"), out ulong digest) ||
                !UInt64(Text(catalog, "initPackStableId"), out ulong catalogInit) || catalogInit != initId ||
                !Integer(catalog, "initContentRevision", out ulong revision) ||
                !JsonArray(catalog, "actions", out var active) || !JsonArray(catalog, "retired", out var retired))
                return VnResult.VN_BAD_PACK;
            if (active.Count > 4096 || retired.Count > 4096) return VnResult.VN_PACK_CATALOG_CAPACITY;
            var names = new HashSet<string>(StringComparer.Ordinal);
            var ids = new HashSet<VnObjectId>();
            var actions = new List<GameplayAction>();
            foreach (object item in active)
            {
                if (!(item is Dictionary<string, object> row) ||
                    !OnlyKeys(row, "name", "actionStableId", "nameBindingRevision", "callableFingerprint",
                        "ownerPackStableId", "providerClosureFingerprint") ||
                    !CatalogName(Text(row, "name")) || !names.Add(Text(row, "name")) ||
                    !VnObjectId.TryParse(Text(row, "actionStableId"), out VnObjectId id) || id.IsNull || !ids.Add(id) ||
                    !UInt64(Text(row, "nameBindingRevision"), out ulong binding) || binding == 0 ||
                    !UInt64(Text(row, "callableFingerprint"), out ulong callable) ||
                    !UInt64(Text(row, "ownerPackStableId"), out ulong owner) || owner == 0 ||
                    owner == initId || !packs.Any(pack => pack.PackStableId == owner) ||
                    !UInt64(Text(row, "providerClosureFingerprint"), out ulong closure)) return VnResult.VN_BAD_PACK;
                actions.Add(new GameplayAction(Text(row, "name"), id, binding, callable, owner, closure));
            }
            var history = new List<RetiredGameplayAction>();
            foreach (object item in retired)
            {
                if (!(item is Dictionary<string, object> row) ||
                    !OnlyKeys(row, "name", "lastActionStableId", "lastNameBindingRevision", "reason") ||
                    !CatalogName(Text(row, "name")) || !names.Add(Text(row, "name")) ||
                    !VnObjectId.TryParse(Text(row, "lastActionStableId"), out VnObjectId id) || id.IsNull ||
                    !UInt64(Text(row, "lastNameBindingRevision"), out ulong binding)) return VnResult.VN_BAD_PACK;
                string reason = Text(row, "reason");
                uint code = reason == "renamed" ? 1U : reason == "deleted" ? 2U : reason == "reused" ? 3U : 0U;
                if (code == 0) return VnResult.VN_BAD_PACK;
                history.Add(new RetiredGameplayAction(Text(row, "name"), id, binding, code));
            }
            publication = new GameplayPublication(platform, selectedLanguage, packs, project, initId, revision,
                digest, actions.ToArray(), history.ToArray());
            error = null;
            return VnResult.VN_OK;
        }

        private static bool Integer(Dictionary<string, object> row, string key, out ulong result)
        {
            result = 0;
            if (!row.TryGetValue(key, out object value) || !(value is ulong integer)) return false;
            result = integer;
            return true;
        }

        private static bool CatalogName(string name)
            => !string.IsNullOrEmpty(name) && name.IndexOf('\0') < 0 && Encoding.UTF8.GetByteCount(name) <= 128;
    }
}
