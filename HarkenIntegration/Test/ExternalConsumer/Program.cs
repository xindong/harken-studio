using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Harken;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            Require(typeof(HarkenEngine).Assembly != typeof(Program).Assembly, "SDK must be a separate assembly.");
            if (args.Length == 2 && args[0] == "--surface") CheckSurface(args[1]);
            else if (args.Length == 2 && args[0] == "--companion")
            {
                NativeLibrary.SetDllImportResolver(typeof(HarkenEngine).Assembly, ResolveNative);
                CheckCompanionSource(args[1]);
            }
            else if (args.Length == 2 && args[0] == "--host")
            {
                NativeLibrary.SetDllImportResolver(typeof(HarkenEngine).Assembly, ResolveNative);
                CheckStartupProviderFailure(args[1]);
                CheckMissingFileRecovery(args[1]);
                CheckHost(args[1], false);
                CheckHost(args[1], true);
                Require(Harken.Test.RemoteCommunicationContract.RunNative() == 0, "External host Remote failed.");
            }
            else throw new ArgumentException("Expected --surface policy.json, --host publication or --companion directory.");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }

    private static void CheckCompanionSource(string directory)
    {
        int reads = 0;
        var source = PackSource.FromReader(path =>
        {
            ++reads;
            string file = Path.Combine(directory, path);
            return File.Exists(file) ? File.ReadAllBytes(file) : null;
        });
        var config = HarkenEngine.DefaultConfig();
        config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
        Require(PackBootstrap.Start(source, ref config, out PackBootstrapSession session) == VnResult.VN_OK, "Companion startup");
        using (session)
        {
            Require(reads == 2 && session.Publication == null, "Init-only source has no fabricated publication");
            ActionPreparationResult ready = session.PrepareActionsAsync(new[] { "Play_Head" }, session.ProjectId).GetAwaiter().GetResult();
            Require(ready.Result == VnResult.VN_OK && reads == 4, "External preparation reads one Action pair");
            Require(Actions.Resolve("Play_Head", session.ProjectId, out ActionResolved action) == VnResult.VN_OK, "Companion resolve");
            Require(Actions.Post(in action, Emitter.None, 1, out ActionFiring firing) == VnResult.VN_OK, "Companion post");
            Require(firing.IsValid, "Accepted firing");
            Require(ready.Preparation.Release() == VnResult.VN_OK, "Companion lease release");
        }
        Console.WriteLine("Separate non-friend companion source, preparation and playback PASS.");
    }

    private static IntPtr ResolveNative(string name, Assembly assembly, DllImportSearchPath? search)
    {
        string directory = Environment.GetEnvironmentVariable("HARKEN_NATIVE_DIR");
        if (name != "HarkenCore" || string.IsNullOrEmpty(directory)) return IntPtr.Zero;
        string file = OperatingSystem.IsWindows() ? "HarkenCore.dll" :
            OperatingSystem.IsMacOS() ? "libHarkenCore.dylib" : "libHarkenCore.so";
        return NativeLibrary.Load(Path.Combine(directory, file));
    }

    private static void CheckSurface(string policyPath)
    {
        Assembly sdk = typeof(HarkenEngine).Assembly;
        string[] friends = sdk.GetCustomAttributes<InternalsVisibleToAttribute>().Select(x => x.AssemblyName).ToArray();
        string[] trusted = { "Harken.Unity", "Harken.Unity.Editor", "Harken.Unity.EditModeTests", "Harken.Unity.PlayModeTests" };
        Require(friends.OrderBy(x => x).SequenceEqual(trusted.OrderBy(x => x)), "Unexpected friend assembly.");
        Require(!friends.Contains(typeof(Program).Assembly.GetName().Name), "Consumer cannot be a friend.");
        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(policyPath));
        JsonElement policy = document.RootElement;
        bool ExpectedPublic(JsonProperty symbol) =>
            policy.GetProperty("groups").GetProperty(symbol.Value.GetString()).GetProperty("disposition").GetString() != "move-internal";
        foreach (JsonProperty symbol in policy.GetProperty("managed_generation").GetProperty("types").EnumerateObject())
        {
            Type type = sdk.GetType("Harken." + symbol.Name, true);
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
            foreach (JsonElement prefix in excluded.GetProperty(category).EnumerateArray())
                Require(!names.Any(name => name.StartsWith(prefix.GetString(), StringComparison.Ordinal)),
                    "Excluded native contract in SDK: " + prefix.GetString());
        }
        RequireNoExcluded("types", sdk.GetTypes().Select(type => type.Name));
        RequireNoExcluded("constants", typeof(VnConstants).GetFields(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic).Select(field => field.Name));
        RequireNoExcluded("results", Enum.GetNames(typeof(VnResult)));
        RequireNoExcluded("functions", sdk.GetType("Harken.HarkenNative", true).GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic).Select(method => method.Name));
        Console.WriteLine("Separate, non-friend consumer and all generated type/constant visibility PASS.");
    }

    private sealed class Provider
    {
        public readonly Dictionary<string, byte[]> Bytes = new Dictionary<string, byte[]>();
        public int Calls;
        public bool Throw;
        public ulong Revision;
        public byte[] Read(string path)
        {
            ++Calls;
            if (Throw) throw new IOException("Expected provider failure.");
            return Bytes.TryGetValue(path, out byte[] bytes) ? bytes : null;
        }
    }

    private static void CheckStartupProviderFailure(string root)
    {
        var provider = new Provider { Throw = true };
        var config = HarkenEngine.DefaultConfig();
        config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
        Expect(PackBootstrap.Start(PackSource.FromReader(provider.Read), ref config, out PackBootstrapSession session),
            VnResult.VN_PACK_PROVIDER_FAILED, "Throwing startup provider");
        Require(session == null, "Failed startup must not leave a session.");
        Console.WriteLine("Startup provider exception isolation PASS.");
    }

    private static void CheckMissingFileRecovery(string root)
    {
        string copy = Path.Combine(Path.GetTempPath(), "HarkenMissingContent_" + Guid.NewGuid().ToString("N"));
        PackBootstrapSession session = null;
        try
        {
            foreach (string file in Directory.GetFiles(root, "*", SearchOption.AllDirectories))
            {
                string target = Path.Combine(copy, Path.GetRelativePath(root, file));
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                File.Copy(file, target);
            }
            const string action = "Play_U4_A";
            string missing = Path.Combine(copy, action + ".bytes");
            string json = Path.Combine(copy, action + ".json");
            byte[] content = File.ReadAllBytes(missing), metadata = File.ReadAllBytes(json);
            File.Delete(missing);
            File.Delete(json);
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            Expect(PackBootstrap.StartDirectory(copy, null, ref config, out session), VnResult.VN_OK, "Init-only startup");
            Expect(session.PrepareActions(new[] { action }, session.ProjectId, out _),
                VnResult.VN_CONTENT_NOT_LOADED, "Missing Action pair");
            Require(session.IsActive, "Missing Content must retain the session.");
            File.WriteAllBytes(json, metadata);
            Expect(session.PrepareActions(new[] { action }, session.ProjectId, out _), VnResult.VN_BAD_PACK, "Half Action pair");
            byte[] corrupt = (byte[])content.Clone();
            corrupt[corrupt.Length - 1] ^= 1;
            File.WriteAllBytes(missing, corrupt);
            Require(session.PrepareActions(new[] { action }, session.ProjectId, out _) != VnResult.VN_OK,
                "Corrupt replacement cannot be accepted.");
            Require(session.IsActive, "Failed recovery must retain the session.");
            File.WriteAllBytes(missing, content);
            Expect(session.PrepareActions(new[] { action }, session.ProjectId, out ActionPreparation ready),
                VnResult.VN_OK, "Explicit file recovery");
            Expect(Actions.Post(action, Emitter.None, out _), VnResult.VN_OK, "Recovered Action name");
            Expect(ready.Release(), VnResult.VN_OK, "Release recovered Action");
            Expect(session.Shutdown(), VnResult.VN_OK, "Recovered session shutdown");
            Console.WriteLine("Missing/half/corrupt pair recovery PASS.");
        }
        finally
        {
            session?.Dispose();
            if (Directory.Exists(copy)) Directory.Delete(copy, true);
        }
    }

    private static void CheckHost(string root, bool fromBytes)
    {
        var provider = new Provider();
        foreach (string file in Directory.GetFiles(root, "*", SearchOption.AllDirectories))
            provider.Bytes.Add(Path.GetRelativePath(root, file).Replace('\\', '/'), File.ReadAllBytes(file));
        PackSource source = fromBytes ? PackSource.FromReader(provider.Read, revision: () => provider.Revision)
            : PackSource.FromDirectory(root);
        VnConfig config = HarkenEngine.DefaultConfig();
        config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
        object marker = new object();
        int hostThread = Environment.CurrentManagedThreadId;
        int notifications = 0;
        using HarkenCallbacks.Registration notify = HarkenCallbacks.InstallNotification(ref config,
            (in VnNotification notification, object state) =>
            {
                Require(ReferenceEquals(state, marker) && hostThread == Environment.CurrentManagedThreadId,
                    "Notification must preserve host state and pump thread.");
                ++notifications;
            }, marker);
        using HarkenCallbacks.Registration log = HarkenCallbacks.InstallLog(ref config,
            (level, code, message, state) => Require(ReferenceEquals(state, marker), "Log state changed."), marker);
        GC.Collect(); GC.WaitForPendingFinalizers();
        PackBootstrapSession session = null;
        try
        {
            Expect(PackBootstrap.Start(source, ref config, out session), VnResult.VN_OK, "Host startup");
            Expect(session.PrepareActions(new[] { "Play_U4_A" }, session.ProjectId, out ActionPreparation prepared),
                VnResult.VN_OK, "Host preparation");
            Expect(HarkenEngine.QueryRemoteCommunication(out VnRemoteStatus status), VnResult.VN_OK, "Default Remote status");
            Require(status.state == (uint)VnRemoteState.VN_REMOTE_STOPPED, "Remote must remain opt-in.");
            Expect(Emitter.CreateStateOnly(out Emitter stateOnly), VnResult.VN_OK, "Create state-only emitter");
            Expect(stateOnly.SetPosition(new VnVec3 { x = 1 }), VnResult.VN_CAPABILITY_UNAVAILABLE, "State-only has no pose");
            Expect(stateOnly.SetDistanceScale(2), VnResult.VN_CAPABILITY_UNAVAILABLE, "State-only has no distance scale");
            Expect(Listener.SetDistanceReference(stateOnly), VnResult.VN_CAPABILITY_UNAVAILABLE, "State-only cannot be a distance reference");
            Expect(stateOnly.Stop(0), VnResult.VN_OK, "State-only stop");
            Expect(stateOnly.Destroy(), VnResult.VN_OK, "Destroy state-only emitter");
            Expect(Emitter.Create(out Emitter emitter), VnResult.VN_OK, "Create emitter");
            Expect(emitter.SetPosition(new VnVec3 { x = 1 }), VnResult.VN_OK, "Emitter position");
            Expect(emitter.SetDistanceScale(2), VnResult.VN_OK, "Emitter distance scale");
            Expect(emitter.SetDistanceScale(float.NaN), VnResult.VN_INVALID_ARG, "Invalid emitter distance scale");
            Expect(Listener.Set(default, new VnVec3 { z = 1 }, new VnVec3 { y = 1 }), VnResult.VN_OK, "Listener");
            Expect(Listener.SetDistanceReference(emitter), VnResult.VN_OK, "Listener distance reference");
            Expect(Listener.SetDistanceReference(Emitter.None), VnResult.VN_OK, "Clear listener distance reference");
            Expect(Actions.Post("Play_U4_A", emitter, out ActionFiring firing), VnResult.VN_OK, "Action");
            Expect(firing.Query(out _), VnResult.VN_OK, "Query firing");
            Expect(firing.Stop(0), VnResult.VN_OK, "Stop firing");
            Expect(firing.Release(), VnResult.VN_IN_USE, "Release before audio retirement");
            Expect(emitter.Destroy(), VnResult.VN_OK, "Destroy emitter");
            Expect(emitter.SetDistanceScale(1), VnResult.VN_STALE_HANDLE, "Destroyed emitter distance scale");
            Expect(Listener.SetDistanceReference(emitter), VnResult.VN_STALE_HANDLE, "Destroyed listener distance reference");
            Expect(HarkenEngine.Update(), VnResult.VN_OK, "Pump callbacks");
            if (fromBytes) CheckProviderFailure(provider, session);
            Expect(PackBootstrap.Start(source, ref config, out PackBootstrapSession second),
                VnResult.VN_ALREADY_INITIALIZED, "Second host");
            Require(second == null && session.IsActive, "Second startup must not take ownership.");
            Expect(prepared.Release(), VnResult.VN_OK, "Host preparation release");
            Expect(session.Shutdown(), VnResult.VN_OK, "Shutdown");
        }
        finally { session?.Dispose(); }
        notify.Dispose(); notify.Dispose(); log.Dispose(); log.Dispose();
        Require(!notify.IsActive && !log.IsActive, "Host callback registrations must retire idempotently.");
        Console.WriteLine($"External host {(fromBytes ? "bytes" : "files")}, Action/spatial/lifecycle PASS; notifications={notifications}.");
    }

    private static void CheckProviderFailure(Provider provider, PackBootstrapSession session)
    {
        const string name = "Play_U4_B", binary = name + ".bytes", json = name + ".json";
        byte[] good = provider.Bytes[binary], metadata = provider.Bytes[json];
        ++provider.Revision;
        provider.Bytes.Remove(binary); provider.Bytes.Remove(json);
        Expect(session.PrepareActions(new[] { name }, session.ProjectId, out _),
            VnResult.VN_CONTENT_NOT_LOADED, "Missing pair");
        int calls = provider.Calls;
        Require(Actions.Post(name, Emitter.None, out _) != VnResult.VN_OK, "Unprepared Action");
        Require(provider.Calls == calls, "Action cannot implicitly retry resource loading.");
        provider.Throw = true;
        Expect(session.PrepareActions(new[] { name }, session.ProjectId, out _),
            VnResult.VN_PACK_PROVIDER_FAILED, "Throwing provider");
        provider.Throw = false;
        ++provider.Revision;
        byte[] corrupt = (byte[])good.Clone(); corrupt[corrupt.Length - 1] ^= 1;
        provider.Bytes[binary] = corrupt; provider.Bytes[json] = metadata;
        Require(session.PrepareActions(new[] { name }, session.ProjectId, out _) != VnResult.VN_OK, "Bad bytes");
        ++provider.Revision;
        provider.Bytes[binary] = good;
        Expect(session.PrepareActions(new[] { name }, session.ProjectId, out ActionPreparation ready), VnResult.VN_OK, "Explicit retry");
        Require(ready.IsPrepared && session.IsActive, "Explicit retry must preserve the session.");
        Expect(ready.Release(), VnResult.VN_OK, "Recovered provider lease release");
    }

    // Compiled public signatures supplement the executed file/byte host flow.
    private static void SupportedCalls(Voice voice, VnObjectId id, HarkenVariableId typed, ActionFiring firing)
    {
        firing.Seek(100, VnActionSeekMode.VN_ACTION_SEEK_ABSOLUTE, out ActionSeekTicket seek);
        seek.Query(out _); seek.Release();
        Values.SetValue(id, 0.5f); Values.SetValue(typed, 0.5f);
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
