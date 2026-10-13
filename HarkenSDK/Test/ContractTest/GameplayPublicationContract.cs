using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using Harken;

namespace Harken.Test
{
    internal static class GameplayPublicationContract
    {
        public static bool Run(string originalRoot, string renamedRoot, string languageRoot)
        {
            try
            {
                Require(originalRoot != null && renamedRoot != null && languageRoot != null,
                    "Gameplay gates require original, renamed, and language production roots.");
                GameplayPublication original = Read(originalRoot, null, out byte[] originalInit);
                GameplayPublication renamed = Read(renamedRoot, null, out byte[] renamedInit);
                Require(original.TryFindAction("Play_U4_A", out GameplayAction action), "Original Action is missing.");
                Require(renamed.TryFindAction("Play_U4_Renamed", out GameplayAction replacement) &&
                    !renamed.TryFindAction(action.Name, out _) && replacement.ActionStableId == action.ActionStableId &&
                    replacement.NameBindingRevision > action.NameBindingRevision &&
                    renamed.RetiredActions.Count == 0, "Rename must publish only the current exact name.");
                Expect(renamed.GetActionPreparationOrder(new[] { action.Name }, renamed.ProjectId, out ulong[] oldOrder),
                    VnResult.VN_UNKNOWN_ID, "The old exact name has no alias or retirement entry");
                Require(oldOrder.Length == 0, "An unknown name must not prepare the replacement Action.");
                CheckProjectionTampering(originalRoot, original, originalInit, false);
                CheckProjectionTampering(renamedRoot, renamed, renamedInit, false);
                JsonObject inventedRetirement = LoadJson(renamedRoot);
                inventedRetirement["gameplayCatalog"]["retired"].AsArray().Add(new JsonObject
                {
                    ["name"] = action.Name, ["lastActionStableId"] = action.ActionStableId.ToString(),
                    ["lastNameBindingRevision"] = action.NameBindingRevision.ToString(CultureInfo.InvariantCulture),
                    ["reason"] = "renamed"
                });
                Expect(Parse(inventedRetirement).ValidateInit(renamedInit, out _), VnResult.VN_PACK_STALE_REVISION,
                    "A JSON-only retirement cannot match the current-name Init catalog");
                Expect(original.ValidateInit(renamedInit, out _), VnResult.VN_PACK_HASH_MISMATCH, "Wrong Init bytes");
                JsonObject rebound = LoadJson(originalRoot);
                foreach (JsonNode pack in rebound["packs"].AsArray())
                    if (pack["packStableId"].GetValue<string>() == original.InitPackStableId.ToString(CultureInfo.InvariantCulture))
                        pack["contentHash"] = renamed.InitPack.ContentHash.ToString(CultureInfo.InvariantCulture);
                Expect(Parse(rebound).ValidateInit(renamedInit, out _), VnResult.VN_PACK_STALE_REVISION,
                    "Rebinding a file hash cannot adopt a different catalog");
                JsonObject legacy = LoadJson(originalRoot);
                legacy.Remove("gameplayCatalog");
                Expect(GameplayPublication.Parse(legacy.ToJsonString(), null, null, out _, out _),
                    VnResult.VN_CAPABILITY_UNAVAILABLE, "Legacy publication discovery");

                GameplayPublication english = Read(languageRoot, "English", out _);
                GameplayPublication japanese = Read(languageRoot, "ja", out _);
                Require(english.Language == "English" && japanese.Language == "ja" && english.Digest != japanese.Digest,
                    "Explicit languages must adopt their own paired publications.");
                string inventory = File.ReadAllText(Path.Combine(languageRoot, PackManifest.ManifestFileName));
                string selected = File.ReadAllText(Path.Combine(languageRoot, "English", PackManifest.ManifestFileName));
                ulong exactRevision = JsonNode.Parse(selected)["gameplayCatalog"]["initContentRevision"].GetValue<ulong>();
                Require(exactRevision > 9007199254740992UL && english.InitContentRevision == exactRevision,
                    "The production Init revision must survive integer parsing above 2^53.");
                Expect(GameplayPublication.Parse(inventory, selected, "ja", out _, out _), VnResult.VN_BAD_PACK,
                    "Wrong selected-language JSON");
                Expect(GameplayPublication.Parse(inventory, null, "English", out _, out _), VnResult.VN_BAD_PACK,
                    "Missing selected-language JSON");
                Expect(GameplayPublication.Read(languageRoot, "retired-language", out _, out _), VnResult.VN_BAD_PACK,
                    "Unknown language has no fallback");
                Console.WriteLine("[Gameplay] paired projections, all row fields, rename, language and lossless revisions PASS.");

                CheckProviderInspection(original, originalInit);
                CheckBytesPreparation(originalRoot, original, originalInit);
            Console.WriteLine("[Gameplay] provider inspection, retained partial startup, explicit retry and engine ownership PASS.");
                return true;
            }
            catch (Exception exception)
            {
                Console.WriteLine("[Gameplay] FAIL: " + exception);
                return false;
            }
        }

