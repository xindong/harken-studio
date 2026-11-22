using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;

namespace Harken
{
    // Browsing facts do not install names or retain every Action's media payload.
    internal sealed class ActionCatalogSnapshot
    {
        internal sealed class Entry
        {
            internal PackMetadataInput Input;
            internal byte[] Digest;
            internal long ByteCount;
            internal VnPackOccurrenceFactV1[] Occurrences;
            internal VnPackMediaFactV1[] Media;
            internal string Path => (Input.Names.Language.Length == 0 ? "" : Input.Names.Language + "/") + Input.Names.Name;
        }

        internal readonly Dictionary<string, Entry> Actions = new Dictionary<string, Entry>(StringComparer.Ordinal);
        internal readonly Dictionary<ulong, string> Names = new Dictionary<ulong, string>();
        internal PackMetadataInput Init;
        internal PackSource Source;
        internal ActionCatalog Catalog;
        private PackSource origin;
        private byte[] initDigest;

        internal static VnResult Capture(PackSource source, IEnumerable<string> candidates,
            out ActionCatalogSnapshot snapshot, out string diagnostic)
        {
            snapshot = null;
            diagnostic = "A synchronous Pack source and Action candidates are required.";
            if (source == null || candidates == null || source.IsAsync) return VnResult.VN_INVALID_ARG;
            try
            {
                var next = new ActionCatalogSnapshot { origin = source };
                VnResult result = source.Capture(null, default, null, out next.Init);
                if (result != VnResult.VN_OK) { diagnostic = "Init: " + result; return result; }
                next.initDigest = Hash(next.Init.Bytes);
                var roots = new HashSet<VnObjectId>();
                foreach (string name in candidates.Distinct(StringComparer.Ordinal).OrderBy(n => n, StringComparer.Ordinal))
                {
                    if (!PackMetadata.IsName(name) || name == "Init")
                    { diagnostic = "Invalid Action candidate: " + name; return VnResult.VN_INVALID_ARG; }
                    result = source.Capture(name, next.Init.Facts.projectId, next.Init.Bytes, out PackMetadataInput input);
                    if (result != VnResult.VN_OK) { diagnostic = name + ": " + result; return result; }
                    result = PackMetadataNative.Inspect(input.Bytes, next.Init.Bytes, input.Names,
                        out input.Facts, out var occurrences, out var media);
                    if (result != VnResult.VN_OK) { diagnostic = name + ": " + result; return result; }
                    if (input.Facts.packStableId == next.Init.Facts.packStableId ||
                        next.Names.ContainsKey(input.Facts.packStableId) || !roots.Add(input.Names.RootId))
                    { diagnostic = "Duplicate Pack or Action identity: " + name; return VnResult.VN_BAD_PACK; }
                    var entry = new Entry { Input = input, ByteCount = input.Bytes.LongLength,
                        Digest = Hash(input.Bytes), Occurrences = occurrences, Media = media };
                    input.Bytes = null;
                    next.Actions.Add(name, entry);
                    next.Names.Add(input.Facts.packStableId, name);
                }
                next.Catalog = new ActionCatalog(next.Init.Facts.projectId.ToString(), source.Language,
                    next.Init.Facts.packStableId, next.Actions.Select(row => new ActionCatalogEntry(
                        row.Key, row.Value.Input.Names.RootId, row.Value.Input.Facts.packStableId)).ToArray());
                next.Source = PackSource.FromSnapshot(source.Language, next.Read, input => next.origin.Validate(input));
                snapshot = next;
                diagnostic = string.Empty;
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { diagnostic = "Catalog capacity exceeded."; return VnResult.VN_DEVICE_ERROR; }
            catch (Exception ex) { diagnostic = ex.Message; return VnResult.VN_PACK_PROVIDER_FAILED; }
        }

        private PackMetadataCapture Read(string name, VnObjectId project, byte[] init)
        {
            if (name == null)
            {
                VnResult initResult = origin.Capture(null, default, null, out PackMetadataInput fresh);
                if (initResult != VnResult.VN_OK) return new PackMetadataCapture(initResult);
                return Equal(initDigest, Hash(fresh.Bytes)) && SameNames(Init.Names, fresh.Names)
                    ? new PackMetadataCapture(VnResult.VN_OK, fresh)
                    : new PackMetadataCapture(VnResult.VN_PACK_STALE_REVISION);
            }
            if (!Actions.TryGetValue(name, out Entry expected)) return new PackMetadataCapture(VnResult.VN_UNKNOWN_ID);
            VnResult result = origin.Capture(name, project, init, out PackMetadataInput input);
            if (result != VnResult.VN_OK) return new PackMetadataCapture(result);
            if (!Equal(expected.Digest, Hash(input.Bytes)) || !SameNames(expected.Input.Names, input.Names))
                return new PackMetadataCapture(VnResult.VN_PACK_STALE_REVISION);
            return new PackMetadataCapture(VnResult.VN_OK, input);
        }

        // Existing sessions retain this snapshot and consult the refreshed equivalent reader.
        internal void RefreshOrigin(ActionCatalogSnapshot equivalent) => origin = equivalent.origin;

        internal bool SameExecution(ActionCatalogSnapshot other)
        {
            if (other == null || Source.Language != other.Source.Language || !Equal(initDigest, other.initDigest) ||
                !SameNames(Init.Names, other.Init.Names) || Actions.Count != other.Actions.Count) return false;
            foreach (var row in Actions)
                if (!other.Actions.TryGetValue(row.Key, out Entry entry) || !Equal(row.Value.Digest, entry.Digest) ||
                    !SameNames(row.Value.Input.Names, entry.Input.Names)) return false;
            return true;
        }

        private static bool SameNames(PackMetadata a, PackMetadata b)
        {
            if (a.ProjectId != b.ProjectId || a.RootId != b.RootId || a.Name != b.Name || a.Language != b.Language ||
                a.Variables.Length != b.Variables.Length || a.Labels.Length != b.Labels.Length) return false;
            for (int i = 0; i < a.Variables.Length; ++i)
                if (a.Variables[i].Id != b.Variables[i].Id || a.Variables[i].Name != b.Variables[i].Name) return false;
            for (int i = 0; i < a.Labels.Length; ++i)
                if (a.Labels[i].Id != b.Labels[i].Id || a.Labels[i].VariableId != b.Labels[i].VariableId ||
                    a.Labels[i].Name != b.Labels[i].Name) return false;
            return true;
        }
        private static byte[] Hash(byte[] bytes) { using (var hash = SHA256.Create()) return hash.ComputeHash(bytes); }
        private static bool Equal(byte[] a, byte[] b) => a.SequenceEqual(b);
    }
}
