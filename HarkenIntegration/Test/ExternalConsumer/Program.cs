using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;
using Harken;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            Require(typeof(HarkenEngine).Assembly != typeof(Program).Assembly, "Integration must be a separate assembly.");
            if (args.Length == 2 && args[0] == "--surface") CheckSurface(args[1]);
            else throw new ArgumentException("Expected --surface policy.json.");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }

    private static void CheckSurface(string policyPath)
    {
        Assembly integration = typeof(HarkenEngine).Assembly;
        string[] friends = integration.GetCustomAttributes<InternalsVisibleToAttribute>().Select(x => x.AssemblyName).ToArray();
        string[] trusted = { "Harken.Unity", "Harken.Unity.Editor" };
        Require(friends.OrderBy(x => x).SequenceEqual(trusted.OrderBy(x => x)), "Unexpected friend assembly.");
        Require(!friends.Contains(typeof(Program).Assembly.GetName().Name), "Consumer cannot be a friend.");
        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(policyPath));
        JsonElement policy = document.RootElement;
        bool ExpectedPublic(JsonProperty symbol) =>
            policy.GetProperty("groups").GetProperty(symbol.Value.GetString()).GetProperty("disposition").GetString() != "move-internal";
        foreach (JsonProperty symbol in policy.GetProperty("managed_generation").GetProperty("types").EnumerateObject())
        {
            Type type = integration.GetType("Harken." + symbol.Name, true);
            Require(type.IsVisible == ExpectedPublic(symbol), "Incorrect generated visibility: " + symbol.Name);
        }
        foreach (JsonProperty symbol in policy.GetProperty("managed_generation").GetProperty("constants").EnumerateObject())
        {
            FieldInfo field = typeof(VnConstants).GetField(symbol.Name, BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            Require(field != null && field.IsPublic == ExpectedPublic(symbol), "Incorrect constant visibility: " + symbol.Name);
        }
        JsonElement excluded = policy.GetProperty("managed_projection").GetProperty("excluded_prefixes");
        void RequireNoExcluded(string category, IEnumerable<string> names)
        {
            var admitted = policy.GetProperty("managed_projection").GetProperty("included_names");
            string[] exact = admitted.TryGetProperty(category, out JsonElement included)
                ? included.EnumerateArray().Select(item => item.GetString()).ToArray() : Array.Empty<string>();
            foreach (JsonElement prefix in excluded.GetProperty(category).EnumerateArray())
                Require(!names.Any(name => name.StartsWith(prefix.GetString(), StringComparison.Ordinal) && !exact.Contains(name)),
                    "Excluded native contract in managed API: " + prefix.GetString());
        }
        RequireNoExcluded("types", integration.GetTypes().Select(type => type.Name));
        RequireNoExcluded("constants", typeof(VnConstants).GetFields(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic).Select(field => field.Name));
        RequireNoExcluded("results", Enum.GetNames(typeof(VnResult)));
        RequireNoExcluded("functions", integration.GetType("Harken.HarkenNative", true).GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic).Select(method => method.Name));
        Console.WriteLine("Separate, non-friend consumer and all generated type/constant visibility PASS.");
    }

    private static void SupportedCalls(Voice voice, VnObjectId id, ActionFiring firing)
    {
        firing.Seek(100, VnActionSeekMode.VN_ACTION_SEEK_ABSOLUTE, out ActionSeekTicket seek);
        seek.Query(out _); seek.Release();
        Values.SetValue(id, 0.5f);
        Values.SetValue(id, id); Values.SetValue(id, "Label");
        voice.SetPan(0); voice.SetWidth(1); voice.SetPitch(1); voice.SetPosition(default);
        voice.SetOrientation(default); voice.SetSpatializationMix(1); voice.SetDistanceScale(1);
        voice.SetVolume(0, 0); voice.SetOutputVolume(0, 0); voice.SetFilter(0, 1000, 1, 1, 0);
        voice.SetSend(0, id, 0); voice.SetSendLevel(0, 0, 0); voice.ClearSend(0); voice.Stop(0);
        HarkenEngine.SetBusVolume(id, 0, 0); HarkenEngine.SetBusOutputVolume(id, 0, 0); HarkenEngine.SetBusMute(id, 0, 0);
    }

    private static void Expect(VnResult actual, VnResult expected, string operation)
        => Require(actual == expected, operation + ": " + actual + " != " + expected);
    private static void Require(bool condition, string message)
    { if (!condition) throw new InvalidOperationException(message); }
}