        private static void CheckProjectionTampering(string root, GameplayPublication publication,
            byte[] initBytes, bool retired)
        {
            var replacements = retired
                ? new Dictionary<string, string>
                {
                    ["name"] = "Tampered_Retired", ["lastActionStableId"] = "01234567-89ab-cdef-8123-456789abcdef",
                    ["lastNameBindingRevision"] = "999", ["reason"] = "deleted"
                }
                : new Dictionary<string, string>
                {
                    ["name"] = "Tampered_Active", ["actionStableId"] = "01234567-89ab-cdef-8123-456789abcdef",
                    ["nameBindingRevision"] = "999", ["callableFingerprint"] = "0",
                    ["ownerPackStableId"] = publication.Actions[1].OwnerPackStableId.ToString(CultureInfo.InvariantCulture),
                    ["providerClosureFingerprint"] = "0"
                };
            foreach (var change in replacements)
            {
                JsonObject json = LoadJson(root);
                json["gameplayCatalog"][retired ? "retired" : "actions"][0][change.Key] = change.Value;
                GameplayPublication tampered = Parse(json);
                Require(tampered.Digest == publication.Digest, "The test must retain the original digest.");
                Expect(tampered.ValidateInit(initBytes, out _), VnResult.VN_PACK_STALE_REVISION,
                    "Tampered " + (retired ? "retired " : "active ") + change.Key);
            }
            JsonObject wrongProject = LoadJson(root);
            wrongProject["gameplayCatalog"]["projectId"] = "01234567-89ab-cdef-8123-456789abcdef";
            Expect(Parse(wrongProject).ValidateInit(initBytes, out _), VnResult.VN_GAMEPLAY_WRONG_PROJECT, "Wrong project");
            JsonObject wrongRevision = LoadJson(root);
            wrongRevision["gameplayCatalog"]["initContentRevision"] = publication.InitContentRevision + 1;
            Expect(Parse(wrongRevision).ValidateInit(initBytes, out _), VnResult.VN_PACK_STALE_REVISION, "Wrong revision");
            byte[] corrupt = (byte[])initBytes.Clone();
            corrupt[corrupt.Length - 1] ^= 1;
            Expect(publication.ValidateInit(corrupt, out _), VnResult.VN_PACK_HASH_MISMATCH, "Corrupt Init");
        }

        private static void CheckProviderInspection(GameplayPublication publication, byte[] initBytes)
        {
            Expect(PackByteProvider.InspectInitPack(publication.InitPackStableId, ProvideBytes,
                initBytes, out InitPackInspection inspection), VnResult.VN_OK, "Provider Init inspection");
            Require(inspection.PackStableId == publication.InitPackStableId &&
                PackByteProvider.LastProvideCalls == 1 && PackByteProvider.LastReleaseCalls == 1 &&
                PackByteProvider.OutstandingPinCount == 0, "Inspection must release the exact provided bytes.");
            Expect(PackByteProvider.InspectInitPack(publication.InitPackStableId, ThrowProvider, null, out _),
                VnResult.VN_PACK_PROVIDER_FAILED, "Throwing inspection provider");
            Require(PackByteProvider.OutstandingPinCount == 0 && PackByteProvider.LastProvideLeftSpanZero,
                "Provider failure must leave no span or pin.");
            Require(PackByteProvider.InspectInitPack(publication.InitPackStableId, ProvideBytes,
                new byte[] { 1, 2, 3 }, out _) != VnResult.VN_OK && PackByteProvider.OutstandingPinCount == 0 &&
                PackByteProvider.LastReleaseCalls == 1, "Rejected Init bytes must still release their pin.");
        }

