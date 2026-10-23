using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using Harken;

namespace Harken.Test
{
    internal static class PackPlatformContract
    {
        internal static readonly PackIndexEntry Windows = new PackIndexEntry(
            "11111111-2222-3333-4444-555555555555", "Windows", "Windows", "Windows");

        private static ulong Hash(byte[] bytes)
        {
            ulong value = 14695981039346656037UL;
            foreach (byte b in bytes) value = unchecked((value ^ b) * 1099511628211UL);
            return value;
        }

        internal static string Index(params PackIndexEntry[] platforms) => JsonSerializer.Serialize(new
        {
            schema_version = PackManifest.SchemaVersion,
            platforms = platforms.Select(p => new { id = p.Id, name = p.Name, basePlatform = p.BasePlatform, path = p.Path })
        });

        internal static string Manifest(PackIndexEntry platform, byte[] init, byte[] content) => JsonSerializer.Serialize(new
        {
            schema_version = PackManifest.SchemaVersion,
            platformId = platform.Id,
            platformName = platform.Name,
            basePlatform = platform.BasePlatform,
            initPackStableId = "11",
            packs = new[]
            {
                new { name = "Init", packStableId = "11", path = "Init.pack", platformId = platform.Id,
                    contentHash = Hash(init).ToString(), dependencies = Array.Empty<string>() },
                new { name = "Content", packStableId = "22", path = "Content.pack", platformId = platform.Id,
                    contentHash = Hash(content).ToString(), dependencies = new[] { "11" } }
            }
        });

        private static string LanguageManifest(byte[] init, byte[] content, string language, bool inventory)
        {
            var rows = new[]
            {
                new { name = "Init", packStableId = "11", path = inventory ? "Init.pack" : language + "/Init.pack",
                    platformId = Windows.Id, language = "", contentHash = Hash(init).ToString(), dependencies = Array.Empty<string>() },
                new { name = "Content", packStableId = "22", path = "English/Content.pack",
                    platformId = Windows.Id, language = "English", contentHash = Hash(content).ToString(), dependencies = new[] { "11", "44" } },
                new { name = "Content", packStableId = "33", path = "ja/Content.pack",
                    platformId = Windows.Id, language = "ja", contentHash = Hash(content).ToString(), dependencies = new[] { "11", "44" } },
                new { name = "Common", packStableId = "44", path = "Common.pack",
                    platformId = Windows.Id, language = "", contentHash = Hash(content).ToString(), dependencies = new[] { "11" } }
            };
            return JsonSerializer.Serialize(new {
                schema_version = PackManifest.SchemaVersion, platformId = Windows.Id, platformName = Windows.Name,
                basePlatform = Windows.BasePlatform, initPackStableId = "11", language,
                gameplayCatalog = new { schema = 1 },
                packs = rows.Where(row => inventory || row.language == "" || row.language == language)
            });
        }

