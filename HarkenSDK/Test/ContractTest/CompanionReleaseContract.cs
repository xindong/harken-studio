using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using Harken;

namespace Harken.Test
{
    internal static class CompanionReleaseContract
    {
        internal static bool Run(string root)
        {
            try
            {
                root = Path.GetFullPath(root);
                string[] packs = Directory.GetFiles(root, "*.bytes", SearchOption.AllDirectories);
                Require(packs.Any(path => Path.GetFileName(path) != "Init.bytes") &&
                    File.Exists(Path.Combine(root, "Init.bytes")), "Init and Action pairs are required");
                byte[] initBytes = File.ReadAllBytes(Path.Combine(root, "Init.bytes"));
                foreach (string pack in packs)
                {
                    byte[] bytes = File.ReadAllBytes(pack);
                    Require(bytes.Length >= 36 && BitConverter.ToUInt32(bytes, 4) == HarkenPackSchema.Version,
                        "binary generation: " + pack);
                    byte[] metadata = File.ReadAllBytes(Path.ChangeExtension(pack, ".json"));
                    using JsonDocument json = JsonDocument.Parse(metadata);
                    Require(json.RootElement.GetProperty("schemaVersion").GetInt32() == HarkenPackSchema.Version,
                        "companion generation: " + pack);
                    string name = Path.GetFileName(pack) == "Init.bytes" ? null : Path.GetFileNameWithoutExtension(pack);
                    Require(PackCompanion.Parse(metadata, name, null, default, out PackCompanion names), "companion parse");
                    foreach (int unsupported in new[] { HarkenPackSchema.Version - 1, HarkenPackSchema.Version + 1 })
                    {
                        JsonObject invalid = JsonNode.Parse(metadata).AsObject();
                        invalid["schemaVersion"] = unsupported;
                        Require(PackCompanion.Parse(JsonSerializer.SerializeToUtf8Bytes(invalid), name, null, default, out _) ==
                            VnResult.VN_BAD_PACK, "unsupported companion generation");
                        byte[] invalidBytes = (byte[])bytes.Clone();
                        Buffer.BlockCopy(BitConverter.GetBytes(unsupported), 0, invalidBytes, 4, 4);
                        Require(CompanionNative.Inspect(invalidBytes, name == null ? null : initBytes, names, out _) !=
                            VnResult.VN_OK, "unsupported binary generation");
                    }
                }
                foreach (string directory in packs.Where(path => Path.GetFileName(path) != "Init.bytes")
                             .Select(Path.GetDirectoryName).Distinct())
                {
                    string language = directory == root ? null : Path.GetRelativePath(root, directory);
                    string[] names = Directory.GetFiles(directory, "*.bytes")
                        .Where(path => Path.GetFileName(path) != "Init.bytes").Select(Path.GetFileNameWithoutExtension).ToArray();
                    VnConfig config = HarkenEngine.DefaultConfig();
                    config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                    Require(PackBootstrap.StartDirectory(root, language, ref config, out PackBootstrapSession session), "Init activation");
                    using (session)
                    {
                        Require(session.PrepareActions(names, session.ProjectId, out ActionPreparation preparation), "Action preparation");
                        foreach (string name in names)
                        {
                            Require(Actions.Resolve(name, session.ProjectId, out ActionResolved resolved), "name resolution");
                            Require(HarkenEngine.Update(), "update");
                            Require(Actions.Post(in resolved, Emitter.None, 0, out ActionFiring firing), "Action post");
                            Require(firing.Query(out _), "firing query");
                        }
                        Require(preparation.Release(), "release");
                        Require(session.Shutdown(), "shutdown");
                    }
                }
                Console.WriteLine("[PackCompanion] schemaVersion " + HarkenPackSchema.Version);
                Console.WriteLine("[PackCompanion] native publication PASS.");
                return true;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("[PackCompanion] FAIL: " + error);
                return false;
            }
        }

        private static void Require(bool condition, string operation)
        {
            if (!condition) throw new InvalidOperationException(operation);
        }

        private static void Require(VnResult result, string operation)
        {
            Require(result == VnResult.VN_OK, operation + ": " + result);
        }
    }
}