        private static void CheckBytesPreparation(string root, GameplayPublication publication, byte[] initBytes)
        {
            Require(publication.TryFindAction("Play_U4_A", out GameplayAction a) &&
                publication.TryFindAction("Play_U4_B", out _), "The fixture must contain independent A and B Actions.");
            publication.TryFindAction("Play_U4_B", out GameplayAction b);
            var provider = new ContentProvider { Publication = publication };
            foreach (PackManifestEntry pack in publication.Packs)
                if (pack.PackStableId != publication.InitPackStableId)
                    provider.Bytes.Add(pack.PackStableId, File.ReadAllBytes(Path.Combine(root, pack.Path)));
            byte[] missing = provider.Bytes[b.OwnerPackStableId];
            provider.Bytes.Remove(b.OwnerPackStableId);
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            PackBootstrapSession session = null;
            try
            {
                Expect(PackBootstrap.Start(publication, initBytes, new[] { a.OwnerPackStableId, b.OwnerPackStableId },
                    ProvideContent, provider, ref config, out session), VnResult.VN_CONTENT_NOT_LOADED, "Partial bytes startup");
                int calls = 3;
                Require(session != null && session.IsActive && !session.IsStartupComplete &&
                    session.StartupResult == VnResult.VN_CONTENT_NOT_LOADED &&
                    session.FailedStartupPackStableId == b.OwnerPackStableId &&
                    session.StartupPacks.Length == 2 && session.StartupPacks[0].IsValid &&
                    !session.StartupPacks[1].IsValid && provider.Calls == calls &&
                    PackByteProvider.OutstandingPinCount == 0,
                    "Missing content must retain an incomplete session with only the accepted A Pack and no pins.");
                provider.Bytes[b.OwnerPackStableId] = missing;
                Expect(session.PrepareStartupPacks(), VnResult.VN_OK, "Explicit missing-content startup retry");
                ++calls;
                Require(session.IsStartupComplete && session.FailedStartupPackStableId == 0 &&
                    session.StartupPacks[0].IsValid && session.StartupPacks[1].IsValid &&
                    provider.Calls == calls && PackByteProvider.OutstandingPinCount == 0,
                    "Explicit preparation must complete the retained session without implicit provider calls.");
                Expect(session.Shutdown(), VnResult.VN_OK, "Retained startup session shutdown");
                session = null;
                provider.Bytes.Remove(b.OwnerPackStableId);
                Expect(PackBootstrap.Start(publication, initBytes, new[] { a.OwnerPackStableId },
                    ProvideContent, provider, ref config, out session), VnResult.VN_OK, "A-only startup");
                ++calls;
                Require(session != null && session.IsActive && session.IsStartupComplete &&
                    session.StartupPacks.Length == 1 && session.StartupPacks[0].IsValid && provider.Calls == calls,
                    "A-only startup must publish exactly its requested content.");
                ulong acceptedHandle = session.StartupPacks[0].Handle;
                ulong acceptedRevision = session.SetRevision;
                Expect(session.AcquirePack(b.OwnerPackStableId, out _), VnResult.VN_CONTENT_NOT_LOADED,
                    "Missing incremental content");
                ++calls;
                Require(session.IsActive && session.SetRevision == acceptedRevision &&
                    session.StartupPacks[0].Handle == acceptedHandle && provider.Calls == calls &&
                    PackByteProvider.OutstandingPinCount == 0,
                    "Missing incremental content must preserve the accepted session.");
                Expect(Actions.Post(a.Name, Emitter.None, out ActionFiring firing), VnResult.VN_OK,
                    "Accepted Pack remains callable after partial startup");
                Require(firing.IsValid, "Accepted firing must be retained.");
                Expect(Actions.Post(b.Name, Emitter.None, out _), VnResult.VN_CONTENT_NOT_LOADED, "Missing Pack trigger");
                Require(provider.Calls == calls, "Triggers must never call the preparation provider.");
                Expect(session.AcquirePack(ulong.MaxValue, out _), VnResult.VN_UNKNOWN_ID, "Unknown selected Pack");
                Require(provider.Calls == calls, "Unknown IDs must not call the provider.");

                byte[] corrupt = (byte[])missing.Clone();
                corrupt[corrupt.Length - 1] ^= 1;
                provider.Bytes[b.OwnerPackStableId] = corrupt;
                Expect(session.AcquirePack(b.OwnerPackStableId, out _), VnResult.VN_PACK_HASH_MISMATCH, "Corrupt explicit retry");
                ++calls;
                Require(session.IsActive && session.SetRevision == acceptedRevision &&
                    session.StartupPacks[0].Handle == acceptedHandle && provider.Calls == calls &&
                    PackByteProvider.OutstandingPinCount == 0,
                    "A failed retry must preserve accepted requests and revision.");
                Expect(firing.Query(out _), VnResult.VN_OK, "Accepted firing survives corrupt retry");
                provider.Bytes[b.OwnerPackStableId] = missing;
                Expect(session.AcquirePack(b.OwnerPackStableId, out Pack acquired), VnResult.VN_OK, "Corrected explicit retry");
                ++calls;
                Require(acquired.IsValid && session.SetRevision == acceptedRevision + 1 &&
                    session.StartupPacks[0].Handle == acceptedHandle && provider.Calls == calls,
                    "Corrected incremental content must publish without replacing accepted handles.");
                Expect(Actions.Post(b.Name, Emitter.None, out _), VnResult.VN_OK, "Prepared second Pack");
                Expect(PackBootstrap.Start(publication, initBytes, Array.Empty<ulong>(), ProvideContent, provider,
                    ref config, out PackBootstrapSession second), VnResult.VN_ALREADY_INITIALIZED, "Second engine owner");
                Require(second == null && session.IsActive, "Rejected startup must leave the first owner active.");
                Expect(firing.Query(out _), VnResult.VN_OK, "First owner firing survives rejected startup");
                Require(provider.Calls == calls && PackByteProvider.OutstandingPinCount == 0,
                    "Completed preparation must leave no pins or implicit provider requests.");
            }
            finally { session?.Dispose(); }
        }