        internal static bool RunLanguageNative(string root)
        {
            // Canonical member locators must not repeat a relative containment root.
            root = Path.GetFullPath(root);
            foreach (string language in new[] { "English", "ja" })
            {
                if (!PackManifest.TryLoadManifest(root, language, out _, out var packs)) return false;
                var init = packs.Single(p => p.Name == "Init");
                var ids = packs.Where(p => p.Name != "Init").Select(p => p.PackStableId).ToArray();
                var config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                config.maxPacks = 8;
                config.maxActions = 64;
                config.maxResidentDefinitionBytes = 32UL << 20;
                var result = PackBootstrap.Start(Path.Combine(root, init.Path), root, ids, language,
                    ref config, out var session);
                if (result != VnResult.VN_OK) { Console.WriteLine("[LanguageNative] start " + language + ": " + result); return false; }
                using (session)
                {
                    using (var manifest = JsonDocument.Parse(File.ReadAllText(
                        Path.Combine(root, language, PackManifest.ManifestFileName))))
                    {
                        var catalog = manifest.RootElement.GetProperty("gameplayCatalog");
                        foreach (var row in catalog.GetProperty("actions").EnumerateArray())
                        {
                            string name = row.GetProperty("name").GetString();
                            VnObjectId id = VnObjectId.Parse(row.GetProperty("actionStableId").GetString());
                            result = Actions.Resolve(name, out var resolved);
                            if (result != VnResult.VN_OK || resolved.ActionId != id)
                            { Console.WriteLine("[LanguageNative] resolve " + name + ": " + result); return false; }
                            var pin = new ActionReference {
                                ExpectedName = name, ActionId = id,
                                ProjectGuid = Convert.FromHexString(catalog.GetProperty("projectId").GetString().Replace("-", "")),
                                NameBindingRevision = ulong.Parse(row.GetProperty("nameBindingRevision").GetString()),
                                CallableFingerprint = ulong.Parse(row.GetProperty("callableFingerprint").GetString())
                            };
                            for (int form = 0; form < 3; form++)
                            {
                                ActionFiring firing;
                                result = form == 0 ? Actions.Post(name, Emitter.None, out firing)
                                    : form == 1 ? Actions.Post(in resolved, Emitter.None, 0, out firing)
                                    : Actions.Post(in pin, Emitter.None, 0, out firing);
                                if (result != VnResult.VN_OK)
                                { Console.WriteLine("[LanguageNative] post form " + form + ": " + result); return false; }
                                if (firing.Query(out var info) != VnResult.VN_OK || info.actionId != id)
                                { Console.WriteLine("[LanguageNative] firing identity mismatch"); return false; }
                                var stopped = firing.Stop(0);
                                var released = firing.Release();
                                // Null-backend voices can retain the firing until rendering; session disposal drains ownership.
                                if (stopped != VnResult.VN_OK ||
                                    (released != VnResult.VN_OK && released != VnResult.VN_IN_USE))
                                { Console.WriteLine("[LanguageNative] stop/release " + stopped + "/" + released); return false; }
                            }
                            if (Actions.Post(name + "\0Other", Emitter.None, out var invalid) != VnResult.VN_INVALID_ARG || invalid.IsValid)
                                return false;
                            if (Actions.Resolve(name + "\0Other", out _) != VnResult.VN_INVALID_ARG)
                                return false;
                            pin.ExpectedName = name + "\0Other";
                            if (Actions.Post(in pin, Emitter.None, 0, out invalid) != VnResult.VN_INVALID_ARG || invalid.IsValid)
                                return false;
                        }
                    }
                    string other = language == "English" ? "ja" : "English";
                    if (!PackManifest.TryLoadManifest(root, other, out _, out var wrongPacks)) return false;
                    var wrong = wrongPacks.Single(p => p.Name != "Init");
                    var current = packs.Single(p => p.Name != "Init");
                    var retained = session.StartupPacks;
                    ulong revision = session.SetRevision;
                    Pack retainedInit = session.InitPack;
                    if (retained.Length != 1 ||
                        !PackManifest.TryResolvePackPath(root, current.PackStableId, language, out string currentPath) ||
                        !PackManifest.TryResolvePackPath(root, wrong.PackStableId, other, out string wrongPath)) return false;
                    var contents = new[] {
                        new CanonicalPackMember(current.PackStableId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                            currentPath, root, expected: retained[0]),
                        new CanonicalPackMember(wrong.PackStableId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                            wrongPath, root)
                    };
                    Array.Sort(contents, (left, right) => left.StableId.CompareTo(right.StableId));
                    var members = new[] {
                        new CanonicalPackMember(session.Inspection.PackStableId, VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT,
                            Path.Combine(root, init.Path), root, expected: retainedInit),
                        contents[0], contents[1]
                    };
                    result = Packs.ActivateCanonicalPackSet(members, revision + 1, out _, out var rows,
                        expectedCurrentInit: retainedInit, baseSetRevision: revision);
                    Console.WriteLine("[LanguageNative] wrong-language activate " + language + ": " + result);
                    if (result != VnResult.VN_PACK_STALE_REVISION || rows.Length != 0 ||
                        session.SetRevision != revision || session.InitPack.Handle != retainedInit.Handle ||
                        session.StartupPacks.Length != retained.Length ||
                        session.StartupPacks[0].Handle != retained[0].Handle) return false;
                    if (Actions.Resolve(current.Name, out var afterReject) != VnResult.VN_OK ||
                        Actions.Post(in afterReject, Emitter.None, 0, out var afterFiring) != VnResult.VN_OK ||
                        !afterFiring.IsValid || afterFiring.Stop(0) != VnResult.VN_OK) return false;
                    var afterReleased = afterFiring.Release();
                    if ((afterReleased != VnResult.VN_OK && afterReleased != VnResult.VN_IN_USE) ||
                        HarkenEngine.Update() != VnResult.VN_OK) return false;
                    result = session.Shutdown();
                    Console.WriteLine("[LanguageNative] shutdown " + language + ": " + result);
                    if (result != VnResult.VN_OK) return false;
                }
            }
            Console.WriteLine("[LanguageNative] both languages bootstrap and string/resolved/pinned posts match; embedded NUL and wrong-language activate refused.");
            return true;
        }

