using System;
using System.Collections.Generic;
using System.Linq;

namespace Harken
{
    public sealed class ActionCatalogEntry
    {
        internal ActionCatalogEntry(string name, VnObjectId actionId, ulong packStableId)
        {
            Name = name; ActionId = actionId; PackStableId = packStableId;
        }
        public string Name { get; }
        public VnObjectId ActionId { get; }
        public ulong PackStableId { get; }
    }

    public sealed class ActionCatalog
    {
        private readonly Dictionary<string, ActionCatalogEntry> actionsByName;
        private readonly Dictionary<VnObjectId, ActionCatalogEntry> actionsById;

        internal ActionCatalog(string projectId, string language, ulong initPackStableId, ActionCatalogEntry[] actions)
        {
            ProjectId = projectId; Language = language; InitPackStableId = initPackStableId;
            Actions = Array.AsReadOnly(actions);
            actionsByName = actions.ToDictionary(row => row.Name, StringComparer.Ordinal);
            actionsById = actions.ToDictionary(row => row.ActionId);
        }

        public string ProjectId { get; }
        public string Language { get; }
        public ulong InitPackStableId { get; }
        public IReadOnlyList<ActionCatalogEntry> Actions { get; }
        public bool TryFindAction(string name, out ActionCatalogEntry action)
        {
            action = null;
            return name != null && actionsByName.TryGetValue(name, out action);
        }
        public bool TryFindAction(VnObjectId id, out ActionCatalogEntry action) => actionsById.TryGetValue(id, out action);

        public VnResult GetPreparationOrder(string[] names, string projectId, out ulong[] ids)
        {
            ids = Array.Empty<ulong>();
            if (names == null) return VnResult.VN_INVALID_ARG;
            if (projectId != ProjectId) return VnResult.VN_GAMEPLAY_WRONG_PROJECT;
            var found = new List<ulong>();
            foreach (string name in names)
            {
                if (!TryFindAction(name, out ActionCatalogEntry action)) return VnResult.VN_UNKNOWN_ID;
                if (!found.Contains(action.PackStableId)) found.Add(action.PackStableId);
            }
            ids = found.ToArray();
            return VnResult.VN_OK;
        }
    }
}
