using System;
using System.Text;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Harken;

namespace Harken.Test
{
    internal static class CompanionVersionContract
    {
        internal static bool Run()
        {
            try
            {
                VersionContract();
                CheckStorageBounds();
                Console.WriteLine("[PackCompanion] metadata version contract PASS.");
                return true;
            }
            catch (InvalidOperationException error)
            {
                Console.WriteLine("[PackCompanion] FAIL: " + error.Message);
                return false;
            }
        }

        private static void CheckStorageBounds()
        {
            byte[] expected = new byte[4096];
            expected[0] = 72;
            byte[] encoded;
            using (var buffer = new MemoryStream())
            {
                using (var stream = new GZipStream(buffer, CompressionLevel.Optimal, true))
                    stream.Write(expected, 0, expected.Length);
                encoded = buffer.ToArray();
            }
            Assert(PackStorage.CaptureBinary(encoded).SequenceEqual(expected), "compressed reader ownership");
            using (var file = new MemoryStream(encoded))
                Assert(PackStorage.ReadBinary(file).SequenceEqual(expected), "compressed file");
            var raw = PackStorage.CaptureBinary(expected);
            expected[0] = 35;
            Assert(raw[0] == 72, "raw reader ownership");
            foreach (uint size in new[] { (uint)PackStorage.MaxGzipMemberBytes + 1, 4095u, 4097u, 0u })
            {
                byte[] damaged = (byte[])encoded.Clone();
                for (int i = 0; i < 4; i++) damaged[damaged.Length - 4 + i] = (byte)(size >> (i * 8));
                try { PackStorage.CaptureBinary(damaged); }
                catch (PackStorage.InvalidMember) { continue; }
                throw new InvalidOperationException("Invalid expanded size accepted: " + size);
            }
            encoded[encoded.Length - 8] ^= 1;
            CheckStorageFailureSources(encoded);
        }

        private sealed class TestControlContext : SynchronizationContext { }

        private static void CheckStorageFailureSources(byte[] badCrc)
        {
            string root = Path.Combine(Path.GetTempPath(), "HarkenPackStorage-" + Guid.NewGuid().ToString("N"));
            string binary = Path.Combine(root, "Init.bytes"), json = Path.Combine(root, "Init.json");
            Directory.CreateDirectory(root);
            var previous = SynchronizationContext.Current;
            try
            {
                File.WriteAllBytes(binary, badCrc);
                File.WriteAllText(json, "{\"packMetadataVersion\":4,\"variables\":[]}");
                SynchronizationContext.SetSynchronizationContext(new TestControlContext());
                VnConfig config = default;
                Assert(PackBootstrap.StartDirectory(root, null, ref config, out _) == VnResult.VN_BAD_PACK,
                    "damaged gzip in directory");
                foreach (bool brokenProvider in new[] { false, true })
                {
                    byte[] Read(string path) => brokenProvider
                        ? throw new InvalidDataException("Storage provider unavailable.")
                        : File.ReadAllBytes(Path.Combine(root, path));
                    VnResult expected = brokenProvider ? VnResult.VN_PACK_PROVIDER_FAILED : VnResult.VN_BAD_PACK;
                    Assert(PackBootstrap.Start(PackSource.FromReader(Read), ref config, out _) == expected,
                        "compressed synchronous source error");
                    var source = PackSource.FromAsyncReader((path, _) => brokenProvider
                        ? Task.FromException<byte[]>(new InvalidDataException("Storage provider unavailable."))
                        : Task.FromResult(Read(path)));
                    Assert(PackBootstrap.StartAsync(source, config).GetAwaiter().GetResult().Result == expected,
                        "compressed asynchronous source error");
                }
            }
            finally
            {
                SynchronizationContext.SetSynchronizationContext(previous);
                File.Delete(binary);
                File.Delete(json);
                Directory.Delete(root);
            }
        }

        private static void VersionContract()
        {
            const string action = "\"name\":\"Play_Head\",\"guid\":\"11111111-2222-3333-4444-555555555555\"";
            byte[] pack = { (byte)'V', (byte)'S', (byte)'P', (byte)'K', 3, 0, 0, 0 };
            foreach (bool init in new[] { true, false })
            {
                string fields = init ? "\"variables\":[]" : action;
                foreach (string version in new[] { "4", "4.0", "4e0", "40e-1", "0.04E+2", "400e-2", "4e-0000" })
                    Require(PackCompanion.Parse(Encoding.UTF8.GetBytes("{\"packMetadataVersion\":" + version + "," + fields +
                        ",\"metadata\":false,\"waveforms\":42}"), pack, init ? null : "Play_Head", "", default, out _),
                        "exact companion version " + version);
                foreach (string version in new[] { null, "3", "999", "\"4\"", "true", "null", "[]", "{}", "4.5", "-4",
                    "0", "4.0000000000000001", "3.9999999999999999", "4e999999999999999999999", "4e-999999999999999999999" })
                {
                    string field = version == null ? "" : "\"packMetadataVersion\":" + version + ",";
                    Assert(PackCompanion.Parse(Encoding.UTF8.GetBytes("{" + field + fields +
                        ",\"schema_version\":4,\"schemaVersion\":4,\"metadata\":{\"packMetadataVersion\":4}}"), pack, init ? null : "Play_Head", "", default, out _) ==
                        VnResult.VN_BAD_PACK, "reject companion version " + (version ?? "missing"));
                }
            }
            Require(PackCompanion.Parse(Encoding.UTF8.GetBytes("{\"packMetadataVersion\":4,\"variables\":[{\"name\":\"Choice\"," +
                "\"guid\":\"11111111-2222-3333-4444-555555555555\",\"labels\":[]}]}"), pack, null, "", default, out _), "empty labels");
            byte[] json = Encoding.UTF8.GetBytes("{\"packMetadataVersion\":4,\"variables\":[]}");
            foreach (byte[] invalid in new[] { null, Array.Empty<byte>(), new byte[7], new byte[8] })
                Assert(PackCompanion.Parse(json, invalid, null, "", default, out _) == VnResult.VN_BAD_PACK,
                    "missing or invalid Pack header");
            foreach (uint version in new[] { 4u, 10u, uint.MaxValue })
            {
                for (int i = 0; i < 4; ++i) pack[4 + i] = (byte)(version >> (i * 8));
                Require(PackCompanion.Parse(json, pack, null, "", default, out _),
                    "companion version is independent of the binary format version");
            }
        }

        private static void Require(VnResult result, string operation)
        { if (result != VnResult.VN_OK) throw new InvalidOperationException(operation + ": " + result); }
        private static void Assert(bool value, string operation)
        { if (!value) throw new InvalidOperationException(operation); }
    }
}
