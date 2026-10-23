using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

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
        public bool IsValid => !string.IsNullOrEmpty(Name) && PackStableId != 0 &&
            PackManifest.IsSafeRelativePath(Path) && PackManifest.IsPlatformId(PlatformId);
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
        public bool IsValid => PackManifest.IsPlatformId(Id) &&
            PackManifest.IsPlatformName(Name) && PackManifest.IsBasePlatform(BasePlatform) && Path == Name;
    }

    public static partial class PackManifest
    {
        public const int SchemaVersion = 3;
        public const string IndexFileName = "HarkenPackIndex.json";
        public const string ManifestFileName = "HarkenPackManifest.json";

        public static bool IsPlatformId(string id) => Guid.TryParseExact(id, "D", out Guid value) &&
            value != Guid.Empty && value.ToString("D") == id;

        public static bool IsBasePlatform(string value) => value == "Windows" || value == "macOS" ||
            value == "Linux" || value == "Android" || value == "iOS";

        public static bool IsPlatformName(string name)
        {
            if (string.IsNullOrEmpty(name) || name != name.Trim() || name.EndsWith(".", StringComparison.Ordinal) ||
                Encoding.UTF8.GetByteCount(name) > 120 ||
                Regex.IsMatch(name, @"[\x00-\x1f\x7f-\x9f<>:""/\\|?*]") ||
                Regex.IsMatch(name, @"^(CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(?:\.|$)", RegexOptions.IgnoreCase))
                return false;
            for (int i = 0; i < name.Length; ++i)
                if (char.IsSurrogate(name[i]) &&
                    (!char.IsHighSurrogate(name[i]) || ++i >= name.Length || !char.IsLowSurrogate(name[i]))) return false;
            return true;
        }

        public static bool TryParseIndex(string json, out PackIndexEntry[] platforms)
        {
            platforms = Array.Empty<PackIndexEntry>();
            if (!TryRoot(json, out Dictionary<string, object> root) ||
                !OnlyKeys(root, "schema_version", "platforms") ||
                !JsonArray(root, "platforms", out List<object> values)) return false;
            var rows = new List<PackIndexEntry>();
            var ids = new HashSet<string>(StringComparer.Ordinal);
            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (object value in values)
            {
                if (!(value is Dictionary<string, object> row) ||
                    !OnlyKeys(row, "id", "name", "basePlatform", "path")) return false;
                var entry = new PackIndexEntry(Text(row, "id"), Text(row, "name"),
                    Text(row, "basePlatform"), Text(row, "path"));
                if (!entry.IsValid || !ids.Add(entry.Id) || !names.Add(entry.Name.Normalize())) return false;
                rows.Add(entry);
            }
            platforms = rows.ToArray();
            return true;
        }

        public static bool TryParseManifest(string json, out PackManifestEntry[] packs) =>
            TryParseManifest(json, out _, out packs);

        public static bool TryParseManifest(string json, out PackIndexEntry platform,
            out PackManifestEntry[] packs)
        {
            platform = default;
            packs = Array.Empty<PackManifestEntry>();
            if (!TryRoot(json, out Dictionary<string, object> root) ||
                !OnlyKeys(root, "schema_version", "platformId", "platformName", "basePlatform", "packs",
                    "initPackStableId", "language", "groups", "catalogChecksum", "gameplayCatalog", "variableCatalog") ||
                !JsonArray(root, "packs", out List<object> values) || values.Count == 0) return false;
            var definition = new PackIndexEntry(Text(root, "platformId"), Text(root, "platformName"),
                Text(root, "basePlatform"), Text(root, "platformName"));
            if (!definition.IsValid || !UInt64(Text(root, "initPackStableId"), out ulong initId) || initId == 0) return false;
            var rows = new List<PackManifestEntry>();
            var ids = new HashSet<ulong>();
            var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (object value in values)
            {
                if (!(value is Dictionary<string, object> row) ||
                    !OnlyKeys(row, "name", "packStableId", "path", "platformId", "packKind", "language",
                        "contentHash", "contentRevision", "variantKey", "dependencies") ||
                    !UInt64(Text(row, "packStableId"), out ulong id) ||
                    !UInt64(Text(row, "contentHash"), out ulong hash) ||
                    !JsonArray(row, "dependencies", out List<object> dependencies)) return false;
                var deps = new List<string>();
                foreach (object dependency in dependencies)
                {
                    if (!(dependency is string text) || !UInt64(text, out ulong depId) || depId == 0) return false;
                    deps.Add(text);
                }
                ulong variant = 0;
                if (row.ContainsKey("variantKey") &&
                    (!UInt64(Text(row, "variantKey"), out variant) || variant > uint.MaxValue)) return false;
                var entry = new PackManifestEntry(Text(row, "name"), id, Text(row, "path"),
                    Text(row, "platformId"), hash, deps.ToArray(), (uint)variant);
                if (!entry.IsValid || entry.PlatformId != definition.Id || !ids.Add(id) || !paths.Add(entry.Path)) return false;
                rows.Add(entry);
            }
            if (!ids.Contains(initId)) return false;
            if (rows.Any(row => row.Dependencies.Any(dep => !ids.Contains(ulong.Parse(dep, CultureInfo.InvariantCulture))))) return false;
            platform = definition;
            packs = rows.ToArray();
            return true;
        }

        public static bool TrySelectPlatform(PackIndexEntry[] platforms, string platformId,
            string platformName, string basePlatform, out PackIndexEntry selected, out string error)
        {
            selected = default;
            error = null;
            bool byId = !string.IsNullOrEmpty(platformId);
            bool byName = !string.IsNullOrEmpty(platformName);
            if (platforms == null || (byId && byName) || (byId && !IsPlatformId(platformId)) ||
                (byName && !IsPlatformName(platformName)) || (!byId && !byName && !IsBasePlatform(basePlatform)))
            {
                error = "Specify one platform ID or instance name, or a supported base platform for automatic selection.";
                return false;
            }
            var matches = new List<PackIndexEntry>();
            foreach (var platform in platforms)
            {
                if (!platform.IsValid) { error = "Invalid Pack platform definition."; return false; }
                if (byId ? platform.Id == platformId : byName ? platform.Name == platformName : platform.BasePlatform == basePlatform)
                    matches.Add(platform);
            }
            if (matches.Count != 1)
            {
                error = matches.Count == 0 ? "No published Pack platform matches the requested selection." :
                    "Multiple Pack platforms match " + basePlatform + ". Specify a platform ID or instance name: " +
                    string.Join(", ", matches.ConvertAll(item => item.Name + " (" + item.Id + ")"));
                return false;
            }
            selected = matches[0];
            return true;
        }

        public static bool MatchesPlatform(PackIndexEntry expected, PackIndexEntry actual) =>
            expected.Id == actual.Id && expected.Name == actual.Name &&
            expected.BasePlatform == actual.BasePlatform && expected.Path == actual.Path;

        public static bool TryResolvePlatformDirectory(string exportRoot, string platformName, out string platformDirectory) =>
            TryResolvePlatformDirectory(exportRoot, null, platformName, null, out platformDirectory, out _);

        public static bool TryResolvePlatformDirectory(string exportRoot, string platformId, string platformName,
            string basePlatform, out string platformDirectory, out string error)
            => TryResolvePlatformDirectory(exportRoot, platformId, platformName, basePlatform, null,
                out platformDirectory, out error);

        public static bool TryResolvePlatformDirectory(string exportRoot, string platformId, string platformName,
            string basePlatform, string language, out string platformDirectory, out string error)
            => ResolvePlatformDirectory(exportRoot, platformId, platformName, basePlatform, language,
                true, out platformDirectory, out error);

        public static bool TryReadPlatformDirectory(string exportRoot, string platformId, string platformName,
            string basePlatform, string language, out string platformDirectory, out string error)
            => ResolvePlatformDirectory(exportRoot, platformId, platformName, basePlatform, language,
                false, out platformDirectory, out error);

        private static bool ResolvePlatformDirectory(string exportRoot, string platformId, string platformName,
            string basePlatform, string language, bool verifyFiles, out string platformDirectory, out string error)
        {
            platformDirectory = null;
            error = "Pack index or manifest is missing, invalid, or inconsistent. Rebuild the selected platform.";
            if (string.IsNullOrEmpty(exportRoot)) return false;
            try
            {
                string indexJson = ReadOptionalIndex(Path.Combine(exportRoot, IndexFileName));
                if (indexJson != null)
                {
                    if (!TryParseIndex(indexJson, out PackIndexEntry[] platforms)) return false;
                    if (!TrySelectPlatform(platforms, platformId, platformName, basePlatform, out PackIndexEntry selected, out error)) return false;
                    error = "Pack index and manifest identities or files do not match. Rebuild " + selected.Name + ".";
                    if (!TryJoinContainedPath(exportRoot, selected.Path, out string path) ||
                        !ReadManifest(path, language, verifyFiles, out PackIndexEntry actual, out _) || !MatchesPlatform(selected, actual)) return false;
                    platformDirectory = path;
                }
                else
                {
                    if (!ReadManifest(exportRoot, language, verifyFiles, out PackIndexEntry actual, out _) ||
                        !TrySelectPlatform(new[] { actual }, platformId, platformName, basePlatform, out _, out error)) return false;
                    platformDirectory = Path.GetFullPath(exportRoot);
                }
                error = null;
                return true;
            }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
            catch (ArgumentException) { return false; }
        }

        private static string ReadOptionalIndex(string path)
        {
            try { return File.ReadAllText(path); }
            catch (FileNotFoundException) { return null; }
            catch (DirectoryNotFoundException) { return null; }
        }

        public static bool TryResolvePackPath(string platformDirectory, ulong packStableId, out string packPath)
            => TryResolvePackPath(platformDirectory, packStableId, null, out packPath);

        public static bool TryResolvePackPath(string platformDirectory, ulong packStableId,
            string language, out string packPath)
        {
            packPath = null;
            if (packStableId == 0 || !TryReadManifest(platformDirectory, language, out _,
                    out PackManifestEntry[] packs, out _)) return false;
            var pack = packs.FirstOrDefault(entry => entry.PackStableId == packStableId);
            return pack.PackStableId != 0 && VerifyPackFile(platformDirectory, pack) &&
                TryJoinContainedPath(platformDirectory, pack.Path, out packPath);
        }

        public static bool TryLoadManifest(string platformDirectory, out PackManifestEntry[] packs) =>
            TryLoadManifest(platformDirectory, out _, out packs);

        public static bool TryLoadManifest(string platformDirectory, out PackIndexEntry platform, out PackManifestEntry[] packs)
            => TryLoadManifest(platformDirectory, null, out platform, out packs);

        public static bool TryLoadManifest(string platformDirectory, string language,
            out PackIndexEntry platform, out PackManifestEntry[] packs)
            => ReadManifest(platformDirectory, language, true, out platform, out packs);

        private static bool ReadManifest(string platformDirectory, string language, bool verifyFiles,
            out PackIndexEntry platform, out PackManifestEntry[] packs)
        {
            if (!TryReadManifest(platformDirectory, language, out platform, out packs, out _)) return false;
            if (!verifyFiles || packs.All(pack => VerifyPackFile(platformDirectory, pack))) return true;
            platform = default;
            packs = Array.Empty<PackManifestEntry>();
            return false;
        }

        public static bool TryReadManifest(string platformDirectory, string language,
            out PackIndexEntry platform, out PackManifestEntry[] packs, out string publicationLanguage)
            => TryReadManifest(platformDirectory, language, out platform, out packs,
                out publicationLanguage, out _);

        public static bool TryReadManifest(string platformDirectory, string language,
            out PackIndexEntry platform, out PackManifestEntry[] packs, out string publicationLanguage,
            out ulong initPackStableId)
        {
            platform = default;
            packs = Array.Empty<PackManifestEntry>();
            publicationLanguage = null;
            initPackStableId = 0;
            if (string.IsNullOrEmpty(platformDirectory)) return false;
            try
            {
                string path = Path.Combine(platformDirectory, ManifestFileName);
                if (!File.Exists(path) || File.Exists(Path.Combine(platformDirectory, ".harken-pack-publishing"))) return false;
                string json = File.ReadAllText(path);
                if (!TryGetSelectedManifestPath(json, language, out string selectedRelativePath)) return false;
                string selectedJson = null;
                if (selectedRelativePath != null)
                {
                    if (!TryJoinContainedPath(platformDirectory, selectedRelativePath,
                            out string selectedPath) || !File.Exists(selectedPath)) return false;
                    selectedJson = File.ReadAllText(selectedPath);
                }
                return TrySelectManifest(json, selectedJson, language, out platform, out packs,
                    out publicationLanguage, out initPackStableId);
            }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
            catch (ArgumentException) { return false; }
        }

        public static bool TrySelectManifest(string inventoryJson, string selectedJson, string language,
            out PackIndexEntry platform, out PackManifestEntry[] packs, out string publicationLanguage,
            out ulong initPackStableId)
        {
            platform = default;
            packs = Array.Empty<PackManifestEntry>();
            publicationLanguage = null;
            initPackStableId = 0;
            if (!TryGetSelectedManifestPath(inventoryJson, language, out string selectedPath) ||
                !TryParseManifest(inventoryJson, out PackIndexEntry parsedPlatform, out PackManifestEntry[] parsed) ||
                !TryRoot(inventoryJson, out var inventory)) return false;
            string selectedLanguage = string.IsNullOrEmpty(language) ? Text(inventory, "language") : language;
            if (selectedPath != null)
            {
                if (!TryParseManifest(selectedJson, out PackIndexEntry selectedPlatform, out var selected) ||
                    !MatchesPlatform(parsedPlatform, selectedPlatform) ||
                    !TryRoot(selectedJson, out var root) || Text(root, "language") != selectedLanguage ||
                    Text(root, "initPackStableId") != Text(inventory, "initPackStableId")) return false;
                ulong initId = ulong.Parse(Text(inventory, "initPackStableId"), CultureInfo.InvariantCulture);
                JsonArray(inventory, "packs", out var inventoryRows);
                var expected = new HashSet<ulong>();
                bool knownLanguage = false;
                foreach (Dictionary<string, object> row in inventoryRows)
                {
                    knownLanguage |= Text(row, "language") == selectedLanguage;
                    if (Text(row, "packStableId") == Text(inventory, "initPackStableId") ||
                        string.IsNullOrEmpty(Text(row, "language")) || Text(row, "language") == selectedLanguage)
                        expected.Add(ulong.Parse(Text(row, "packStableId"), CultureInfo.InvariantCulture));
                }
                if (!knownLanguage || selected.Length != expected.Count) return false;
                foreach (var pack in selected)
                {
                    var original = parsed.FirstOrDefault(item => item.PackStableId == pack.PackStableId);
                    if (!expected.Remove(pack.PackStableId) || original.Name != pack.Name ||
                        !original.Dependencies.SequenceEqual(pack.Dependencies)) return false;
                    if (pack.PackStableId == initId)
                    {
                        if (pack.Path != original.Path && pack.Path != selectedLanguage + "/" + original.Path) return false;
                        if (pack.Path == original.Path && pack.ContentHash != original.ContentHash) return false;
                    }
                    else if (pack.Path != original.Path || pack.ContentHash != original.ContentHash ||
                        pack.VariantKey != original.VariantKey) return false;
                }
                parsed = selected;
            }
            else if (selectedJson != null) return false;
            platform = parsedPlatform;
            packs = parsed;
            publicationLanguage = selectedLanguage ?? string.Empty;
            initPackStableId = ulong.Parse(Text(inventory, "initPackStableId"), CultureInfo.InvariantCulture);
            return true;
        }

        public static bool TryGetSelectedManifestPath(string inventoryJson, string language,
            out string relativePath)
        {
            relativePath = null;
            if (!TryParseManifest(inventoryJson, out _) || !TryRoot(inventoryJson, out var inventory)) return false;
            string selected = string.IsNullOrEmpty(language) ? Text(inventory, "language") : language;
            if (string.IsNullOrEmpty(selected) || !inventory.ContainsKey("gameplayCatalog"))
                return string.IsNullOrEmpty(language);
            if (!IsSafeRelativePath(selected) || selected.IndexOfAny(new[] { '/', '\\' }) >= 0) return false;
            relativePath = selected + "/" + ManifestFileName;
            return true;
        }

        public static bool VerifyPackBytes(byte[] bytes, PackManifestEntry pack)
        {
            if (!pack.IsValid || bytes == null || bytes.Length == 0) return false;
            using (var stream = new MemoryStream(bytes, false)) return Hash(stream) == pack.ContentHash;
        }

        public static bool VerifyPackFile(string platformDirectory, PackManifestEntry pack)
        {
            if (!pack.IsValid || !TryJoinContainedPath(platformDirectory, pack.Path, out string path)) return false;
            try
            {
                using (var stream = File.OpenRead(path))
                    return stream.Length != 0 && Hash(stream) == pack.ContentHash;
            }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
        }

        private static ulong Hash(Stream stream)
        {
            ulong hash = 14695981039346656037UL;
            byte[] buffer = new byte[65536];
            int count;
            while ((count = stream.Read(buffer, 0, buffer.Length)) != 0)
                hash = HashBytes(buffer.AsSpan(0, count), hash);
            return hash;
        }

        internal static ulong HashBytes(ReadOnlySpan<byte> bytes, ulong hash = 14695981039346656037UL)
        {
            foreach (byte value in bytes) hash = unchecked((hash ^ value) * 1099511628211UL);
            return hash;
        }

        public static bool TryJoinContainedPath(string root, string relativePath, out string resolved)
        {
            resolved = null;
            if (string.IsNullOrEmpty(root) || !IsSafeRelativePath(relativePath)) return false;
            string fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            string destination = Path.GetFullPath(Path.Combine(fullRoot, relativePath));
            StringComparison comparison = Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!destination.StartsWith(fullRoot, comparison)) return false;
            resolved = destination;
            return true;
        }

        public static bool IsSafeRelativePath(string value)
        {
            if (string.IsNullOrWhiteSpace(value) || Path.IsPathRooted(value) || value.IndexOf(':') >= 0 || value.IndexOf('\\') >= 0) return false;
            return !value.Split('/').Any(part => string.IsNullOrEmpty(part) || part == "." || part == ".." ||
                Regex.IsMatch(part, @"[\x00-\x1f<>:""|?*]"));
        }

        private static bool TryRoot(string json, out Dictionary<string, object> root)
        {
            root = null;
            try
            {
                var parser = new JsonParser(json);
                root = parser.Read(0) as Dictionary<string, object>;
                return parser.End && root != null && root.TryGetValue("schema_version", out object version) &&
                    ((version is ulong integer && integer == SchemaVersion) ||
                     (version is double number && number.Equals((double)SchemaVersion)));
            }
            catch (FormatException) { return false; }
            catch (ArgumentException) { return false; }
        }

        private static string Text(Dictionary<string, object> row, string key) =>
            row.TryGetValue(key, out object value) ? value as string : null;
        private static bool UInt64(string text, out ulong value) =>
            ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value);
        private static bool JsonArray(Dictionary<string, object> row, string key, out List<object> values)
        {
            values = row.TryGetValue(key, out object value) ? value as List<object> : null;
            return values != null;
        }
        private static bool OnlyKeys(Dictionary<string, object> row, params string[] keys)
        {
            return row.Keys.All(key => System.Array.IndexOf(keys, key) >= 0);
        }

    }
}