        private sealed class ContentProvider
        {
            public GameplayPublication Publication;
            public readonly Dictionary<ulong, byte[]> Bytes = new Dictionary<ulong, byte[]>();
            public int Calls;
        }

        private static VnResult ProvideContent(ulong id, string path, uint variant, ulong hash, out byte[] bytes, object state)
        {
            var source = (ContentProvider)state;
            ++source.Calls;
            if (!source.Publication.TryFindPack(id, out PackManifestEntry entry) || entry.Path != path ||
                entry.VariantKey != variant || entry.ContentHash != hash)
            {
                bytes = null;
                return VnResult.VN_PACK_PROVIDER_FAILED;
            }
            return source.Bytes.TryGetValue(id, out bytes) ? VnResult.VN_OK : VnResult.VN_CONTENT_NOT_LOADED;
        }

        private static VnResult ProvideBytes(ulong id, string path, uint variant, ulong hash, out byte[] bytes, object state)
        {
            bytes = (byte[])state;
            return VnResult.VN_OK;
        }

        private static VnResult ThrowProvider(ulong id, string path, uint variant, ulong hash, out byte[] bytes, object state)
            => throw new InvalidOperationException("Deliberate provider exception.");

        private static GameplayPublication Read(string root, string language, out byte[] initBytes)
        {
            Expect(GameplayPublication.Read(root, language, out GameplayPublication publication, out string error),
                VnResult.VN_OK, "Read publication: " + error);
            initBytes = File.ReadAllBytes(Path.Combine(root, publication.InitPack.Path));
            Expect(publication.ValidateInit(initBytes, out error), VnResult.VN_OK, "Validate paired Init: " + error);
            return publication;
        }

        private static JsonObject LoadJson(string root)
            => JsonNode.Parse(File.ReadAllText(Path.Combine(root, PackManifest.ManifestFileName))).AsObject();

        private static GameplayPublication Parse(JsonObject json)
        {
            Expect(GameplayPublication.Parse(json.ToJsonString(), null, null, out GameplayPublication publication, out string error),
                VnResult.VN_OK, "Parse changed metadata: " + error);
            return publication;
        }

        private static void Expect(VnResult actual, VnResult expected, string operation)
            => Require(actual == expected, operation + ": expected " + expected + ", got " + actual);

        private static void Require(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException(message);
        }
    }
}
