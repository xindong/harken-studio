using System;
using System.Collections.Generic;

namespace Harken
{
    internal static class NameTable
    {
        internal readonly struct Action
        {
            internal readonly VnObjectId Id;
            internal readonly ulong Revision, Fingerprint;
            internal readonly PackBootstrapSession.SharedPack Owner;
            internal Action(VnObjectId id, ulong revision, ulong fingerprint, PackBootstrapSession.SharedPack owner)
            { Id = id; Revision = revision; Fingerprint = fingerprint; Owner = owner; }
            internal bool Live => Owner.Handle.IsValid;
        }

        internal sealed class Table
        {
            internal VnObjectId Project;
            internal ulong Generation;
            internal Dictionary<string, Action> Actions;
            internal Dictionary<string, VnObjectId> Variables;
            internal Dictionary<(VnObjectId, string), VnObjectId> Labels;
        }

        private static Table current;
        private static ulong generation;

        internal static ulong Generation => current?.Generation ?? 0;

        internal static Table Prepare(VnObjectId project, PackMetadata init,
            IEnumerable<KeyValuePair<string, Action>> actions)
        {
            var table = new Table
            {
                Project = project,
                Actions = new Dictionary<string, Action>(StringComparer.Ordinal),
                Variables = new Dictionary<string, VnObjectId>(StringComparer.Ordinal),
                Labels = new Dictionary<(VnObjectId, string), VnObjectId>()
            };
            foreach (var row in actions) table.Actions.Add(row.Key, row.Value);
            foreach (var row in init.Variables) table.Variables[row.Name] = row.Id;
            foreach (var row in init.Labels) table.Labels[(row.VariableId, row.Name)] = row.Id;
            return table;
        }

        internal static void Publish(Table table)
        {
            table.Generation = unchecked(++generation);
            current = table;
        }

        internal static void Clear() => current = null;

        internal static VnResult FindAction(string name, VnObjectId project, out Action action)
        {
            action = default;
            Table table = current;
            if (table == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            if (!project.IsNull && project != table.Project) return VnResult.VN_GAMEPLAY_WRONG_PROJECT;
            return table.Actions.TryGetValue(name, out action) && action.Live ? VnResult.VN_OK : VnResult.VN_UNKNOWN_ID;
        }

        internal static VnResult FindAction(VnObjectId id, out Action action)
        {
            action = default;
            Table table = current;
            if (table == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            foreach (Action row in table.Actions.Values)
                if (row.Id == id && row.Live) { action = row; return VnResult.VN_OK; }
            return VnResult.VN_UNKNOWN_ID;
        }

        internal static VnResult FindVariable(string name, VnObjectId project, out VnObjectId id)
        {
            id = default;
            Table table = current;
            if (table == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            if (!project.IsNull && project != table.Project) return VnResult.VN_GAMEPLAY_WRONG_PROJECT;
            return table.Variables.TryGetValue(name, out id) ? VnResult.VN_OK : VnResult.VN_UNKNOWN_ID;
        }

        internal static VnResult FindLabel(VnObjectId variable, string name, out VnObjectId id)
        {
            id = default;
            Table table = current;
            if (table == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            return table.Labels.TryGetValue((variable, name), out id) ? VnResult.VN_OK : VnResult.VN_UNKNOWN_ID;
        }
    }
}
