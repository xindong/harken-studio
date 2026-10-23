using System;
using System.IO;
using System.Linq;
using Harken;

namespace Harken.Test
{
    internal static class RepositoryPublicationContract
    {
        public static bool Run(string publicationRoot = null)
        {
            try
            {
                string root = publicationRoot ?? Path.Combine(AppContext.BaseDirectory,
                    "NativePublications", "UnityFixture", "CompanionPacks");
                byte[] init = File.ReadAllBytes(Path.Combine(root, "Init.bytes"));
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
                if (count == 0) throw new InvalidOperationException("Publication has no Action companion pairs.");
                uint version = (uint)init[4] | (uint)init[5] << 8 | (uint)init[6] << 16 | (uint)init[7] << 24;
                Console.WriteLine("[PackCompanion] schemaVersion " + version);
                Console.WriteLine("[PackCompanion] native publication PASS.");
                return true;
            }
            catch (Exception error)
            {
                Console.WriteLine("[PackCompanion] FAIL: " + error);
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
