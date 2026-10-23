using System;
using System.Globalization;
using System.IO;
using System.Text;
using Harken;

namespace Harken.Test
{
    internal static class CompanionDirectoryContract
    {
        internal static bool Run()
        {
            string directory = Path.Combine(Path.GetTempPath(), "harken-companion-" + Guid.NewGuid().ToString("N"));
            PackBootstrapSession session = null;
            try
            {
                VersionContract();
                string fixture = Path.Combine(AppContext.BaseDirectory, "NativePublications", "UnityFixture", "U4Original");
                var source = new PackDirectorySource(fixture, string.Empty);
                Require(source.Capture(null, default, null, out CompanionInput init), "fixture Init");
                Require(source.Capture("Play_U4_A", init.Facts.projectId, init.Bytes, out CompanionInput action), "fixture A");
                Require(source.Capture("Play_U4_B", init.Facts.projectId, init.Bytes, out CompanionInput other), "fixture B");
                Directory.CreateDirectory(directory);
                byte[] initBytes = init.Bytes;
                byte[] actionBytes = action.Bytes;
                byte[] otherBytes = other.Bytes;
                WriteInit(directory, initBytes);
                WriteAction(directory, action.Names.Name, action.Names.RootId, actionBytes);
                WriteAction(directory, other.Names.Name, other.Names.RootId, otherBytes);
                File.WriteAllText(Path.Combine(directory, "Unused_Broken.json"), "not json");
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                WriteInit(directory, initBytes, null);
                Assert(PackBootstrap.StartDirectory(directory, "French", ref config, out session) == VnResult.VN_BAD_PACK &&
                    session == null, "missing Init version leaves no session");
                byte[] unsupportedInit = (byte[])initBytes.Clone();
                for (int i = 4; i < 8; ++i) unsupportedInit[i] = 0xff;
                WriteInit(directory, unsupportedInit, uint.MaxValue.ToString(CultureInfo.InvariantCulture));
                Assert(PackBootstrap.StartDirectory(directory, "French", ref config, out session) == VnResult.VN_BAD_PACK &&
                    session == null, "matching unsupported pair leaves no session");
                WriteInit(directory, initBytes);
                Require(PackBootstrap.StartDirectory(directory, "French", ref config, out session), "Init-only startup");
                Assert(session.ProjectId == init.Facts.projectId.ToString() && session.Publication == null &&
                    session.StartupPacks.Length == 0 && session.IsStartupComplete, "real identity without Publication");
                Assert(Actions.Resolve(action.Names.Name, out _) != VnResult.VN_OK, "initially absent");
                Assert(session.PrepareActions(new[] { action.Names.Name }, Guid.NewGuid().ToString("D"), out _) ==
                    VnResult.VN_GAMEPLAY_WRONG_PROJECT, "explicit project check");
                Assert(session.PrepareActions(new[] { action.Names.Name, "Missing" }, session.ProjectId, out ActionPreparation refused) ==
                    VnResult.VN_CONTENT_NOT_LOADED, "batch refuses missing input");
                Assert(refused.IsReleased && Actions.Resolve(action.Names.Name, out _) != VnResult.VN_OK, "atomic batch");
                string localized = Path.Combine(directory, "French");
                Directory.CreateDirectory(localized);
                File.WriteAllText(Path.Combine(localized, action.Names.Name + ".json"), "{}");
                Assert(session.PrepareActions(new[] { action.Names.Name }, session.ProjectId, out _) == VnResult.VN_BAD_PACK, "half pair refuses fallback");
                WriteAction(localized, action.Names.Name, action.Names.RootId, actionBytes, "999");
                Assert(session.PrepareActions(new[] { action.Names.Name }, session.ProjectId, out _) == VnResult.VN_BAD_PACK,
                    "complete language pair with bad version cannot fall back");
                File.Delete(Path.Combine(localized, action.Names.Name + ".bytes"));
                File.Delete(Path.Combine(localized, action.Names.Name + ".json"));
                Require(session.PrepareActions(new[] { action.Names.Name }, session.ProjectId, out ActionPreparation first), "General fallback");
                File.Delete(Path.Combine(directory, "Init.bytes"));
                File.Delete(Path.Combine(directory, "Init.json"));
                File.Delete(Path.Combine(directory, action.Names.Name + ".bytes"));
                File.Delete(Path.Combine(directory, action.Names.Name + ".json"));
                Require(session.PrepareActions(new[] { action.Names.Name }, session.ProjectId, out ActionPreparation warm), "warm without source");
                Require(session.PrepareActions(new[] { other.Names.Name }, session.ProjectId, out ActionPreparation second), "B without Init or A files");
                Require(session.AcquirePack(action.Facts.packStableId, out Pack raw), "raw lease");
                Require(Actions.Resolve(action.Names.Name, session.ProjectId, out ActionResolved beforeRename), "prepared resolve");
                Require(HarkenEngine.Update(), "update");
                Require(Actions.Post(in beforeRename, Emitter.None, 3, out ActionFiring firing), "prepared post");
                WriteAction(directory, action.Names.Name, action.Names.RootId, actionBytes, BinaryVersion(actionBytes) + ".0000000000000001");
                Assert(session.RefreshAction(action.Names.Name, session.ProjectId, out _) == VnResult.VN_BAD_PACK,
                    "refresh rejects rounded version");
                Require(Actions.Resolve(action.Names.Name, out _), "version failure preserves name");
                Require(Actions.Post(in beforeRename, Emitter.None, 4, out _), "version failure preserves resolved token");
                Require(firing.Query(out _), "version failure preserves firing");
                WriteAction(directory, action.Names.Name, action.Names.RootId, actionBytes, BinaryVersion(actionBytes) + "e0");
                Require(session.RefreshAction(action.Names.Name, session.ProjectId, out ActionPreparation corrected), "corrected version retries");
                Require(corrected.Release(), "corrected scope release leaves original leases");
                const string renamed = "Play_Renamed";
                WriteAction(directory, renamed, action.Names.RootId, actionBytes);
                Require(session.RefreshAction(renamed, session.ProjectId, out ActionPreparation refresh), "rename");
                Assert(Actions.Post(in beforeRename, Emitter.None, 4, out _) == VnResult.VN_STALE_HANDLE, "old token expires");
                Require(firing.Query(out _), "firing survives rename");
                WriteAction(directory, action.Names.Name, action.Names.RootId, actionBytes);
                Assert(session.PrepareActions(new[] { action.Names.Name }, session.ProjectId, out _) == VnResult.VN_GAMEPLAY_NAME_RETIRED, "old file cannot rename back");
                Require(first.Release(), "old scope release");
                Require(warm.Release(), "warm scope release");
                Require(session.ReleasePack(action.Facts.packStableId, raw), "original raw handle release");
                Require(refresh.Release(), "renamed scope release");
                Assert(Actions.Resolve(renamed, out _) != VnResult.VN_OK, "last lease removes name");
                Require(session.AcquirePack(action.Facts.packStableId, out Pack reloaded), "known unloaded owner reacquires by accepted name");
                Require(Actions.Resolve(renamed, out _), "raw reacquire restores accepted name");
                Require(session.ReleasePack(action.Facts.packStableId, reloaded), "reloaded raw release");
                Assert(session.PrepareActions(new[] { action.Names.Name }, session.ProjectId, out _) == VnResult.VN_GAMEPLAY_NAME_RETIRED, "unload preserves accepted name");
                const string renamedAgain = "Play_RenamedAgain";
                WriteAction(directory, renamedAgain, action.Names.RootId, actionBytes);
                Require(session.RefreshAction(renamedAgain, session.ProjectId, out ActionPreparation revived), "refresh unloaded owner");
                File.WriteAllText(Path.Combine(directory, ".harken-pack-publishing"), "in progress");
                Assert(session.RefreshAction(renamedAgain, session.ProjectId, out _) == VnResult.VN_PACK_STALE_REVISION, "marker refuses refresh");
                Require(Actions.Resolve(renamedAgain, out _), "failed refresh preserves name");
                File.Delete(Path.Combine(directory, ".harken-pack-publishing"));
                File.WriteAllText(Path.Combine(directory, renamedAgain + ".json"), "{\"schemaVersion\":" + BinaryVersion(actionBytes) + ",\"name\":\"Wrong\",\"guid\":\"" + action.Names.RootId + "\"}");
                Assert(session.RefreshAction(renamedAgain, session.ProjectId, out _) == VnResult.VN_BAD_PACK, "JSON name must match basename");
                Require(Actions.Resolve(renamedAgain, out _), "bad JSON preserves owner");
                Require(revived.Release(), "revived scope release");
                Require(second.Release(), "B release");
                Require(session.Shutdown(), "shutdown");
                Require(first.Release(), "terminated scope release");
                Assert(first.Prepare() == VnResult.VN_NOT_INITIALIZED, "old scope cannot restart");
                session = null;
                WriteInit(directory, initBytes);
                Require(PackBootstrap.StartDirectory(directory, null, ref config, out session), "restart");
                Require(session.PrepareActions(new[] { action.Names.Name }, session.ProjectId, out ActionPreparation restarted), "new session has no invented history");
                Require(restarted.Release(), "restarted release");
                Require(session.Shutdown(), "restarted shutdown");
                session = null;
                RunProducedClosureAndVariant();
                Console.WriteLine("[Companion directory] Init-only, atomic preparation, warm reuse, language refusal, rename and lease lifecycle PASS.");
                return true;
            }
            catch (Exception error) { Console.WriteLine("[Companion directory] FAIL: " + error); return false; }
            finally
            {
                session?.Dispose();
                if (Directory.Exists(directory)) Directory.Delete(directory, true);
            }
        }

