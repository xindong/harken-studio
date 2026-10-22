using System;
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
                string fixture = Path.Combine(AppContext.BaseDirectory, "NativePublications", "UnityFixture", "U4Original");
                Require(GameplayPublication.Read(fixture, null, out GameplayPublication publication, out _), "fixture");
                Assert(publication.TryFindAction("Play_U4_A", out GameplayAction action), "fixture A");
                Assert(publication.TryFindAction("Play_U4_B", out GameplayAction other), "fixture B");
                Directory.CreateDirectory(directory);
                byte[] initBytes = File.ReadAllBytes(Path.Combine(fixture, publication.InitPack.Path));
                Assert(publication.TryFindPack(action.OwnerPackStableId, out PackManifestEntry entry), "fixture pack");
                byte[] actionBytes = File.ReadAllBytes(Path.Combine(fixture, entry.Path));
                Assert(publication.TryFindPack(other.OwnerPackStableId, out PackManifestEntry otherEntry), "fixture B pack");
                byte[] otherBytes = File.ReadAllBytes(Path.Combine(fixture, otherEntry.Path));
                WriteInit(directory, initBytes);
                WriteAction(directory, action.Name, action.ActionStableId, actionBytes);
                WriteAction(directory, other.Name, other.ActionStableId, otherBytes);
                File.WriteAllText(Path.Combine(directory, "Unused_Broken.json"), "not json");
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                Require(PackBootstrap.StartDirectory(directory, "French", ref config, out session), "Init-only startup");
                Assert(session.ProjectId == publication.ProjectId && session.Publication == null &&
                    session.StartupPacks.Length == 0 && session.IsStartupComplete, "real identity without Publication");
                Assert(Actions.Resolve(action.Name, out _) != VnResult.VN_OK, "initially absent");
                Assert(session.PrepareActions(new[] { action.Name }, Guid.NewGuid().ToString("D"), out _) ==
                    VnResult.VN_GAMEPLAY_WRONG_PROJECT, "explicit project check");
                Assert(session.PrepareActions(new[] { action.Name, "Missing" }, session.ProjectId, out ActionPreparation refused) ==
                    VnResult.VN_CONTENT_NOT_LOADED, "batch refuses missing input");
                Assert(refused.IsReleased && Actions.Resolve(action.Name, out _) != VnResult.VN_OK, "atomic batch");
                string localized = Path.Combine(directory, "French");
                Directory.CreateDirectory(localized);
                File.WriteAllText(Path.Combine(localized, action.Name + ".json"), "{}");
                Assert(session.PrepareActions(new[] { action.Name }, session.ProjectId, out _) == VnResult.VN_BAD_PACK, "half pair refuses fallback");
                File.Delete(Path.Combine(localized, action.Name + ".json"));
                Require(session.PrepareActions(new[] { action.Name }, session.ProjectId, out ActionPreparation first), "General fallback");
                File.Delete(Path.Combine(directory, "Init.bytes"));
                File.Delete(Path.Combine(directory, "Init.json"));
                File.Delete(Path.Combine(directory, action.Name + ".bytes"));
                File.Delete(Path.Combine(directory, action.Name + ".json"));
                Require(session.PrepareActions(new[] { action.Name }, session.ProjectId, out ActionPreparation warm), "warm without source");
                Require(session.PrepareActions(new[] { other.Name }, session.ProjectId, out ActionPreparation second), "B without Init or A files");
                Require(session.AcquirePack(action.OwnerPackStableId, out Pack raw), "raw lease");
                Require(Actions.Resolve(action.Name, session.ProjectId, out ActionResolved beforeRename), "prepared resolve");
                Require(HarkenEngine.Update(), "update");
                Require(Actions.Post(in beforeRename, Emitter.None, 3, out ActionFiring firing), "prepared post");
                const string renamed = "Play_Renamed";
                WriteAction(directory, renamed, action.ActionStableId, actionBytes);
                Require(session.RefreshAction(renamed, session.ProjectId, out ActionPreparation refresh), "rename");
                Assert(Actions.Post(in beforeRename, Emitter.None, 4, out _) == VnResult.VN_STALE_HANDLE, "old token expires");
                Require(firing.Query(out _), "firing survives rename");
                WriteAction(directory, action.Name, action.ActionStableId, actionBytes);
                Assert(session.PrepareActions(new[] { action.Name }, session.ProjectId, out _) == VnResult.VN_GAMEPLAY_NAME_RETIRED, "old file cannot rename back");
                Require(first.Release(), "old scope release");
                Require(warm.Release(), "warm scope release");
                Require(session.ReleasePack(action.OwnerPackStableId, raw), "original raw handle release");
                Require(refresh.Release(), "renamed scope release");
                Assert(Actions.Resolve(renamed, out _) != VnResult.VN_OK, "last lease removes name");
                Assert(session.PrepareActions(new[] { action.Name }, session.ProjectId, out _) == VnResult.VN_GAMEPLAY_NAME_RETIRED, "unload preserves accepted name");
                const string renamedAgain = "Play_RenamedAgain";
                WriteAction(directory, renamedAgain, action.ActionStableId, actionBytes);
                Require(session.RefreshAction(renamedAgain, session.ProjectId, out ActionPreparation revived), "refresh unloaded owner");
                File.WriteAllText(Path.Combine(directory, ".harken-pack-publishing"), "in progress");
                Assert(session.RefreshAction(renamedAgain, session.ProjectId, out _) == VnResult.VN_PACK_STALE_REVISION, "marker refuses refresh");
                Require(Actions.Resolve(renamedAgain, out _), "failed refresh preserves name");
                File.Delete(Path.Combine(directory, ".harken-pack-publishing"));
                File.WriteAllText(Path.Combine(directory, renamedAgain + ".json"), "{\"name\":\"Wrong\",\"guid\":\"" + action.ActionStableId + "\"}");
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
                Require(session.PrepareActions(new[] { action.Name }, session.ProjectId, out ActionPreparation restarted), "new session has no invented history");
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
                Require(session.RefreshAction("Play_Head", session.ProjectId, out ActionPreparation refreshed), "General to localized");
                Assert(session.AcquirePack(before.Facts.packStableId, out _) == VnResult.VN_CAPABILITY_UNAVAILABLE,
                    "old physical ID no longer owns content");
                Require(head.Query(out _), "old firing retains its content");
                Require(session.ReleasePack(before.Facts.packStableId, raw), "old raw lease follows stable owner");
                Require(general.Release(), "old scope follows stable owner");
                Require(refreshed.Release(), "localized final release");
                Assert(Actions.Resolve("Play_Head", out _) != VnResult.VN_OK, "no orphaned variant");
                Require(session.Shutdown(), "closure shutdown");
                session = null;
            }
            finally
            {
                session?.Dispose();
                if (Directory.Exists(directory)) Directory.Delete(directory, true);
            }
        }

        private static void WriteInit(string directory, byte[] bytes)
        {
            File.WriteAllBytes(Path.Combine(directory, "Init.bytes"), bytes);
            File.WriteAllText(Path.Combine(directory, "Init.json"), "{\"variables\":[],\"waveforms\":false}", new UTF8Encoding(false));
        }
        private static void WriteAction(string directory, string name, VnObjectId id, byte[] bytes)
        {
            File.WriteAllBytes(Path.Combine(directory, name + ".bytes"), bytes);
            File.WriteAllText(Path.Combine(directory, name + ".json"), "{\"name\":\"" + name + "\",\"guid\":\"" + id + "\",\"waveforms\":42}", new UTF8Encoding(false));
        }
        private static void Require(VnResult result, string operation)
        { if (result != VnResult.VN_OK) throw new InvalidOperationException(operation + ": " + result); }
        private static void Assert(bool value, string operation)
        { if (!value) throw new InvalidOperationException(operation); }
    }
}
