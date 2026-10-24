using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Harken;
using Harken.Unity;

namespace Harken.Test
{
    // Standalone runner for the C# binding's machine gates:
    //   1. ABI fidelity  — pure managed (Marshal.SizeOf/OffsetOf vs the pinned
    //      model values); ALWAYS runs, needs no native library.
    //   2. Coordinate conversion — pure managed host-axis and unit mappings,
    //      including a Unity left/right pan-sign regression.
    //   3. Zero-GC hot path — drives Play / SetValue / SetVoice* / PostAction
    //      (by id) / Update and asserts the managed-allocation delta is zero;
    //      needs the native HarkenCore library. If it is not loadable the gate
    //      is reported SKIPPED (never silently green), per the honesty rule.
    //
    // Exit code: 0 when the ABI and coordinate gates pass and the zero-GC gate
    // is PASS or, in optional mode, SKIP. --require-native makes SKIP a failure.
    internal static class Program
    {
        private static int Main(string[] args)
        {
            if (args.Length == 1 && (args[0] == "--attenuation-only" || args[0] == "--attenuation-old-native"))
            {
                if (string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("HARKEN_NATIVE_DIR")))
                {
                    Console.Error.WriteLine(args[0] + " requires an explicit HARKEN_NATIVE_DIR.");
                    return 2;
                }
                InstallNativeResolver(true);
                return RunAbiFidelity() && RunAttenuationAbi(args[0] == "--attenuation-old-native") ? 0 : 1;
            }
            if (args.Length == 1 && args[0] == "--variables-only")
            {
                InstallNativeResolver(true);
                if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("HARKEN_VARIABLE_INIT")))
                {
                    Console.Error.WriteLine("--variables-only requires HARKEN_VARIABLE_INIT.");
                    return 2;
                }
                return RunAbiFidelity() && VariableSdkContract.Run() ? 0 : 1;
            }
            if (args.Length == 1 && args[0] == "--state-only-unavailable")
            {
                InstallNativeResolver(true);
                VnConfig config = HarkenEngine.DefaultConfig();
                bool refused = HarkenEngine.Init(config) == VnResult.VN_CAPABILITY_UNAVAILABLE &&
                    Emitter.CreateStateOnly(out Emitter emitter) == VnResult.VN_CAPABILITY_UNAVAILABLE && !emitter.IsValid;
                Console.WriteLine("Old native StateOnly compatibility: " + (refused ? "PASS" : "FAIL"));
                return refused ? 0 : 1;
            }
            bool requireNative = false;
            string currentWorldPath = null;
            string packRoot = null;
            string companionRoot = null;
            for (int i = 0; i < args.Length; i++)
            {
                string arg = args[i];
                if (arg == "--require-native")
                {
                    requireNative = true;
                    continue;
                }
                if (arg == "--current-world" && i + 1 < args.Length)
                {
                    currentWorldPath = args[++i];
                    continue;
                }
                if (arg == "--pack-root" && i + 1 < args.Length)
                {
                    packRoot = args[++i];
                    continue;
                }
                if (arg == "--companion-root" && i + 1 < args.Length)
                {
                    companionRoot = args[++i];
                    continue;
                }
                Console.Error.WriteLine("Unknown argument: " + arg);
                Console.Error.WriteLine("Usage: dotnet run --project HarkenSDK/Test/ContractTest -- [--require-native] [--current-world path] [--pack-root path] [--companion-root path]");
                return 2;
            }

            InstallNativeResolver(requireNative);
            if (companionRoot != null) return RepositoryPublicationContract.Run(companionRoot) ? 0 : 1;

            Console.WriteLine("== Harken C# binding gates ==");
            Console.WriteLine("Native mode : " + (requireNative ? "REQUIRED" : "OPTIONAL"));
            bool abiOk = RunAbiFidelity();
            bool variableSdkOk = VariableSdkContract.Run();
            bool coordinatesOk = RunCoordinateConversions();
            bool bootstrapOk = RunPackBootstrapContract();
            int namePost = RunNamePostAndProvider();
            int nativeBootstrap = RunNativePackBootstrap(packRoot);
            int zeroGc = RunZeroGc(currentWorldPath);   // 0 PASS, 1 FAIL, 2 SKIP
            int remoteNative = RemoteCommunicationContract.RunNative();
            bool repositoryNativeOk = !requireNative || RepositoryPublicationContract.Run();
            bool companionNativeOk = !requireNative || CompanionDirectoryContract.Run();
            companionNativeOk &= !requireNative || CompanionSourceContract.Run();

            Console.WriteLine();
            Console.WriteLine("ABI fidelity : " + (abiOk ? "PASS" : "FAIL"));
            Console.WriteLine("Coordinates  : " + (coordinatesOk ? "PASS" : "FAIL"));
            Console.WriteLine("Pack bootstrap: " + (bootstrapOk ? "PASS" : "FAIL"));
            Console.WriteLine("Name post     : " +
                (namePost == 0 ? "PASS" : namePost == 2 ? "SKIP" : "FAIL"));
            Console.WriteLine("Pack native   : " +
                (nativeBootstrap == 0 ? "PASS" :
                 nativeBootstrap == 2 ? "SKIP" :
                 nativeBootstrap == 3 ? "NOT REQUESTED" : "FAIL"));
            Console.WriteLine("Zero-GC hot  : " + (zeroGc == 0 ? "PASS" : zeroGc == 2 ? "SKIP" : "FAIL"));
            Console.WriteLine("Remote native: " + (remoteNative == 0 ? "PASS" : remoteNative == 2 ? "SKIP" : "FAIL"));
            if (requireNative)
            {
                Console.WriteLine("Repository publications: " + (repositoryNativeOk ? "PASS" : "FAIL"));
                bool nativeOk = companionNativeOk && repositoryNativeOk && namePost == 0 && zeroGc == 0 && remoteNative == 0;
                Console.WriteLine("Required native validation: " + (nativeOk ? "PASS" : "FAIL"));
                if (namePost == 2 || zeroGc == 2 || remoteNative == 2)
                {
                    Console.WriteLine("Required native validation failed: a native gate was skipped.");
                }
            }

            bool ok = companionNativeOk && repositoryNativeOk && variableSdkOk && abiOk && coordinatesOk && bootstrapOk &&
                (nativeBootstrap == 0 || nativeBootstrap == 3 ||
                 (!requireNative && nativeBootstrap == 2)) &&
                (namePost == 0 || (!requireNative && namePost == 2)) &&
                (zeroGc == 0 || (!requireNative && zeroGc == 2)) &&
                (remoteNative == 0 || (!requireNative && remoteNative == 2));
            return ok ? 0 : 1;
        }

        // ---- Gate 1: ABI fidelity (pure managed) ----------------------------
        private static bool RunAbiFidelity()
        {
            List<string> failures = HarkenAbi.CheckLayout();
            CheckCallbackAbi(failures);
            var id = VnObjectId.Parse("cacc73dc-2e34-4d29-894f-3ed9db63e6fa");
            if (id.high != 0xcacc73dc2e344d29UL || id.low != 0x894f3ed9db63e6faUL ||
                id.ToString() != "cacc73dc-2e34-4d29-894f-3ed9db63e6fa" ||
                VnObjectId.FromGuid(id.ToGuid()) != id || VnObjectId.TryParse("17", out _))
                failures.Add("UUID byte order and canonical text must agree with the C ABI");
            var other = new VnObjectId(id.high + 1, id.low);
            if (new HashSet<VnObjectId> { id, other }.Count != 2 || id.CompareTo(other) >= 0)
                failures.Add("UUID identity must retain both halves");
            if (failures.Count == 0)
            {
                Console.WriteLine("[ABI] all mirrored struct sizes/offsets match the pinned model.");
                Console.WriteLine("[ABI] VnPackByteProviderFn returns VnResult; VnPackByteReleaseFn span is VnPackByteSpan.");
                return true;
            }
            Console.WriteLine("[ABI] " + failures.Count + " mismatch(es):");
            foreach (string f in failures)
            {
                Console.WriteLine("      " + f);
            }
            return false;
        }

        private static unsafe bool RunAttenuationAbi(bool oldNative)
        {
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            VnResult initialized = HarkenEngine.Init(config);
            Console.WriteLine("[Attenuation ABI] Init=" + initialized);
            if (initialized != VnResult.VN_OK) return false;
            try
            {
                bool ok = true;
                var existing = new VnObjectId(0, 0xe30001);
                var unused = new VnObjectId(0, 0xe30002);
                var desc = new VnAttenuationDesc
                {
                    structSize = 608,
                    minDistance = 2,
                    maxDistance = 102,
                    airAbsorptionNearHz = 1.0e9f,
                    airAbsorptionFarHz = 1.0e9f
                };
                ok &= AttenuationResult("Register prefix608", HarkenNative.Vn_RegisterAttenuation(existing, in desc), VnResult.VN_OK);
                desc.structSize = 612;
                desc.proximityFrequency = 400;
                ok &= AttenuationResult("Replace prefix612", HarkenNative.Vn_ReplaceAttenuation(existing, in desc), VnResult.VN_OK);

                desc.structSize = (uint)Marshal.SizeOf<VnAttenuationDesc>();
                VnResult expected = oldNative ? VnResult.VN_INVALID_ARG : VnResult.VN_OK;
                ok &= AttenuationResult("Register current HPF off size=" + desc.structSize,
                    HarkenNative.Vn_RegisterAttenuation(unused, in desc), expected);
                ok &= AttenuationResult("Replace current HPF off",
                    HarkenNative.Vn_ReplaceAttenuation(existing, in desc), expected);
                desc.highpassCurve.enabled = 1;
                desc.highpassCurve.pointCount = 1;
                desc.highpassCurve.pointsRaw[1] = BitConverter.SingleToUInt32Bits(1000);
                ok &= AttenuationResult("Replace current HPF 1000Hz",
                    HarkenNative.Vn_ReplaceAttenuation(existing, in desc), expected);
                ok &= AttenuationResult("Register current HPF 1000Hz",
                    HarkenNative.Vn_RegisterAttenuation(new VnObjectId(0, 0xe30004), in desc), expected);
                if (!oldNative)
                {
                    desc.highpassCurve.pointsRaw[1] = 0;
                    ok &= AttenuationResult("Replace current HPF 0Hz",
                        HarkenNative.Vn_ReplaceAttenuation(existing, in desc), VnResult.VN_OK);
                    desc.highpassCurve.pointsRaw[1] = BitConverter.SingleToUInt32Bits(-1);
                    ok &= AttenuationResult("Replace negative HPF",
                        HarkenNative.Vn_ReplaceAttenuation(existing, in desc), VnResult.VN_INVALID_ARG);
                    desc.highpassCurve.pointsRaw[1] = 0;
                    desc.highpassCurve.enabled = 2;
                    ok &= AttenuationResult("Replace invalid HPF enabled",
                        HarkenNative.Vn_ReplaceAttenuation(existing, in desc), VnResult.VN_INVALID_ARG);
                }
                Console.WriteLine("[Attenuation ABI] " + (oldNative ? "old native rejects current descriptor: " : "current native: ") + (ok ? "PASS" : "FAIL"));
                return ok;
            }
            finally { HarkenEngine.Term(); }
        }

        private static bool AttenuationResult(string operation, VnResult actual, VnResult expected)
        {
            Console.WriteLine("[Attenuation ABI] " + operation + " actual=" + actual + " expected=" + expected);
            return actual == expected;
        }

        static void CheckCallbackAbi(List<string> failures)
        {
            System.Reflection.MethodInfo provide =
                typeof(VnPackByteProviderFn).GetMethod("Invoke");
            if (provide == null || provide.ReturnType != typeof(VnResult))
            {
                failures.Add("VnPackByteProviderFn must return VnResult");
            }
            System.Reflection.MethodInfo release =
                typeof(VnPackByteReleaseFn).GetMethod("Invoke");
            System.Reflection.ParameterInfo[] releaseParams =
                release == null ? null : release.GetParameters();
            if (releaseParams == null || releaseParams.Length == 0 ||
                releaseParams[releaseParams.Length - 1].ParameterType !=
                    typeof(VnPackByteSpan))
            {
                failures.Add("VnPackByteReleaseFn last parameter must be VnPackByteSpan");
            }
        }

        static int RunNamePostAndProvider()
        {
            try
            {
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                VnResult init = HarkenEngine.Init(config);
                if (init != VnResult.VN_OK)
                {
                    Console.WriteLine("[NamePost] SKIP: Vn_Init returned " + init + ".");
                    return 2;
                }
            }
            catch (DllNotFoundException)
            {
                Console.WriteLine("[NamePost] SKIP: native 'HarkenCore' library not found.");
                return 2;
            }
            catch (BadImageFormatException)
            {
                Console.WriteLine("[NamePost] SKIP: native library architecture mismatch.");
                return 2;
            }

            try
            {
                bool ok = true;
                VnResult result = Actions.Post(
                    "Play_Fire", Emitter.None, out ActionFiring firing);
                string describe = Actions.Describe(result, "Play_Fire");
                if (result != VnResult.VN_CAPABILITY_UNAVAILABLE ||
                    firing.IsValid ||
                    describe.IndexOf("capability", StringComparison.OrdinalIgnoreCase) < 0)
                {
                    Console.WriteLine(
                        "[NamePost] FAIL: no-kind25 Post returned " + result +
                        " firing=" + firing.Handle + " describe=" + describe + ".");
                    ok = false;
                }

                result = Actions.Post("", Emitter.None, out firing);
                if (result != VnResult.VN_INVALID_ARG || firing.IsValid)
                {
                    Console.WriteLine(
                        "[NamePost] FAIL: empty name returned " + result + ".");
                    ok = false;
                }
                result = Actions.Post("Play/Fire", Emitter.None, out firing);
                if (result != VnResult.VN_INVALID_ARG || firing.IsValid)
                {
                    Console.WriteLine(
                        "[NamePost] FAIL: illegal name returned " + result + ".");
                    ok = false;
                }

                result = Actions.Post("Play_Fire\0Other", Emitter.None, out firing);
                if (result != VnResult.VN_INVALID_ARG || firing.IsValid)
                {
                    Console.WriteLine("[NamePost] FAIL: embedded NUL Post returned " + result);
                    ok = false;
                }
                result = Actions.Resolve("Play_Fire\0Other", out ActionResolved invalidResolved);
                if (result != VnResult.VN_INVALID_ARG || !invalidResolved.ActionId.IsNull)
                {
                    Console.WriteLine("[NamePost] FAIL: embedded NUL Resolve returned " + result);
                    ok = false;
                }
                var invalidPin = new ActionReference {
                    ExpectedName = "Play_Fire\0Other", ProjectGuid = new byte[16],
                    ActionId = new VnObjectId(0x3da91a64b9474560UL, 2002)
                };
                result = Actions.Post(in invalidPin, Emitter.None, 0, out firing);
                if (result != VnResult.VN_INVALID_ARG || firing.IsValid)
                {
                    Console.WriteLine("[NamePost] FAIL: embedded NUL Pinned returned " + result);
                    ok = false;
                }

                result = PackByteProvider.LoadPack(
                    22, FailProvider, null, out Pack pack);
                if (result != VnResult.VN_PACK_PROVIDER_FAILED || pack.IsValid ||
                    !PackByteProvider.LastProvideLeftSpanZero ||
                    PackByteProvider.OutstandingPinCount != 0)
                {
                    Console.WriteLine(
                        "[NamePost] FAIL: failing provider returned " + result +
                        " spanZero=" + PackByteProvider.LastProvideLeftSpanZero +
                        " pins=" + PackByteProvider.OutstandingPinCount + ".");
                    ok = false;
                }

                result = PackByteProvider.LoadPack(
                    22, GarbageProvider, null, out pack);
                if ((result != VnResult.VN_BAD_PACK &&
                     result != VnResult.VN_PACK_INCOMPATIBLE) ||
                    pack.IsValid ||
                    PackByteProvider.LastReleaseCalls < 1 ||
                    PackByteProvider.OutstandingPinCount != 0)
                {
                    Console.WriteLine(
                        "[NamePost] FAIL: garbage provider returned " + result +
                        " releases=" + PackByteProvider.LastReleaseCalls +
                        " pins=" + PackByteProvider.OutstandingPinCount + ".");
                    ok = false;
                }

                Console.WriteLine(
                    ok
                        ? "[NamePost] name Post, Describe, and byte-provider PASS."
                        : "[NamePost] name Post or byte-provider FAIL.");
                return ok ? 0 : 1;
            }
            finally
            {
                HarkenEngine.Term();
            }
        }

        static VnResult FailProvider(
            ulong packStableId, string relativeLocatorUtf8, uint variantKey,
            ulong expectedContentHash, out byte[] payload, object state)
        {
            payload = null;
            return VnResult.VN_PACK_PROVIDER_FAILED;
        }

        static VnResult ValidProvider(
            ulong packStableId, string relativeLocatorUtf8, uint variantKey,
            ulong expectedContentHash, out byte[] payload, object state)
        {
            payload = (byte[])state;
            return VnResult.VN_OK;
        }

        static VnResult GarbageProvider(
            ulong packStableId, string relativeLocatorUtf8, uint variantKey,
            ulong expectedContentHash, out byte[] payload, object state)
        {
            payload = new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 };
            return VnResult.VN_OK;
        }

        // ---- Gate 2: host coordinate conversion (pure managed) --------------
        private static bool RunCoordinateConversions()
        {
            bool ok = true;
            ok &= CheckVector(
                "Unity forward",
                UnityCoordinates.ToHarken(0.0f, 0.0f, 1.0f),
                0.0f, 0.0f, -1.0f);
            ok &= CheckVector(
                "Unity right",
                UnityCoordinates.ToHarken(1.0f, 0.0f, 0.0f),
                1.0f, 0.0f, 0.0f);

            VnVec3 unityForward = UnityCoordinates.ToHarken(0.0f, 0.0f, 1.0f);
            VnVec3 unityUp = UnityCoordinates.ToHarken(0.0f, 1.0f, 0.0f);
            VnVec3 harkenRight = Cross(unityForward, unityUp);
            ok &= CheckVector("Unity listener right", harkenRight, 1.0f, 0.0f, 0.0f);
            float leftPan = Dot(UnityCoordinates.ToHarken(-1.0f, 0.0f, 0.0f), harkenRight);
            if (leftPan >= 0.0f)
            {
                Console.WriteLine("[Coordinates] Unity left pan FAIL: expected a negative pan value.");
                ok = false;
            }
            if (UnityCoordinates.IsValidDirection(0.0f, 0.0f, 0.0f) ||
                UnityCoordinates.IsValidDirection(float.NaN, 0.0f, 1.0f) ||
                !UnityCoordinates.IsValidDirection(float.MaxValue, float.MaxValue, float.MaxValue))
            {
                Console.WriteLine("[Coordinates] Unity direction validation FAIL.");
                ok = false;
            }
            if (UnityCoordinates.IsFinite(0.0f, float.PositiveInfinity, 0.0f))
            {
                Console.WriteLine("[Coordinates] Unity position validation FAIL.");
                ok = false;
            }

            if (ok)
            {
                Console.WriteLine("[Coordinates] Unity mapping matches the Harken spatial contract.");
            }
            return ok;
        }

        private static bool RunPackBootstrapContract()
        {
            VnInitPackInspectReport native = default;
            native.result = (int)VnResult.VN_OK;
            native.catalogEntries = 50000;
            native.catalogBytes = 15200088;
            native.recommendedMaxCatalogEntries = 50000;
            native.recommendedMaxActions = 1024;
            native.recommendedCatalogBytes = 15200088;
            native.recommendedResidentDefinitionBytes = 8UL << 20;
            InitPackInspection inspection = new InitPackInspection(native);

            VnConfig config = HarkenEngine.DefaultConfig();
            VnResult result = PackBootstrap.ApplyRecommendations(
                ref config, inspection, 3);
            bool ok = result == VnResult.VN_OK &&
                config.maxCatalogEntries == 50000 &&
                config.maxActions == 1024 &&
                config.maxCatalogBytes == 15200088 &&
                config.maxResidentDefinitionBytes == (8UL << 20);

            VnConfig catalogCountLow = HarkenEngine.DefaultConfig();
            catalogCountLow.maxCatalogEntries = 49999;
            ok &= PackBootstrap.ApplyRecommendations(
                ref catalogCountLow, inspection, 3) ==
                VnResult.VN_PACK_CATALOG_CAPACITY;

            VnConfig catalogBytesLow = HarkenEngine.DefaultConfig();
            catalogBytesLow.maxCatalogBytes = 15200087;
            ok &= PackBootstrap.ApplyRecommendations(
                ref catalogBytesLow, inspection, 3) ==
                VnResult.VN_PACK_CATALOG_CAPACITY;

            VnConfig residentBytesLow = HarkenEngine.DefaultConfig();
            residentBytesLow.maxResidentDefinitionBytes = (8UL << 20) - 1;
            ok &= PackBootstrap.ApplyRecommendations(
                ref residentBytesLow, inspection, 3) ==
                VnResult.VN_PACK_RESIDENT_CAPACITY;

            VnConfig actionsLow = HarkenEngine.DefaultConfig();
            actionsLow.maxActions = 1023;
            ok &= PackBootstrap.ApplyRecommendations(
                ref actionsLow, inspection, 3) ==
                VnResult.VN_PACK_RESIDENT_CAPACITY;

            VnConfig packCountDefault = HarkenEngine.DefaultConfig();
            ok &= PackBootstrap.ApplyRecommendations(
                ref packCountDefault, inspection, 255) == VnResult.VN_OK;
            VnConfig packCountLow = HarkenEngine.DefaultConfig();
            ok &= PackBootstrap.ApplyRecommendations(
                ref packCountLow, inspection, 256) ==
                VnResult.VN_INVALID_CONFIG;

            if (ok)
            {
                Console.WriteLine(
                    "[PackBootstrap] recommendation and exact-budget contract PASS.");
            }
            else
            {
                Console.WriteLine(
                    "[PackBootstrap] recommendation or exact-budget contract FAIL.");
            }
            return ok;
        }

        private static int RunNativePackBootstrap(string packRoot)
        {
            if (string.IsNullOrEmpty(packRoot))
            {
                return 3;
            }
            string fullRoot = Path.GetFullPath(packRoot);
            string initPath = Path.Combine(fullRoot, "Init.pack");
            if (!File.Exists(initPath) ||
                !File.Exists(Path.Combine(fullRoot, "Content.pack")))
            {
                Console.WriteLine(
                    "[PackNative] FAIL: shared fixture is incomplete at " +
                    fullRoot + ".");
                return 1;
            }

            bool initialized = false;
            try
            {
                VnResult result = Packs.InspectInitPack(initPath, out InitPackInspection inspection);
                if (result != VnResult.VN_OK) return 1;
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                config.maxPacks = 4;
                config.maxActions = 64;
                config.maxResidentDefinitionBytes = 32UL << 20;
                if (PackBootstrap.ApplyRecommendations(ref config, inspection, 1) != VnResult.VN_OK ||
                    HarkenEngine.Init(config) != VnResult.VN_OK) return 1;
                initialized = true;
                var members = new[] {
                    new CanonicalPackMember(inspection.PackStableId, 2, provider: ValidProvider,
                        providerState: File.ReadAllBytes(initPath)),
                    new CanonicalPackMember(22, 3, provider: ValidProvider,
                        providerState: File.ReadAllBytes(Path.Combine(fullRoot, "Content.pack")))
                };
                int provides = PackByteProvider.LastProvideCalls;
                int releases = PackByteProvider.LastReleaseCalls;
                result = Packs.ActivateCanonicalPackSet(members, 1, out var report, out var rows);
                bool ok = result == VnResult.VN_OK && inspection.CatalogEntries == 1000 &&
                    rows.Length == 2 && Array.TrueForAll(rows, row => row.handle != 0) &&
                    PackByteProvider.LastProvideCalls - provides == 2 &&
                    PackByteProvider.LastReleaseCalls - releases == 2 && PackByteProvider.OutstandingPinCount == 0;
                if (!ok) return 1;
                result = Actions.Post(new VnObjectId(0x3da91a64b9474560UL, 0xAC710100UL),
                    Emitter.None, out ActionFiring firing);
                ok &= result == VnResult.VN_OK && firing.IsValid;
                ok &= Actions.Post(new VnObjectId(0x3da91a64b9474560UL, 0xAC710140UL),
                    Emitter.None, out _) == VnResult.VN_CONTENT_NOT_LOADED;
                ok &= Packs.UnloadCanonicalPackSet(new Pack(report.initHandle),
                    report.resultSetRevision, report.resultSetRevision + 1) == VnResult.VN_OK;
                Console.WriteLine(ok ? "[PackNative] shared Init/content fixture PASS."
                    : "[PackNative] shared Init/content fixture FAIL.");
                return ok ? 0 : 1;
            }
            catch (DllNotFoundException)
            {
                Console.WriteLine("[PackNative] SKIP: native 'HarkenCore' library not found.");
                return 2;
            }
            catch (BadImageFormatException)
            {
                Console.WriteLine("[PackNative] SKIP: native library architecture mismatch.");
                return 2;
            }
            finally
            {
                if (initialized) HarkenEngine.Term();
            }
        }

        private static VnVec3 Cross(VnVec3 a, VnVec3 b)
        {
            return new VnVec3
            {
                x = a.y * b.z - a.z * b.y,
                y = a.z * b.x - a.x * b.z,
                z = a.x * b.y - a.y * b.x
            };
        }

        private static float Dot(VnVec3 a, VnVec3 b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        private static bool CheckVector(string name, VnVec3 actual, float x, float y, float z)
        {
            const float epsilon = 0.000001f;
            bool ok =
                Math.Abs(actual.x - x) <= epsilon &&
                Math.Abs(actual.y - y) <= epsilon &&
                Math.Abs(actual.z - z) <= epsilon;
            if (!ok)
            {
                Console.WriteLine(
                    "[Coordinates] " + name + " FAIL: expected (" + x + ", " + y + ", " + z +
                    "), got (" + actual.x + ", " + actual.y + ", " + actual.z + ").");
            }
            return ok;
        }

        // ---- Gate 3: zero-GC hot path (needs the native library) ------------
        private static int RunZeroGc(string currentWorldPath)
        {
            VnObjectId actionId = new VnObjectId(0x3da91a64b9474560UL, 0xAC7101);
            VnObjectId varId = new VnObjectId(0x3da91a64b9474560UL, 0x5A1);

            try
            {
                VnConfig config = HarkenEngine.DefaultConfig();
                config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
                VnResult init = HarkenEngine.Init(config);
                if (init != VnResult.VN_OK)
                {
                    Console.WriteLine("[GC] SKIP: Vn_Init returned " + init + " (cannot exercise the hot path).");
                    return 2;
                }
            }
            catch (DllNotFoundException)
            {
                Console.WriteLine("[GC] SKIP: native 'HarkenCore' library not found. Build HARKEN_BUILD_SHARED");
                Console.WriteLine("      and set HARKEN_NATIVE_DIR to the directory holding HarkenCore.dll,");
                Console.WriteLine("      or place it next to the runner. ABI gate above is independent of this.");
                return 2;
            }
            catch (BadImageFormatException)
            {
                Console.WriteLine("[GC] SKIP: native 'HarkenCore' library architecture mismatch (need x64).");
                return 2;
            }

            try
            {
                if (!CheckCurrentWorldContract(currentWorldPath))
                {
                    return 1;
                }
                if (!CheckActionFiringStateContract())
                {
                    return 1;
                }
                return DriveHotPath(actionId, varId);
            }
            finally
            {
                HarkenEngine.Term();
            }
        }

        private static bool CheckCurrentWorldContract(string artifactPath)
        {
            VnResult result = CurrentWorld.Load(
                null, out CurrentWorldLoadReport report);
            if (result != VnResult.VN_INVALID_ARG ||
                report.Status !=
                    VnCurrentWorldLoadStatus.VN_CURRENT_WORLD_LOAD_INVALID_ARGUMENT ||
                report.Result != VnResult.VN_INVALID_ARG)
            {
                Console.WriteLine(
                    "[CurrentWorld] FAIL: null path returned " + result +
                    " with status " + report.Status +
                    " and report result " + report.Result + ".");
                return false;
            }
            Console.WriteLine("[CurrentWorld] null path report contract PASS.");
            if (string.IsNullOrEmpty(artifactPath))
            {
                return true;
            }

            result = CurrentWorld.Load(
                artifactPath, out report);
            const ulong actionDefinitions = 1UL << 9;
            if (result != VnResult.VN_OK ||
                report.Status != VnCurrentWorldLoadStatus.VN_CURRENT_WORLD_LOAD_OK ||
                report.Result != VnResult.VN_OK ||
                report.ResultGeneration == 0 ||
                report.RuntimeSemanticFingerprint == 0 ||
                (report.CapabilityMask & actionDefinitions) == 0)
            {
                Console.WriteLine(
                    "[CurrentWorld] FAIL: Action artifact returned " + result +
                    " with status " + report.Status +
                    ", report result " + report.Result +
                    ", generation " + report.ResultGeneration +
                    ", capability mask 0x" + report.CapabilityMask.ToString("X") + ".");
                return false;
            }
            Console.WriteLine("[CurrentWorld] Studio-authored Action artifact load PASS.");
            return true;
        }

        private static bool CheckActionFiringStateContract()
        {
            ActionFiring stale = default;
            VnResult result = stale.Query(out VnActionFiringInfo info);
            if (result != VnResult.VN_STALE_HANDLE)
            {
                Console.WriteLine(
                    "[Action] FAIL: stale firing returned " + result +
                    " with state " + info.state + ".");
                return false;
            }
            Console.WriteLine("[Action] stale firing state contract PASS.");
            return true;
        }

        private static int DriveHotPath(VnObjectId actionId, VnObjectId varId)
        {
            Voice staleVoice = default;
            var pos = new VnVec3 { x = 1.0f, y = 0.0f, z = 0.0f };

            // --- warmup: JIT every method and its marshaling stub once ---
            Values.SetValue(varId, 1.0f);
            staleVoice.SetVolume(-3.0f, 0);
            staleVoice.SetPitch(1.0f);
            staleVoice.SetPosition(pos);
            Actions.Post(actionId, Emitter.None, out _);
            HarkenEngine.Update();

            _ = GC.GetAllocatedBytesForCurrentThread();
            long controlBefore = GC.GetAllocatedBytesForCurrentThread();
            var allocationControl = new byte[1024];
            long controlDelta = GC.GetAllocatedBytesForCurrentThread() - controlBefore;
            GC.KeepAlive(allocationControl);
            if (controlDelta <= 0)
            {
                Console.WriteLine("[GC] FAIL: calling-thread counter missed the positive allocation control.");
                return 1;
            }
            Console.WriteLine("[GC] Calling-thread allocation control detected " + controlDelta + " managed bytes.");

            // --- measured region: the per-frame hot path ---
            const int iterations = 4096;
            long before = GC.GetAllocatedBytesForCurrentThread();
            for (int i = 0; i < iterations; i++)
            {
                Values.SetValue(varId, (float)i);
                staleVoice.SetVolume(-3.0f, 0);
                staleVoice.SetPitch(1.0f);
                staleVoice.SetPosition(pos);
                Actions.Post(actionId, Emitter.None, out _);
                HarkenEngine.Update();
            }
            long delta = GC.GetAllocatedBytesForCurrentThread() - before;

            Console.WriteLine("[GC] " + iterations + " hot-path iterations allocated " + delta + " managed bytes on the calling thread.");
            if (delta == 0)
            {
                return 0;
            }
            Console.WriteLine("[GC] FAIL: hot path allocated managed memory (expected 0).");
            return 1;
        }

        // Harness-only helper (not part of the shipped binding): points the
        // DllImport at the dev build's HarkenCore so `dotnet run` can find it.
        // The binding itself uses the compile-time-baked name and never does
        // runtime resolution.
        private static void InstallNativeResolver(bool requireNative)
        {
            NativeLibrary.SetDllImportResolver(typeof(HarkenNative).Assembly, (name, asm, path) =>
            {
                if (name != HarkenNative.LibraryName)
                {
                    return IntPtr.Zero;
                }
                string explicitDir = Environment.GetEnvironmentVariable("HARKEN_NATIVE_DIR");
                bool explicitDirIsRequired = requireNative && !string.IsNullOrWhiteSpace(explicitDir);
                foreach (string dir in CandidateDirs(explicitDir, explicitDirIsRequired))
                {
                    if (string.IsNullOrEmpty(dir))
                    {
                        continue;
                    }
                    foreach (string file in new[] { "HarkenCore.dll", "libHarkenCore.so", "libHarkenCore.dylib" })
                    {
                        string candidate = Path.Combine(dir, file);
                        if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out IntPtr h))
                        {
                            return h;
                        }
                    }
                }
                if (explicitDirIsRequired)
                {
                    throw new DllNotFoundException(
                        "No loadable HarkenCore library found in required HARKEN_NATIVE_DIR: " + explicitDir);
                }
                return IntPtr.Zero; // fall back to the default OS search
            });
        }

        private static IEnumerable<string> CandidateDirs(string explicitDir, bool explicitDirIsRequired)
        {
            yield return explicitDir;
            if (explicitDirIsRequired)
            {
                yield break;
            }
            yield return AppContext.BaseDirectory;
            yield return Directory.GetCurrentDirectory();
            // Common repo-relative dev build outputs (best-effort).
            string root = FindRepoRoot();
            if (root != null)
            {
                foreach (string cfg in new[] { "Debug", "Release" })
                {
                    yield return Path.Combine(root, "build-nc", "bin", cfg);
                    yield return Path.Combine(root, "build-vs", "bin", cfg);
                }
            }
        }

        private static string FindRepoRoot()
        {
            string dir = AppContext.BaseDirectory;
            for (int i = 0; i < 8 && dir != null; i++)
            {
                if (File.Exists(Path.Combine(dir, "CMakeLists.txt")) &&
                    Directory.Exists(Path.Combine(dir, "HarkenEngine")))
                {
                    return dir;
                }
                dir = Path.GetDirectoryName(dir);
            }
            return null;
        }
    }
}
