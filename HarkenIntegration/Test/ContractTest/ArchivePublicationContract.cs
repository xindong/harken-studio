using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using Harken;

namespace Harken.Test
{
    internal static class ArchivePublicationContract
    {
        public static bool Run(string root)
        {
            string work = Path.Combine(Path.GetTempPath(), "harken-archive-contract-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(work);
            try
            {
                string name = Directory.EnumerateFiles(root, "*.json")
                    .Select(Path.GetFileNameWithoutExtension)
                    .First(n => n != "Init" && File.Exists(Path.Combine(root, n + ".bytes")));
                string zip = Path.Combine(work, "Packs.zip");
                Create(zip, archive =>
                {
                    AddPair(archive, root, "Init");
                    AddPair(archive, root, name);
                    Add(archive, "Bad.bytes", File.ReadAllBytes(Path.Combine(root, name + ".bytes")).Take(8).ToArray());
                    string badMetadata = File.ReadAllText(Path.Combine(root, name + ".json")).Replace('"' + name + '"', "\"Bad\"");
                    Add(archive, "Bad.json", Encoding.UTF8.GetBytes(badMetadata));
                    Add(archive, "Partial/" + name + ".json", File.ReadAllBytes(Path.Combine(root, name + ".json")));
                });
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                Expect(PackBootstrap.StartArchive(zip, "French", ref config, out var session), VnResult.VN_OK, "open archive");
                using (session)
                {
                    // Init succeeds independently of the invalid, unrequested Action.
                    Expect(session.PrepareActions(new[] { "Bad" }, session.ProjectId, out _), VnResult.VN_BAD_PACK, "bad Action");
                    // An atomic publisher can replace the path without changing this live session.
                    File.Move(zip, zip + ".previous");
                    File.WriteAllBytes(zip, Array.Empty<byte>());
                    string otherCase = char.IsUpper(name[0]) ? char.ToLowerInvariant(name[0]) + name.Substring(1)
                        : char.ToUpperInvariant(name[0]) + name.Substring(1);
                    Expect(session.PrepareActions(new[] { otherCase }, session.ProjectId, out _),
                        VnResult.VN_CONTENT_NOT_LOADED, "case-sensitive Action");
                    Expect(session.PrepareActions(new[] { name }, session.ProjectId, out var preparation),
                        VnResult.VN_OK, "prepare generic fallback after failure");
                    Expect(session.SetLanguageAsync("Partial").GetAwaiter().GetResult(), VnResult.VN_BAD_PACK,
                        "partial localized pair cannot replace the generic Action");
                    Expect(Actions.Resolve(name, session.ProjectId, out var action), VnResult.VN_OK, "resolve retained generic Action");
                    Expect(Actions.Post(in action, Emitter.None, 0, out var firing), VnResult.VN_OK, "post retained generic Action");
                    if (!firing.IsValid) throw new InvalidOperationException("Missing firing after failed language change.");
                    Expect(preparation.Release(), VnResult.VN_OK, "release Action");
                    Expect(session.Shutdown(), VnResult.VN_OK, "shutdown archive");
                }
                File.Delete(zip);
                File.Move(zip + ".previous", zip);
                Expect(PackBootstrap.StartDirectory(work, null, ref config, out session), VnResult.VN_OK,
                    "ordinary game directory startup");
                using (session) Expect(session.Shutdown(), VnResult.VN_OK, "shutdown directory startup");
                var started = PackBootstrap.StartAsync(PackSource.FromDirectory(work, "French"), config)
                    .GetAwaiter().GetResult();
                Expect(started.Result, VnResult.VN_OK, "async game directory startup");
                using (started.Session)
                {
                    Expect(started.Session.PrepareActions(new[] { name }, started.Session.ProjectId, out var prepared),
                        VnResult.VN_OK, "prepare from async archive session");
                    Expect(Actions.Resolve(name, started.Session.ProjectId, out var action), VnResult.VN_OK, "resolve prepared Action");
                    Expect(Actions.Post(in action, Emitter.None, 0, out var firing), VnResult.VN_OK, "post after async startup");
                    if (!firing.IsValid) throw new InvalidOperationException("Missing firing after async startup.");
                    Expect(prepared.Release(), VnResult.VN_OK, "release after async startup");
                    Expect(started.Session.Shutdown(), VnResult.VN_OK, "shutdown after async startup");
                }
                CheckMetadataChecksum(zip, root, ref config);
                // An exclusive writer requires every native and managed archive handle to have closed.
                using (File.Open(zip, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
                File.WriteAllBytes(zip, new byte[] { 0x50, 0x4b, 3, 4 });
                Expect(PackBootstrap.StartArchive(zip, null, ref config, out session), VnResult.VN_BAD_PACK, "truncated ZIP");
                if (session != null) throw new InvalidOperationException("Failed startup returned a session.");
                CheckSize(zip, root, ref config, CompressionLevel.Optimal, VnResult.VN_BAD_PACK);
                CheckSize(zip, root, ref config, CompressionLevel.NoCompression, VnResult.VN_OK);
                using (File.Open(zip, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
                Console.WriteLine("[PackArchive] native archive publication PASS.");
                return true;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("[PackArchive] FAIL: " + error);
                return false;
            }
            finally { Directory.Delete(work, true); }
        }

        private static void CheckMetadataChecksum(string zip, string root, ref VnConfig config)
        {
            // Both suffixes are valid JSON whitespace and have the same declared length.
            byte[] json = File.ReadAllBytes(Path.Combine(root, "Init.json")).Concat(new byte[] { 32 }).ToArray();
            void WriteArchive()
            {
                Create(zip, archive =>
                {
                    Add(archive, "Init.json", json, CompressionLevel.NoCompression);
                    Add(archive, "Init.bytes", File.ReadAllBytes(Path.Combine(root, "Init.bytes")));
                });
            }
            WriteArchive();
            byte[] stored = File.ReadAllBytes(zip);
            int offset = stored.AsSpan().IndexOf(json);
            if (offset < 0) throw new InvalidOperationException("Expected stored Init.json.");
            stored[offset + json.Length - 1] = 9;
            File.WriteAllBytes(zip, stored);
            Expect(PackBootstrap.StartArchive(zip, null, ref config, out var rejected),
                VnResult.VN_BAD_PACK, "mismatched JSON checksum");
            if (rejected != null) throw new InvalidOperationException("Bad CRC returned a session.");
            json[json.Length - 1] = 9;
            WriteArchive();
            Expect(PackBootstrap.StartArchive(zip, null, ref config, out var valid),
                VnResult.VN_OK, "identical JSON with correct checksum");
            using (valid) Expect(valid.Shutdown(), VnResult.VN_OK, "shutdown corrected archive");
        }

        private static void CheckSize(string zip, string root, ref VnConfig config, CompressionLevel compression, VnResult expected)
        {
            // A valid JSON document can exceed the compressed-member bound; stored publications retain support.
            var json = new byte[16 * 1024 * 1024 + 1];
            Array.Fill(json, (byte)' ');
            File.ReadAllBytes(Path.Combine(root, "Init.json")).CopyTo(json, 0);
            Create(zip, archive =>
            {
                Add(archive, "Init.json", json, compression);
                Add(archive, "Init.bytes", File.ReadAllBytes(Path.Combine(root, "Init.bytes")));
            });
            var result = PackBootstrap.StartArchive(zip, null, ref config, out var session);
            using (session)
            {
                Expect(result, expected, "expanded size (" + compression + ")");
                if (session != null) Expect(session.Shutdown(), VnResult.VN_OK, "shutdown stored archive");
            }
        }

        private static void Create(string path, Action<ZipArchive> fill)
        {
            using var file = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None);
            using var archive = new ZipArchive(file, ZipArchiveMode.Create);
            fill(archive);
        }

        private static void AddPair(ZipArchive archive, string root, string name)
        {
            foreach (string suffix in new[] { ".bytes", ".json" })
                Add(archive, name + suffix, File.ReadAllBytes(Path.Combine(root, name + suffix)));
        }

        private static void Add(ZipArchive archive, string name, byte[] bytes,
            CompressionLevel compression = CompressionLevel.Optimal)
        {
            using var entry = archive.CreateEntry(name, compression).Open();
            entry.Write(bytes, 0, bytes.Length);
        }

        private static void Expect(VnResult result, VnResult expected, string operation)
        {
            if (result != expected)
                throw new InvalidOperationException(operation + ": expected " + expected + ", got " + result);
        }
    }
}