        private static void RunProducedClosureAndVariant()
        {
            string fixture = Path.Combine(AppContext.BaseDirectory, "NativePublications", "UnityFixture", "CompanionPacks");
            string directory = Path.Combine(Path.GetTempPath(), "harken-closure-" + Guid.NewGuid().ToString("N"));
            PackBootstrapSession session = null;
            try
            {
                Directory.CreateDirectory(directory);
                foreach (string file in Directory.GetFiles(fixture))
                    File.Copy(file, Path.Combine(directory, Path.GetFileName(file)));
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                Require(PackBootstrap.StartDirectory(directory, "English", ref config, out session), "produced Init");
                Require(session.PrepareActions(new[] { "Play_Head" }, session.ProjectId, out ActionPreparation general), "embedded closure");
                Assert(!File.Exists(Path.Combine(directory, "Play_Tail.bytes")) &&
                    !File.Exists(Path.Combine(directory, "Play_Nested.json")), "only requested root was supplied");
                Require(Actions.Resolve("Play_Head", out ActionResolved resolved), "root resolve");
                Assert(Actions.Resolve("Play_Tail", out _) != VnResult.VN_OK, "embedded definitions do not invent public names");
                Require(HarkenEngine.Update(), "closure update");
                Require(Actions.Post(in resolved, Emitter.None, 12, out ActionFiring head), "closure post");
                Require(head.QueryStep(0, out VnActionStepResult headStep), "head step");
                Assert(headStep.childFiring != 0, "head invoked tail");
                Require(new ActionFiring(headStep.childFiring).QueryStep(0, out VnActionStepResult tailStep), "tail step");
                Assert(tailStep.childFiring != 0, "tail invoked nested");
                Require(new ActionFiring(tailStep.childFiring).QueryStep(0, out VnActionStepResult nestedStep), "nested step");
                Assert(nestedStep.producedPlayableInstance != 0, "nested produced sound");

                var source = new PackDirectorySource(directory, string.Empty);
                Require(source.Capture("Play_Head", VnObjectId.Parse(session.ProjectId),
                    File.ReadAllBytes(Path.Combine(directory, "Init.bytes")), out CompanionInput before), "inspect General identity");
                Require(session.AcquirePack(before.Facts.packStableId, out Pack raw), "General raw lease");
                string localized = Path.Combine(directory, "English");
                Directory.CreateDirectory(localized);
                foreach (string file in Directory.GetFiles(Path.Combine(fixture, "English")))
                    File.Copy(file, Path.Combine(localized, Path.GetFileName(file)));
                Require(new PackDirectorySource(directory, "English").Capture("Play_Head", VnObjectId.Parse(session.ProjectId),
                    File.ReadAllBytes(Path.Combine(directory, "Init.bytes")), out CompanionInput english), "inspect localized identity");
                Require(session.RefreshAction("Play_Head", session.ProjectId, out ActionPreparation refreshed), "General to localized");
                Assert(session.AcquirePack(before.Facts.packStableId, out _) == VnResult.VN_CAPABILITY_UNAVAILABLE,
                    "old physical ID no longer owns content");
                Require(head.Query(out _), "old firing retains its content");
                Require(session.ReleasePack(before.Facts.packStableId, raw), "old raw lease follows stable owner");
                Require(general.Release(), "old scope follows stable owner");
                Require(refreshed.Release(), "localized final release");
                Assert(Actions.Resolve("Play_Head", out _) != VnResult.VN_OK, "no orphaned variant");
                foreach (string file in Directory.GetFiles(localized)) File.Delete(file);
                Assert(session.AcquirePack(english.Facts.packStableId, out _) == VnResult.VN_PACK_STALE_REVISION,
                    "raw reload cannot substitute a different physical ID");
                Assert(Actions.Resolve("Play_Head", out _) != VnResult.VN_OK, "refused raw reload publishes nothing");
                Require(session.Shutdown(), "closure shutdown");
                session = null;
            }
            finally
            {
                session?.Dispose();
                if (Directory.Exists(directory)) Directory.Delete(directory, true);
            }
        }

