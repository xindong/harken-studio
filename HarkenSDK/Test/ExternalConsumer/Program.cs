using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
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
                CheckMissingFileRecovery(args[1], true);
                CheckMissingFileRecovery(args[1], false);
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
        public GameplayPublication Publication;
        public readonly Dictionary<ulong, byte[]> Bytes = new Dictionary<ulong, byte[]>();
        public int Calls;
        public bool Throw;
    }

    private static VnResult Provide(ulong id, string locator, uint variant, ulong hash, out byte[] bytes, object state)
    {
        var provider = (Provider)state;
        ++provider.Calls;
        if (provider.Throw) throw new InvalidOperationException("Expected provider failure.");
        Require(provider.Publication.TryFindPack(id, out PackManifestEntry pack) &&
            pack.Path == locator && pack.VariantKey == variant && pack.ContentHash == hash, "Provider identity mismatch.");
        return provider.Bytes.TryGetValue(id, out bytes) ? VnResult.VN_OK : VnResult.VN_CONTENT_NOT_LOADED;
    }

    private static void CheckStartupProviderFailure(string root)
    {
        Expect(GameplayPublication.Read(root, null, out GameplayPublication publication, out string error), VnResult.VN_OK, error);
        Require(publication.TryFindAction("Play_U4_A", out GameplayAction action), "Fixture Action missing.");
        var provider = new Provider { Publication = publication, Throw = true };
        VnConfig config = HarkenEngine.DefaultConfig();
        config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
        Expect(PackBootstrap.Start(publication, File.ReadAllBytes(Path.Combine(root, publication.InitPack.Path)),
            new[] { action.OwnerPackStableId }, Provide, provider, ref config, out PackBootstrapSession session),
            VnResult.VN_PACK_PROVIDER_FAILED, "Throwing startup provider");
        Require(session == null, "Failed startup must not leave a session.");
        Console.WriteLine("Startup provider exception isolation PASS.");
    }

    private static void CheckMissingFileRecovery(string root, bool withGameplayCatalog)
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
            Expect(GameplayPublication.Read(copy, null, out GameplayPublication publication, out string error), VnResult.VN_OK, error);
            Require(publication.TryFindAction("Play_U4_A", out GameplayAction action), "Fixture Action missing.");
            Require(publication.TryFindPack(action.OwnerPackStableId, out PackManifestEntry pack), "Content missing.");
            if (!withGameplayCatalog)
            {
                string manifestPath = Path.Combine(copy, PackManifest.ManifestFileName);
                JsonNode manifest = JsonNode.Parse(File.ReadAllText(manifestPath));
                Require(manifest.AsObject().Remove("gameplayCatalog"), "Fixture gameplay catalog missing.");
                File.WriteAllText(manifestPath, manifest.ToJsonString());
            }
            string missing = Path.Combine(copy, pack.Path);
            byte[] content = File.ReadAllBytes(missing);
            File.Delete(missing);
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            Expect(PackBootstrap.Start(Path.Combine(copy, publication.InitPack.Path), copy,
                new[] { action.OwnerPackStableId }, ref config, out session), VnResult.VN_CONTENT_NOT_LOADED, "Missing file startup");
            Require(session != null && session.IsActive && !session.IsStartupComplete &&
                session.FailedStartupPackStableId == action.OwnerPackStableId, "Missing Content must retain a recoverable session.");
            Require((session.Publication != null) == withGameplayCatalog, "Unexpected gameplay catalog state.");
            Expect(session.AcquirePack(ulong.MaxValue, out _), VnResult.VN_UNKNOWN_ID, "Unknown Content identity");
            byte[] corrupt = (byte[])content.Clone();
            corrupt[corrupt.Length - 1] ^= 1;
            File.WriteAllBytes(missing, corrupt);
            Expect(session.PrepareStartupPacks(), VnResult.VN_PACK_HASH_MISMATCH, "Corrupt replacement Content");
            Require(session.IsActive && !session.IsStartupComplete, "Failed recovery must retain the session.");
            File.WriteAllBytes(missing, content);
            Expect(session.PrepareStartupPacks(), VnResult.VN_OK, "Explicit file recovery");
            Require(session.IsStartupComplete, "Restored file must complete the same session.");
            if (withGameplayCatalog)
                Expect(Actions.Post(action.Name, Emitter.None, out _), VnResult.VN_OK, "Recovered Action name");
            else
            {
                Expect(Actions.Post(action.Name, Emitter.None, out _), VnResult.VN_CAPABILITY_UNAVAILABLE,
                    "Legacy name lookup without a catalog");
                Expect(Actions.Post(action.ActionStableId, Emitter.None, out _), VnResult.VN_OK, "Recovered Action identity");
            }
            Expect(session.Shutdown(), VnResult.VN_OK, "Recovered session shutdown");
            Console.WriteLine("Missing/corrupt file recovery (gameplay catalog=" + withGameplayCatalog + ") PASS.");
        }
        finally
        {
            session?.Dispose();
            if (Directory.Exists(copy)) Directory.Delete(copy, true);
        }
    }

    private static void CheckHost(string root, bool fromBytes)
    {
        Expect(GameplayPublication.Read(root, null, out GameplayPublication publication, out string error), VnResult.VN_OK, error);
        Require(publication.TryFindAction("Play_U4_A", out GameplayAction action), "Fixture Action missing.");
        byte[] initBytes = File.ReadAllBytes(Path.Combine(root, publication.InitPack.Path));
        var provider = new Provider { Publication = publication };
        foreach (PackManifestEntry pack in publication.Packs)
            if (pack.PackStableId != publication.InitPackStableId)
                provider.Bytes.Add(pack.PackStableId, File.ReadAllBytes(Path.Combine(root, pack.Path)));
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
            ulong[] startup = { action.OwnerPackStableId };
            VnResult started = fromBytes
                ? PackBootstrap.Start(publication, initBytes, startup, Provide, provider, ref config, out session)
                : PackBootstrap.Start(Path.Combine(root, publication.InitPack.Path), root, startup, ref config, out session);
            Expect(started, VnResult.VN_OK, "Host startup");
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
            Expect(Actions.Post(action.Name, emitter, out ActionFiring firing), VnResult.VN_OK, "Action");
            Expect(firing.Query(out _), VnResult.VN_OK, "Query firing");
            Expect(firing.Stop(0), VnResult.VN_OK, "Stop firing");
            // Stop is asynchronous. The production null device has no audio pump;
            // terminal release is exercised by the separate Unity player fixture.
            Expect(firing.Release(), VnResult.VN_IN_USE, "Release before audio retirement");
            Expect(emitter.Destroy(), VnResult.VN_OK, "Destroy emitter");
            Expect(emitter.SetDistanceScale(1), VnResult.VN_STALE_HANDLE, "Destroyed emitter distance scale");
            Expect(Listener.SetDistanceReference(emitter), VnResult.VN_STALE_HANDLE, "Destroyed listener distance reference");
            Expect(HarkenEngine.Update(), VnResult.VN_OK, "Pump callbacks");
            if (fromBytes) CheckProviderFailure(publication, provider, session);
            Expect(PackBootstrap.Start(publication, initBytes, Array.Empty<ulong>(), Provide, provider,
                ref config, out PackBootstrapSession second), VnResult.VN_ALREADY_INITIALIZED, "Second host");
            Require(second == null && session.IsActive, "Second startup must not take ownership.");
            Expect(session.Shutdown(), VnResult.VN_OK, "Shutdown");
        }
        finally { session?.Dispose(); }
        notify.Dispose(); notify.Dispose(); log.Dispose(); log.Dispose();
        Require(!notify.IsActive && !log.IsActive, "Host callback registrations must retire idempotently.");
        Console.WriteLine($"External host {(fromBytes ? "bytes" : "files")}, Action/spatial/lifecycle PASS; notifications={notifications}.");
    }

    private static void CheckProviderFailure(GameplayPublication publication, Provider provider, PackBootstrapSession session)
    {
        Require(publication.TryFindAction("Play_U4_B", out GameplayAction other), "Second Action missing.");
        byte[] good = provider.Bytes[other.OwnerPackStableId];
        provider.Bytes.Remove(other.OwnerPackStableId);
        Expect(session.AcquirePack(other.OwnerPackStableId, out _), VnResult.VN_CONTENT_NOT_LOADED, "Missing bytes");
        int calls = provider.Calls;
        Expect(Actions.Post(other.Name, Emitter.None, out _), VnResult.VN_CONTENT_NOT_LOADED, "Unprepared Action");
        Require(provider.Calls == calls, "Action cannot implicitly retry resource loading.");
        provider.Throw = true;
        Expect(session.AcquirePack(other.OwnerPackStableId, out _), VnResult.VN_PACK_PROVIDER_FAILED, "Throwing provider");
        provider.Throw = false;
        byte[] corrupt = (byte[])good.Clone(); corrupt[corrupt.Length - 1] ^= 1;
        provider.Bytes[other.OwnerPackStableId] = corrupt;
        Expect(session.AcquirePack(other.OwnerPackStableId, out _), VnResult.VN_PACK_HASH_MISMATCH, "Bad bytes");
        provider.Bytes[other.OwnerPackStableId] = good;
        Expect(session.AcquirePack(other.OwnerPackStableId, out Pack pack), VnResult.VN_OK, "Explicit retry");
        Require(pack.IsValid && session.IsActive, "Explicit retry must preserve the session.");
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