        internal static bool RunNative(string root)
        {
            if (!PackManifest.TryParseIndex(File.ReadAllText(Path.Combine(root, PackManifest.IndexFileName)), out var platforms) ||
                platforms.Length != 2 || platforms.Any(p => p.BasePlatform != "Android")) return false;
            if (PackManifest.TryResolvePlatformDirectory(root, null, null, "Android", out _, out _)) return false;
            foreach (var platform in platforms)
            {
                if (!PackManifest.TryResolvePlatformDirectory(root, platform.Id, null, null, out string directory, out string error) ||
                    !PackManifest.TryLoadManifest(directory, out var packs))
                {
                    Console.WriteLine("[PackPlatformNative] FAIL: " + error);
                    return false;
                }
                var init = packs.Single(p => p.Name == "Init");
                var contentIds = packs.Where(p => p.Name != "Init").Select(p => p.PackStableId).ToArray();
                var config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                config.maxPacks = 8;
                config.maxActions = 64;
                config.maxResidentDefinitionBytes = 32UL << 20;
                var result = PackBootstrap.Start(Path.Combine(directory, init.Path), directory,
                    contentIds, ref config, out var session);
                if (result != VnResult.VN_OK)
                {
                    Console.WriteLine("[PackPlatformNative] FAIL: " + platform.Name + " " + result);
                    return false;
                }
                if (HarkenEngine.Update() != VnResult.VN_OK)
                {
                    session.Shutdown();
                    return false;
                }
                if (session.Shutdown() != VnResult.VN_OK) return false;
                Console.WriteLine("[PackPlatformNative] Loaded " + platform.Name + " (" + platform.Id + ")");
            }
            return true;
        }

