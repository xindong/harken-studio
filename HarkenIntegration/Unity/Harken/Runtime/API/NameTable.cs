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
            internal PackBootstrapSession.SharedPack[] SmallActions;
            internal PackBootstrapSession.DirectoryLease Directory;
            internal Dictionary<string, VnObjectId> Variables;
            internal Dictionary<(VnObjectId, string), VnObjectId> Labels;
        }

        private static Table current;
        private static ulong generation;

        internal static PackBootstrapSession.DirectoryLease ActiveDirectory => current?.Directory;
        internal static ulong Generation => current?.Generation ?? 0;

        internal static Table PrepareInit(VnObjectId project, PackMetadata init)
        {
            var table = new Table
            {
                Project = project,
                Actions = new Dictionary<string, Action>(StringComparer.Ordinal),
                Variables = new Dictionary<string, VnObjectId>(StringComparer.Ordinal),
                Labels = new Dictionary<(VnObjectId, string), VnObjectId>()
            };
            foreach (var row in init.Variables) table.Variables[row.Name] = row.Id;
            foreach (var row in init.Labels) table.Labels[(row.VariableId, row.Name)] = row.Id;
            return table;
        }

        internal static Table PrepareActions(Table init, int capacity) => new Table
        {
            Project = init.Project,
            Variables = init.Variables,
            Labels = init.Labels,
            Actions = new Dictionary<string, Action>(capacity, StringComparer.Ordinal)
        };

        internal static void Publish(Table table)
        {
            table.Generation = unchecked(++generation);
            current = table;
        }

        internal static void Expand(PackBootstrapSession.DirectoryLease lease, Table table)
        {
            var previous = current;
            if (previous?.Directory != lease) return;
            table.Generation = previous.Generation;
            current = table;
        }

        internal static void Clear() => current = null;

        internal static VnResult FindAction(string name, VnObjectId project, out Action action)
        {
            action = default;
            Table table = current;
            if (table == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            if (!project.IsNull && project != table.Project) return VnResult.VN_GAMEPLAY_WRONG_PROJECT;
            if (table.Directory != null) return table.Directory.Find(name, default, out action);
            if (table.SmallActions != null)
            {
                foreach (var owner in table.SmallActions)
                    if (owner.Names.Name == name && owner.Handle.IsValid)
                    {
                        action = owner.Action;
                        return VnResult.VN_OK;
                    }
                return VnResult.VN_UNKNOWN_ID;
            }
            return table.Actions.TryGetValue(name, out action) && action.Live ? VnResult.VN_OK : VnResult.VN_UNKNOWN_ID;
        }

        internal static VnResult FindAction(VnObjectId id, out Action action)
        {
            action = default;
            Table table = current;
            if (table == null) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            if (table.Directory != null) return table.Directory.Find(null, id, out action);
            if (table.SmallActions != null)
            {
                foreach (var owner in table.SmallActions)
                    if (owner.Names.RootId == id && owner.Handle.IsValid)
                    {
                        action = owner.Action;
                        return VnResult.VN_OK;
                    }
                return VnResult.VN_UNKNOWN_ID;
            }
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