        private static void VersionContract()
        {
            const string action = "\"name\":\"Play_Head\",\"guid\":\"11111111-2222-3333-4444-555555555555\"";
            byte[] pack = { (byte)'V', (byte)'S', (byte)'P', (byte)'K', 3, 0, 0, 0 };
            foreach (bool init in new[] { true, false })
            {
                string fields = init ? "\"variables\":[]" : action;
                foreach (string version in new[] { "3", "3.0", "3e0", "30e-1", "0.03E+2", "300e-2", "3e-0000" })
                    Require(PackCompanion.Parse(Encoding.UTF8.GetBytes("{\"schemaVersion\":" + version + "," + fields +
                        ",\"metadata\":false,\"waveforms\":42}"), pack, init ? null : "Play_Head", "", default, out _),
                        "exact companion version " + version);
                foreach (string version in new[] { null, "999", "\"3\"", "true", "null", "[]", "{}", "3.5", "-3",
                    "0", "3.0000000000000001", "2.9999999999999999", "3e999999999999999999999", "3e-999999999999999999999" })
                {
                    string field = version == null ? "" : "\"schemaVersion\":" + version + ",";
                    Assert(PackCompanion.Parse(Encoding.UTF8.GetBytes("{" + field + fields +
                        ",\"schema_version\":3,\"metadata\":{\"schemaVersion\":3}}"), pack, init ? null : "Play_Head", "", default, out _) ==
                        VnResult.VN_BAD_PACK, "reject companion version " + (version ?? "missing"));
                }
            }
            Require(PackCompanion.Parse(Encoding.UTF8.GetBytes("{\"schemaVersion\":3,\"variables\":[{\"name\":\"Choice\"," +
                "\"guid\":\"11111111-2222-3333-4444-555555555555\",\"labels\":[]}]}"), pack, null, "", default, out _), "empty labels");
            byte[] json = Encoding.UTF8.GetBytes("{\"schemaVersion\":3,\"variables\":[]}");
            foreach (byte[] invalid in new[] { null, Array.Empty<byte>(), new byte[7], new byte[8] })
                Assert(PackCompanion.Parse(json, invalid, null, "", default, out _) == VnResult.VN_BAD_PACK,
                    "missing or invalid Pack version prefix");
            foreach (uint version in new[] { 10u, 0x01020304u, uint.MaxValue })
            {
                for (int i = 0; i < 4; ++i) pack[4 + i] = (byte)(version >> (i * 8));
                json = Encoding.UTF8.GetBytes("{\"schemaVersion\":" + version.ToString(CultureInfo.InvariantCulture) +
                    ",\"variables\":[]}");
                Require(PackCompanion.Parse(json, pack, null, "", default, out _),
                    "pair comparison is independent of the supported schema");
            }
        }

