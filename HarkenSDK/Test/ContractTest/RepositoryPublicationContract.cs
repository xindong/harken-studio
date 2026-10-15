using System;
using System.IO;
using System.Linq;
using Harken;

namespace Harken.Test
{
    internal static class RepositoryPublicationContract
    {
        public static bool Run()
        {
            try
            {
                string root = Path.Combine(AppContext.BaseDirectory, "NativePublications");
                foreach (string category in new[] { "UnityFixture", "PlayableDemo" })
                    if (!Directory.Exists(Path.Combine(root, category)) ||
                        Directory.GetFiles(Path.Combine(root, category), PackManifest.ManifestFileName,
                            SearchOption.AllDirectories).Length == 0)
                        throw new InvalidOperationException("Native publication category is missing: " + category);
                string[] manifests = Directory.GetFiles(root, PackManifest.ManifestFileName,
                    SearchOption.AllDirectories);
                if (manifests.Length == 0) throw new InvalidOperationException("Native publications are missing.");
                foreach (string manifest in manifests.OrderBy(path => path, StringComparer.Ordinal))
                {
                    string directory = Path.GetDirectoryName(manifest);
                    string language = null;
                    string parent = Path.GetDirectoryName(directory);
                    if (parent != null && File.Exists(Path.Combine(parent, PackManifest.ManifestFileName)))
                    {
                        language = Path.GetFileName(directory);
                        directory = parent;
                    }
                    Require(GameplayPublication.Read(directory, language, out GameplayPublication publication,
                        out string error), "Read " + manifest + ": " + error);
                    byte[] init = File.ReadAllBytes(Path.Combine(directory, publication.InitPack.Path));
                    ulong[] content = publication.Packs.Where(pack => pack.PackStableId != publication.InitPackStableId)
                        .Select(pack => pack.PackStableId).ToArray();
                    VnConfig config = HarkenEngine.DefaultConfig();
                    config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                    PackBootstrapSession session = null;
                    try
                    {
                        Require(PackBootstrap.Start(publication, init, content, ReadContent, directory,
                            ref config, out session), "Start " + manifest);
                        if (session == null || !session.IsStartupComplete)
                            throw new InvalidOperationException("Incomplete publication: " + manifest);
                        Require(session.Shutdown(), "Shutdown " + manifest);
                        session = null;
                    }
                    finally { session?.Dispose(); }
                }
                Console.WriteLine("[Repository publications] " + manifests.Length +
                    " current fixture/sample publications loaded through native Runtime PASS.");
                return true;
            }
            catch (Exception error)
            {
                Console.WriteLine("[Repository publications] FAIL: " + error);
                return false;
            }
        }

        private static VnResult ReadContent(ulong id, string path, uint variant, ulong hash,
            out byte[] bytes, object state)
        {
            bytes = File.ReadAllBytes(Path.Combine((string)state, path));
            return VnResult.VN_OK;
        }

        private static void Require(VnResult result, string operation)
        {
            if (result != VnResult.VN_OK)
                throw new InvalidOperationException(operation + ": " + result);
        }
    }
}
