using System;
using System.IO;
using System.Linq;
using Harken;

namespace Harken.Test
{
    internal static class ExportedPackContract
    {
        public static bool Run(string root)
        {
            try
            {
                string[] directories = new[] { root }.Concat(Directory.GetDirectories(root)
                    .OrderBy(path => path, StringComparer.Ordinal)).ToArray();
                int count = 0;
                foreach (string directory in directories)
                {
                    string[] names = Directory.GetFiles(directory)
                        .Where(path => Path.GetExtension(path) == ".json" || Path.GetExtension(path) == ".bytes")
                        .Select(Path.GetFileNameWithoutExtension).Where(name => name != "Init")
                        .Distinct(StringComparer.Ordinal).OrderBy(name => name, StringComparer.Ordinal).ToArray();
                    if (directory != root && (File.Exists(Path.Combine(directory, "Init.bytes")) ||
                        File.Exists(Path.Combine(directory, "Init.json"))))
                        throw new InvalidOperationException("Init must be in the selected platform directory.");
                    if (names.Length == 0 && directory != root) continue;
                    string language = directory == root ? null : Path.GetFileName(directory);
                    VnConfig config = HarkenEngine.DefaultConfig();
                    config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                    PackBootstrapSession session = null;
                    try
                    {
                        Require(PackBootstrap.StartDirectory(root, language, ref config, out session), "Start " + directory);
                        foreach (string name in names)
                        {
                            Require(session.PrepareActions(new[] { name }, session.ProjectId,
                                out ActionPreparation preparation), "Prepare " + name);
                            Require(Actions.Resolve(name, session.ProjectId, out ActionResolved action), "Resolve " + name);
                            Require(HarkenEngine.Update(), "Update");
                            Require(Actions.Post(in action, Emitter.None, 0, out ActionFiring firing), "Post " + name);
                            if (!firing.IsValid) throw new InvalidOperationException("Missing firing: " + name);
                            Require(preparation.Release(), "Release " + name);
                            ++count;
                        }
                        Require(session.Shutdown(), "Shutdown " + directory);
                        session = null;
                    }
                    finally { session?.Dispose(); }
                }
                if (count == 0) throw new InvalidOperationException("Export has no Action Pack pairs.");
                Console.WriteLine("[PackMetadata] packMetadataVersion " + PackMetadata.SupportedPackMetadataVersion);
                Console.WriteLine("[PackMetadata] native export PASS.");
                return true;
            }
            catch (Exception error)
            {
                Console.WriteLine("[PackMetadata] FAIL: " + error);
                return false;
            }
        }

        public static bool RunResidentBudget(string root)
        {
            const ulong budget = 64UL * 1024 * 1024;
            try
            {
                string[] paths = Directory.GetFiles(root, "*.hrp")
                    .Where(p => Path.GetFileNameWithoutExtension(p) != "Init")
                    .OrderBy(p => p, StringComparer.Ordinal).ToArray();
                string[] names = paths.Select(Path.GetFileNameWithoutExtension).ToArray();
                ulong bytes = 0;
                foreach (string path in paths)
                {
                    using var header = new BinaryReader(File.OpenRead(path));
                    if (header.ReadUInt32() != 0x32505248)
                        throw new InvalidDataException("Expected an HRP2 publication: " + path);
                    bytes += header.ReadUInt32();
                }
                if (bytes <= budget) throw new InvalidDataException("Supply Action Packs totalling more than 64 MiB.");
                Console.WriteLine($"[Resident budget] {names.Length} Action Packs, {bytes} canonical bytes.");
                foreach (ulong requested in new[] { 0UL, budget })
                {
                    var config = HarkenEngine.DefaultConfig();
                    if (config.maxPackDecodeStagingBytes != 0)
                        throw new InvalidOperationException("Expected a demand-based default staging budget.");
                    config.maxPackDecodeStagingBytes = requested;
                    config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                    Require(PackBootstrap.StartDirectory(root, null, ref config, out var session), "StartDirectory");
                    using (session)
                    {
                        VnResult result = session.PrepareActions(names, session.ProjectId, out var preparation);
                        Console.WriteLine($"[Resident budget] Requested {requested} bytes, PrepareActions returned {result}.");
                        if (requested == 0)
                        {
                            Require(result, "PrepareActions with default config");
                            foreach (string name in names)
                                Require(Actions.Resolve(name, session.ProjectId, out _), "Resolve " + name);
                            Require(preparation.Release(), "Release");
                        }
                        else
                        {
                            if (result != VnResult.VN_PACK_RESIDENT_CAPACITY)
                                throw new InvalidOperationException("Expected explicit staging limit to reject the batch.");
                            foreach (string name in names)
                                if (Actions.Resolve(name, session.ProjectId, out _) != VnResult.VN_UNKNOWN_ID)
                                    throw new InvalidOperationException("Rejected batch published " + name);
                            Require(session.PrepareActions(new[] { names[0] }, session.ProjectId, out var retry), "Retry one Pack");
                            Require(retry.Release(), "Release retry");
                        }
                        Require(session.Shutdown(), "Shutdown");
                    }
                }
                return true;
            }
            catch (Exception error)
            {
                Console.WriteLine("[Resident budget] FAIL: " + error);
                return false;
            }
        }

        private static void Require(VnResult result, string operation)
        {
            if (result != VnResult.VN_OK)
                throw new InvalidOperationException(operation + ": " + result);
        }
    }
}
