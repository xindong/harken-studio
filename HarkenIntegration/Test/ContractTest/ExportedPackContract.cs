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

        // Each translated member must fit 12 MiB alone; their combined staging must exceed it.
        public static bool RunResidentLanguageBudget(string root)
        {
            try
            {
                string language = Path.GetFileName(Directory.GetDirectories(root).Single());
                string[] names = Directory.GetFiles(Path.Combine(root, language), "*.hrp")
                    .Select(Path.GetFileNameWithoutExtension).OrderBy(n => n, StringComparer.Ordinal).ToArray();
                if (names.Length < 2) throw new InvalidDataException("Supply multiple translated Action Packs.");
                foreach (ulong budget in new[] { 0UL, 12UL * 1024 * 1024 })
                foreach (bool switchLanguage in new[] { false, true })
                {
                    var config = HarkenEngine.DefaultConfig();
                    config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                    config.maxPackDecodeStagingBytes = budget;
                    Require(PackBootstrap.StartDirectory(root, switchLanguage ? null : language,
                        ref config, out var session), "Start");
                    using (session)
                    {
                        var result = session.PrepareActions(names, session.ProjectId, out var preparation);
                        if (switchLanguage)
                        {
                            Require(result, "Prepare originals");
                            result = session.SetLanguageAsync(language).GetAwaiter().GetResult();
                        }
                        Console.WriteLine($"[Resident language] budget={budget}, switch={switchLanguage}, result={result}");
                        if (budget == 0)
                        {
                            Require(result, "Prepare all translations");
                            Require(preparation.Release(), "Release");
                        }
                        else
                        {
                            if (result != VnResult.VN_PACK_RESIDENT_CAPACITY)
                                throw new InvalidOperationException("Expected cumulative budget failure.");
                            if (switchLanguage && session.Language.Length != 0)
                                throw new InvalidOperationException("Failed switch changed the active language.");
                            foreach (string name in names)
                            {
                                var resolved = Actions.Resolve(name, session.ProjectId, out var action);
                                if (switchLanguage)
                                {
                                    Require(resolved, "Original still resolved");
                                    Require(Actions.Post(in action, Emitter.None, 0, out var firing), "Post original");
                                    if (!firing.IsValid) throw new InvalidOperationException("Missing firing.");
                                }
                                else if (resolved != VnResult.VN_UNKNOWN_ID)
                                    throw new InvalidOperationException("Rejected batch published " + name);
                            }
                            if (switchLanguage) Require(preparation.Release(), "Release originals");
                            foreach (string name in names)
                            {
                                Require(session.PrepareActions(new[] { name }, session.ProjectId, out var single), "Prepare one");
                                if (switchLanguage) Require(session.SetLanguageAsync(language).GetAwaiter().GetResult(), "Switch one");
                                Require(single.Release(), "Release one");
                            }
                        }
                        Require(session.Shutdown(), "Shutdown");
                    }
                }
                return true;
            }
            catch (Exception error)
            {
                Console.WriteLine("[Resident language] FAIL: " + error);
                return false;
            }
        }

        public static bool RunResidentMember(string root)
        {
            const uint oldLimit = 16 * 1024 * 1024;
            try
            {
                var members = Directory.GetFiles(root, "*.hrp").Select(path =>
                {
                    using var header = new BinaryReader(File.OpenRead(path));
                    uint version = header.ReadUInt32();
                    if (version != 0x32505248 && version != 0x31505248)
                        throw new InvalidDataException("Expected a Windows resident Pack: " + path);
                    return (name: Path.GetFileNameWithoutExtension(path), bytes: header.ReadUInt32(), json: header.ReadUInt32());
                }).ToArray();
                var init = members.Single(m => m.name == "Init");
                var names = members.Where(m => m.name != "Init").Select(m => m.name).ToArray();
                if (!members.Any(m => m.bytes > oldLimit) || !members.Any(m => m.json > oldLimit))
                    throw new InvalidDataException("Supply a Pack and metadata larger than 16 MiB.");
                // A file-sized budget covers the stored file, but not its expanded arrays.
                // Reject it from the header without allocating a full-size read buffer.
                foreach (var member in members.Where(m => m.bytes > oldLimit || m.json > oldLimit))
                {
                    long stored = new FileInfo(Path.Combine(root, member.name + ".hrp")).Length;
                    if (stored <= 1024 * 1024) continue;
                    var source = new PackDirectorySource(root, null, true, (ulong)stored);
                    string name = member.name == "Init" ? null : member.name;
                    long before = GC.GetAllocatedBytesForCurrentThread();
                    var result = source.Capture(name, default, null, out _);
                    long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
                    Console.WriteLine($"[Resident member] {member.name}, {stored} stored bytes, {allocated} allocated on rejection.");
                    if (result != VnResult.VN_PACK_RESIDENT_CAPACITY || allocated >= stored)
                        throw new InvalidOperationException("Budget rejection allocated the full resident file.");
                }
                foreach (string language in new[] { "MissingTranslation", string.Empty })
                {
                    foreach (ulong budget in new[] { 0UL, (ulong)oldLimit })
                    {
                        var config = HarkenEngine.DefaultConfig();
                        config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                        config.maxPackDecodeStagingBytes = budget;
                        PackBootstrapSession session;
                        VnResult result;
                        if (language.Length == 0)
                            result = PackBootstrap.StartDirectory(root, language, ref config, out session);
                        else
                        {
                            var startup = PackBootstrap.StartAsync(PackSource.FromDirectory(root, language), config).GetAwaiter().GetResult();
                            session = startup.Session;
                            result = startup.Result;
                        }
                        Console.WriteLine($"[Resident member] language='{language}' budget={budget} StartDirectory={result}");
                        if (budget != 0 && (init.bytes > budget || init.json > budget))
                        {
                            if (result != VnResult.VN_PACK_RESIDENT_CAPACITY || session != null)
                                throw new InvalidOperationException("Oversized Init should exceed the explicit budget.");
                            continue;
                        }
                        Require(result, "StartDirectory");
                        using (session)
                        {
                            ActionPreparation preparation;
                            if (language.Length == 0)
                                result = session.PrepareActions(names, session.ProjectId, out preparation);
                            else
                            {
                                var ready = session.PrepareActionsAsync(names, session.ProjectId).GetAwaiter().GetResult();
                                preparation = ready.Preparation;
                                result = ready.Result;
                            }
                            Console.WriteLine($"[Resident member] PrepareActions={result}");
                            if (budget == 0)
                            {
                                Require(result, "Prepare");
                                foreach (string name in names)
                                {
                                    Require(Actions.Resolve(name, session.ProjectId, out var action), "Resolve " + name);
                                    Require(Actions.Post(in action, Emitter.None, 0, out var firing), "Post " + name);
                                    if (!firing.IsValid) throw new InvalidOperationException("Missing firing: " + name);
                                }
                                Require(preparation.Release(), "Release");
                            }
                            else
                            {
                                if (result != VnResult.VN_PACK_RESIDENT_CAPACITY)
                                    throw new InvalidOperationException("Oversized batch should exceed the explicit budget.");
                                foreach (string name in names)
                                    if (Actions.Resolve(name, session.ProjectId, out _) != VnResult.VN_UNKNOWN_ID)
                                        throw new InvalidOperationException("Rejected batch published " + name);
                            }
                            Require(session.Shutdown(), "Shutdown");
                        }
                    }
                }
                return true;
            }
            catch (Exception error)
            {
                Console.WriteLine("[Resident member] FAIL: " + error);
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
