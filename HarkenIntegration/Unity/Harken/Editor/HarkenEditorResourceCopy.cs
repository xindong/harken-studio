using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Harken.Unity.Editor
{
    public sealed partial class HarkenEditorCatalog
    {
        /// <summary>Copies Init, selected parent Packs and their published VKD references, preserving resource-root paths.
        /// Existing unselected files are untouched. On failure, copiedPaths identifies completed files.</summary>
        public VnResult CopyResources(string destinationRoot, IEnumerable<string> actionNames,
            out IReadOnlyList<string> copiedPaths, out string diagnostic)
        {
            var copied = new List<string>();
            copiedPaths = copied.AsReadOnly();
            diagnostic = string.Empty;
            if (snapshot == null) { diagnostic = Diagnostic; return LastResult; }
            if (resources == null || actionNames == null || string.IsNullOrWhiteSpace(destinationRoot))
            { diagnostic = "A presentation resource root, destination and Action selection are required."; return VnResult.VN_INVALID_ARG; }
            try
            {
                string[] names = actionNames.Distinct(StringComparer.Ordinal).ToArray();
                foreach (string name in names)
                    if (!snapshot.Actions.ContainsKey(name)) { diagnostic = "Unknown Action: " + name; return VnResult.VN_UNKNOWN_ID; }
                string destination = Path.GetFullPath(destinationRoot);
                HarkenEditorResources.RejectExistingRedirects(destination);
                Directory.CreateDirectory(destination);
                var written = new HashSet<string>(StringComparer.Ordinal);
                Action<string, byte[]> write = (relative, bytes) =>
                {
                    relative = HarkenEditorResources.Normalize(relative);
                    if (written.Contains(relative)) return;
                    if (bytes == null) throw new FileNotFoundException(relative);
                    string target = HarkenEditorResources.FilePath(destination, relative);
                    string parent = Path.GetDirectoryName(target);
                    // Validate every existing parent before creating a child through it.
                    string cursor = destination;
                    foreach (string part in relative.Split('/').Take(relative.Split('/').Length - 1))
                    {
                        if ((File.GetAttributes(cursor) & FileAttributes.ReparsePoint) != 0) throw new IOException("Copy destination redirects.");
                        cursor = Path.Combine(cursor, part);
                        Directory.CreateDirectory(cursor);
                    }
                    if ((File.GetAttributes(parent) & FileAttributes.ReparsePoint) != 0 ||
                        (File.Exists(target) && (File.GetAttributes(target) & FileAttributes.ReparsePoint) != 0))
                        throw new IOException("Copy destination redirects.");
                    File.WriteAllBytes(target, bytes);
                    written.Add(relative);
                    copied.Add(relative);
                };
                VnResult captured = snapshot.Source.Capture(null, default, null, out CompanionInput init);
                if (captured != VnResult.VN_OK) { diagnostic = "Init: " + captured; return captured; }
                write(resources.Resolve("Init.json", "Init.bytes"), init.Bytes);
                write(resources.Resolve("Init.json", "Init.json"), snapshot.Init.Json);
                foreach (string name in names)
                {
                    var entry = snapshot.Actions[name];
                    captured = snapshot.Source.Capture(name, init.Facts.projectId, init.Bytes, out CompanionInput input);
                    if (captured != VnResult.VN_OK) { diagnostic = name + ": " + captured; return captured; }
                    write(resources.Resolve(entry.Path + ".json", Path.GetFileName(entry.Path) + ".bytes"), input.Bytes);
                    write(resources.Resolve(entry.Path + ".json", Path.GetFileName(entry.Path) + ".json"), entry.Input.Json);
                    foreach (var reference in References(entry).Values)
                    {
                        string path = resources.Resolve(entry.Path + ".json", reference.File);
                        byte[] bytes = resources.Read(path, HarkenVkd.ByteLimit);
                        HarkenVkd.Read(bytes, path);
                        write(path, bytes);
                    }
                }
                return VnResult.VN_OK;
            }
            catch (Exception ex) { diagnostic = ex.Message; return VnResult.VN_PACK_PROVIDER_FAILED; }
        }
    }
}