        private static void WriteInit(string directory, byte[] bytes, string version = "")
        {
            if (version == "") version = BinaryVersion(bytes);
            File.WriteAllBytes(Path.Combine(directory, "Init.bytes"), bytes);
            File.WriteAllText(Path.Combine(directory, "Init.json"), "{" +
                (version == null ? "" : "\"schemaVersion\":" + version + ",") + "\"variables\":[],\"waveforms\":false}", new UTF8Encoding(false));
        }
        private static void WriteAction(string directory, string name, VnObjectId id, byte[] bytes, string version = "")
        {
            if (version == "") version = BinaryVersion(bytes);
            File.WriteAllBytes(Path.Combine(directory, name + ".bytes"), bytes);
            File.WriteAllText(Path.Combine(directory, name + ".json"), "{\"schemaVersion\":" + version +
                ",\"name\":\"" + name + "\",\"guid\":\"" + id + "\",\"waveforms\":42}", new UTF8Encoding(false));
        }
        private static string BinaryVersion(byte[] bytes)
            => ((uint)bytes[4] | (uint)bytes[5] << 8 | (uint)bytes[6] << 16 | (uint)bytes[7] << 24).ToString(CultureInfo.InvariantCulture);

        private static void Require(VnResult result, string operation)
        { if (result != VnResult.VN_OK) throw new InvalidOperationException(operation + ": " + result); }
        private static void Assert(bool value, string operation)
        { if (!value) throw new InvalidOperationException(operation); }
    }
}
