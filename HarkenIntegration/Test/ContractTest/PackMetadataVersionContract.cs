using System;
using System.Text;
using Harken;

namespace Harken.Test
{
    internal static class PackMetadataVersionContract
    {
        internal static bool Run()
        {
            try
            {
                VersionContract();
                Console.WriteLine("[PackMetadata] metadata version contract PASS.");
                return true;
            }
            catch (InvalidOperationException error)
            {
                Console.WriteLine("[PackMetadata] FAIL: " + error.Message);
                return false;
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
                    Require(PackMetadata.Parse(Encoding.UTF8.GetBytes("{\"packMetadataVersion\":" + version + "," + fields +
                        ",\"metadata\":false,\"waveforms\":42}"), pack, init ? null : "Play_Head", "", default, out _),
                        "exact Pack metadata version " + version);
                foreach (string version in new[] { null, "3", "999", "\"4\"", "true", "null", "[]", "{}", "4.5", "-4",
                    "0", "4.0000000000000001", "3.9999999999999999", "4e999999999999999999999", "4e-999999999999999999999" })
                {
                    string field = version == null ? "" : "\"packMetadataVersion\":" + version + ",";
                    Assert(PackMetadata.Parse(Encoding.UTF8.GetBytes("{" + field + fields +
                        ",\"schema_version\":4,\"schemaVersion\":4,\"metadata\":{\"packMetadataVersion\":4}}"), pack, init ? null : "Play_Head", "", default, out _) ==
                        VnResult.VN_BAD_PACK, "reject Pack metadata version " + (version ?? "missing"));
                }
            }
            Require(PackMetadata.Parse(Encoding.UTF8.GetBytes("{\"packMetadataVersion\":4,\"variables\":[{\"name\":\"Choice\"," +
                "\"guid\":\"11111111-2222-3333-4444-555555555555\",\"labels\":[]}]}"), pack, null, "", default, out _), "empty labels");
            byte[] json = Encoding.UTF8.GetBytes("{\"packMetadataVersion\":4,\"variables\":[]}");
            foreach (byte[] invalid in new[] { null, Array.Empty<byte>(), new byte[7], new byte[8] })
                Assert(PackMetadata.Parse(json, invalid, null, "", default, out _) == VnResult.VN_BAD_PACK,
                    "missing or invalid Pack header");
            foreach (uint version in new[] { 4u, 10u, uint.MaxValue })
            {
                for (int i = 0; i < 4; ++i) pack[4 + i] = (byte)(version >> (i * 8));
                Require(PackMetadata.Parse(json, pack, null, "", default, out _),
                    "Pack metadata version is independent of the binary format version");
            }
        }

        private static void Require(VnResult result, string operation)
        { if (result != VnResult.VN_OK) throw new InvalidOperationException(operation + ": " + result); }
        private static void Assert(bool value, string operation)
        { if (!value) throw new InvalidOperationException(operation); }
    }
}