        internal static bool Run()
        {
            int failures = 0;
            int checks = 0;
            void Check(bool pass, string name)
            {
                ++checks;
                if (!pass) { ++failures; Console.WriteLine("[PackPlatform] FAIL: " + name); }
            }
            byte[] init = { 1, 2, 3 };
            byte[] content = { 4, 5, 6 };
            string index = Index(Windows);
            string manifest = Manifest(Windows, init, content);
            string currentVersion = PackManifest.SchemaVersion.ToString(System.Globalization.CultureInfo.InvariantCulture);
            string versionPrefix = "\"schema_version\":" + currentVersion;
            foreach (string version in new[] { currentVersion, currentVersion + ".0", currentVersion + "e0" })
            {
                Check(PackManifest.TryParseIndex(index.Replace(versionPrefix, "\"schema_version\":" + version), out _), "index numeric version " + version);
                Check(PackManifest.TryParseManifest(manifest.Replace(versionPrefix, "\"schema_version\":" + version), out _), "manifest numeric version " + version);
            }
            foreach (string version in new[] { "0", "1", "2", "4", "6", "\"3\"", "null", "3.5", "3.0000000001", "2.9999999999",
                "3.0000000000000004", "2.9999999999999996", "1e309", "03", "3." })
            {
                Check(!PackManifest.TryParseIndex(index.Replace(versionPrefix, "\"schema_version\":" + version), out _), "index rejects version " + version);
                Check(!PackManifest.TryParseManifest(manifest.Replace(versionPrefix, "\"schema_version\":" + version), out _), "manifest rejects version " + version);
            }
            Check(!PackManifest.TryParseIndex("{\"schema_version\":1,\"platforms\":[{\"name\":\"Windows\",\"path\":\"Windows\"}]}", out _), "old index rejected");
            Check(!PackManifest.TryParseManifest("{\"schema_version\":1,\"packs\":[{\"name\":\"Init\",\"path\":\"Init.pack\",\"packStableId\":\"11\"}]}", out _), "old manifest rejected");
            Check(PackManifest.TryParseManifest(manifest.Replace("\"packs\":", "\"gameplayCatalog\":{\"schema\":1},\"packs\":"), out var located) && located.Length == 2, "gameplayCatalog still locates packs");
            Check(PackManifest.TryParseManifest(manifest.Replace("\"packs\":",
                "\"gameplayCatalog\":{\"schema\":1},\"variableCatalog\":{\"variables\":[]},\"packs\":"),
                out var combined) && combined.Length == 2, "combined catalogs preserve file location");
            Check(!PackManifest.TryParseIndex(index.Replace(versionPrefix, versionPrefix + "," + versionPrefix), out _), "duplicate key rejected");
            Check(!PackManifest.TryParseManifest(manifest.Replace("\"platformName\":", "\"platform\":\"Windows\",\"platformName\":"), out _), "mixed fields rejected");
            Check(!PackManifest.TryParseIndex(Index(Windows, Windows), out _), "duplicate ID rejected");
            Check(PackManifest.TryParseIndex(Index(), out var empty) && !PackManifest.TrySelectPlatform(empty, null, null, "Windows", out _, out _), "empty published set has no candidate");

            var low = new PackIndexEntry("22222222-2222-3333-4444-555555555555", "Android", "Android", "Android");
            var high = new PackIndexEntry("33333333-2222-3333-4444-555555555555", "Android_High", "Android", "Android_High");
            var platforms = new[] { Windows, low, high };
            Check(!PackManifest.TrySelectPlatform(platforms, null, null, "Android", out _, out string error) &&
                error.Contains(low.Id) && error.Contains(high.Id), "automatic Android selection reports both candidates");
            Check(PackManifest.TrySelectPlatform(platforms, high.Id, null, "Windows", out var selected, out _) && selected.Id == high.Id, "explicit ID");
            Check(PackManifest.TrySelectPlatform(platforms, null, "Android", "Windows", out selected, out _) && selected.Id == low.Id, "explicit name");
            Check(PackManifest.TrySelectPlatform(platforms, null, null, "Windows", out selected, out _) && selected.Id == Windows.Id, "unique base");
            Check(!PackManifest.TrySelectPlatform(platforms, null, null, "iOS", out _, out _), "missing base");
            Check(!PackManifest.TrySelectPlatform(platforms, "Android", null, "Android", out _, out _), "base is not ID");
            Check(!PackManifest.TrySelectPlatform(platforms, low.Id, low.Name, "Android", out _, out _), "mixed selection refused");
            var unicode = new PackIndexEntry(high.Id, "Mobile_\u97f3\u9891_[HQ]", "Android", "Mobile_\u97f3\u9891_[HQ]");
            Check(PackManifest.TryParseIndex(Index(unicode), out var parsed) && parsed[0].Name == unicode.Name, "escaped Unicode and brackets");
            Check(!PackManifest.TryParseIndex(Index(new PackIndexEntry(high.Id, low.Name.ToLowerInvariant(), "Android", low.Name.ToLowerInvariant()), low), out _), "case-insensitive directory collision");

            string root = Path.GetFullPath(Path.Combine(".temp", "sdk-pack-platform-" + Guid.NewGuid().ToString("N")));
            try
            {
                foreach (var platform in platforms)
                {
                    string dir = Path.Combine(root, platform.Name);
                    Directory.CreateDirectory(dir);
                    File.WriteAllBytes(Path.Combine(dir, "Init.pack"), init);
                    File.WriteAllBytes(Path.Combine(dir, "Content.pack"), content);
                    File.WriteAllText(Path.Combine(dir, PackManifest.ManifestFileName), Manifest(platform, init, content));
                }
                File.WriteAllText(Path.Combine(root, PackManifest.IndexFileName), Index(platforms));
                Check(!PackManifest.TryResolvePlatformDirectory(root, null, null, "Android", out _, out _), "filesystem ambiguity");
                Check(PackManifest.TryResolvePlatformDirectory(root, high.Id, null, null, out string highDir, out _) && highDir == Path.Combine(root, high.Name), "filesystem ID");
                string currentIndex = Path.Combine(root, PackManifest.IndexFileName);
                File.WriteAllText(currentIndex, "{");
                Check(!PackManifest.TryResolvePlatformDirectory(root, high.Id, null, null, out _, out _),
                    "invalid index rejected");
                File.Delete(currentIndex);
                Check(!PackManifest.TryResolvePlatformDirectory(root, high.Id, null, null, out _, out _),
                    "missing index cannot select a platform subdirectory");
                File.WriteAllText(currentIndex, Index(platforms));
                Check(PackManifest.TryResolvePackPath(highDir, 22, out string path) && path == Path.Combine(highDir, "Content.pack"), "pack path stays in selected instance");
                File.WriteAllText(Path.Combine(highDir, PackManifest.ManifestFileName), Manifest(low, init, content));
                Check(!PackManifest.TryResolvePlatformDirectory(root, high.Id, null, null, out _, out _), "swapped manifest rejected");
                File.WriteAllText(Path.Combine(highDir, PackManifest.ManifestFileName), Manifest(high, init, content));
                File.WriteAllBytes(Path.Combine(highDir, "Init.pack"), content);
                Check(!PackManifest.TryResolvePlatformDirectory(root, high.Id, null, null, out _, out _), "swapped Init rejected");
                File.WriteAllBytes(Path.Combine(highDir, "Init.pack"), init);
                File.WriteAllText(Path.Combine(highDir, ".harken-pack-publishing"), "");
                Check(!PackManifest.TryResolvePlatformDirectory(root, high.Id, null, null, out _, out _), "publishing marker refused");
                File.Delete(Path.Combine(highDir, ".harken-pack-publishing"));
                File.WriteAllText(Path.Combine(highDir, PackManifest.ManifestFileName), Manifest(high, init, content).Replace("Content.pack", "../Content.pack"));
                Check(!PackManifest.TryResolvePlatformDirectory(root, high.Id, null, null, out _, out _), "path escape rejected");
                File.WriteAllText(Path.Combine(root, PackManifest.IndexFileName), Index(Windows, low));
                Check(!PackManifest.TryResolvePlatformDirectory(root, high.Id, null, null, out _, out _), "retired directory is not discovered");
                Check(PackManifest.TryResolvePlatformDirectory(Path.Combine(root, low.Name), low.Id, null, null, out _, out _), "explicit directory checks identity");
                Check(!PackManifest.TryResolvePlatformDirectory(Path.Combine(root, low.Name), high.Id, null, null, out _, out _), "explicit directory cannot impersonate another ID");

                string localized = Path.Combine(root, Windows.Name);
                File.WriteAllText(Path.Combine(localized, PackManifest.ManifestFileName), LanguageManifest(init, content, "English", true));
                File.WriteAllBytes(Path.Combine(localized, "Common.pack"), content);
                foreach (string language in new[] { "English", "ja" })
                {
                    string dir = Path.Combine(localized, language);
                    Directory.CreateDirectory(dir);
                    File.WriteAllBytes(Path.Combine(dir, "Init.pack"), init);
                    File.WriteAllBytes(Path.Combine(dir, "Content.pack"), content);
                    File.WriteAllText(Path.Combine(dir, PackManifest.ManifestFileName), LanguageManifest(init, content, language, false));
                }
                Check(PackManifest.TryLoadManifest(localized, out var defaults) && defaults.Length == 3 &&
                    defaults.Single(p => p.PackStableId == 11).Path == "English/Init.pack", "default language selects matching Init and common dependencies");
                Check(PackManifest.TryLoadManifest(localized, "ja", out _, out var japanese) && japanese.Length == 3 &&
                    japanese.Any(p => p.PackStableId == 33) && !japanese.Any(p => p.PackStableId == 22), "explicit language selects only its owners");
                Check(PackManifest.TryResolvePackPath(localized, 11, "ja", out var japaneseInit) &&
                    japaneseInit == Path.Combine(localized, "ja", "Init.pack"), "language-aware path resolution");
                Check(!PackManifest.TryLoadManifest(localized, "French", out _, out _), "unknown language refused");
                Directory.CreateDirectory(Path.Combine(localized, "French"));
                File.WriteAllBytes(Path.Combine(localized, "French", "Init.pack"), init);
                File.WriteAllText(Path.Combine(localized, "French", PackManifest.ManifestFileName), LanguageManifest(init, content, "French", false));
                Check(!PackManifest.TryLoadManifest(localized, "French", out _, out _), "retired language sidecar is not discovered");
                Check(!PackManifest.TryLoadManifest(localized, "../ja", out _, out _), "language traversal refused");
                string jaManifest = Path.Combine(localized, "ja", PackManifest.ManifestFileName);
                File.WriteAllText(jaManifest, LanguageManifest(init, content, "English", false));
                Check(!PackManifest.TryLoadManifest(localized, "ja", out _, out _), "swapped language manifest refused");
                File.WriteAllText(jaManifest, LanguageManifest(init, content, "ja", false));
                File.WriteAllBytes(Path.Combine(localized, "ja", "Init.pack"), content);
                Check(!PackManifest.TryLoadManifest(localized, "ja", out _, out _), "mismatched language Init hash refused");
                File.WriteAllBytes(Path.Combine(localized, "ja", "Init.pack"), init);
                File.Delete(jaManifest);
                Check(!PackManifest.TryLoadManifest(localized, "ja", out _, out _), "missing language publication does not fall back");
                Check(PackManifest.TryLoadManifest(localized, out _), "other language remains usable");
            }
            finally { Directory.Delete(root, true); }
            Console.WriteLine("[PackPlatform] " + (checks - failures) + "/" + checks + " passed");
            return failures == 0;
        }
    }
}
