using System;
using System.Reflection;
using System.Text;
using System.Globalization;
using System.IO;
using System.Diagnostics;
using System.Threading;
using Harken;
using Harken.Generated;

namespace Harken.Test
{
    internal static class VariableSdkContract
    {
        public static bool Run()
        {
            try
            {
                string catalog = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "VariableCatalog.json"));
                Require(VariableCatalogEmitter.EmitCSharpFromJson(catalog, "contract", out string generated, out _));
                Check(generated.Contains("ExecutionCatalogFingerprint = " +
                    HarkenVariables.ExecutionCatalogFingerprint.ToString(CultureInfo.InvariantCulture) + "UL;"),
                    "managed and tooling execution fingerprints match");
                Check(generated.Contains("CatalogFingerprint = " +
                    HarkenVariables.CatalogFingerprint.ToString(CultureInfo.InvariantCulture) + "UL;"),
                    "managed and tooling display fingerprints match");
                Check(generated.Contains("VehicleRPM = new VnObjectId(0x" +
                    HarkenVariables.VehicleRPM.high.ToString("x16", CultureInfo.InvariantCulture) + "UL, 0x" +
                    HarkenVariables.VehicleRPM.low.ToString("x16", CultureInfo.InvariantCulture) + "UL);"),
                    "variable generation preserves both GUID halves");
                string numericId = catalog.Replace("\"" + HarkenVariables.VehicleRPM + "\"", "7");
                Check(VariableCatalogEmitter.EmitCSharpFromJson(numericId, "contract", out _, out _) ==
                    VnResult.VN_BAD_PACK, "numeric variable identity is refused");
                string numericReplacement = catalog.Replace("\"00000000-0000-0000-0000-000000000000\"", "7");
                Check(VariableCatalogEmitter.EmitCSharpFromJson(numericReplacement, "contract", out _, out _) ==
                    VnResult.VN_BAD_PACK, "numeric replacement identity is refused");
                var typed = HarkenVariableId.FromGenerated("VehicleRPM", HarkenVariables.VehicleRPM);
                var labelId = new VnObjectId(0, 721);
                string labels = "[{\"anchor\":0,\"name\":\"class\",\"labelId\":\"00000000-0000-4000-8000-000000000001\"}," +
                    "{\"anchor\":1,\"name\":\"A-A\",\"labelId\":\"00000000-0000-4000-8000-000000000002\"}]";
                string named = catalog.Replace("\"labels\": []", "\"labels\": " + labels);
                Require(VariableCatalogEmitter.EmitCSharpFromJson(named, "labels", out string namedGenerated, out _));
                Check(namedGenerated.Contains("Label_VehicleRPM_class_00000000000040008000000000000001"),
                    "Smooth labels generate stable identities with safe identifiers");
                Check(namedGenerated.Contains("ExecutionCatalogFingerprint = " + HarkenVariables.ExecutionCatalogFingerprint + "UL;"),
                    "Smooth labels do not become discrete execution anchors");
                Check(VariableCatalogEmitter.EmitCSharpFromJson(named.Replace("A-A", "class"), "duplicates", out _, out _) ==
                    VnResult.VN_BAD_PACK, "duplicate label names refused");
                Check(Values.SetValue(typed, "class", default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "label write with explicit empty emitter never selects global");
                Check(Values.SetValue(typed, labelId, default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "label identity write with explicit empty emitter never selects global");
                Check(Values.SetValue(typed, "bad\0name") == VnResult.VN_INVALID_ARG,
                    "embedded null label names refused");
                Check(Values.SetValue(typed, 1, default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "explicit empty emitter never selects global");
                Check(Values.SetValue(HarkenVariables.VehicleRPM, 1, default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "raw explicit empty emitter never selects global");
                Check(typeof(Emitter).GetMethod("SetValue") == null &&
                    typeof(Emitter).GetMethod("SetValues") == null, "id-only emitter setters are not public");
                Check(Values.SetValue("VehicleRPM", 1, default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "named explicit empty emitter never selects global");
                Check(Values.Resolve("Vehicle\0RPM", out _) == VnResult.VN_INVALID_ARG,
                    "embedded NUL cannot truncate a Variable name");
                Check(Values.Resolve("", out _) == VnResult.VN_INVALID_ARG, "empty Variable name refused");
                const string actions = "{\"gameplayCatalog\":{\"actions\":[" +
                    "{\"name\":\"class\",\"actionStableId\":\"01234567-89ab-cdef-fedc-ba9876543210\"}," +
                    "{\"name\":\"A_class\",\"actionStableId\":\"12345678-9abc-def0-0123-456789abcdef\"}]}}";
                Require(ActionCatalogEmitter.EmitCSharpFromJson(actions, out string actionCode, out _));
                Check(actionCode.Contains("A_class = new VnObjectId(0x0123456789abcdefUL, 0xfedcba9876543210UL)") &&
                    actionCode.Contains("A_A_class = new VnObjectId"), "Action symbols retain GUIDs and escape without collisions");
                Check(ActionCatalogEmitter.EmitCSharpFromJson(actions.Replace("A_class", "class"), out _, out _) ==
                    VnResult.VN_BAD_PACK, "duplicate Action names refused by generator");
                Require(VariableCatalogEmitter.EmitCSharpFromJson(catalog.Replace("VehicleRPM", "class"),
                    "contract", out string keywordVariable, out _));
                Check(keywordVariable.Contains("V_class = new VnObjectId"), "Variable keyword is escaped");
                Require(VariableCatalogEmitter.EmitCSharpFromJson(catalog.Replace("VehicleRPM", "Label_Speed"),
                    "contract", out string labelPrefixVariable, out _));
                Check(labelPrefixVariable.Contains("V_Label_Speed = new VnObjectId"),
                    "Variable symbols cannot occupy the generated label namespace");
                Check(typeof(Emitter).GetMethod("ClearValue") == null &&
                    typeof(Emitter).GetMethod("ClearValues") == null, "local clear is not public");
                Check(typeof(Values).GetMethod("SetValue",
                    new[] { typeof(HarkenVariableId), typeof(float), typeof(Emitter?) }) != null,
                    "typed SetValue remains public");
                Check(typeof(Values).GetMethod("SetValue",
                    new[] { typeof(VnObjectId), typeof(float), typeof(Emitter?) }) != null,
                    "generated GUID SetValue remains public");
                MethodInfo unified = typeof(Values).GetMethod("SetValue",
                    new[] { typeof(string), typeof(string), typeof(float), typeof(Emitter?) });
                Check(unified != null && unified.GetParameters()[3].IsOptional, "name and project SetValue");
                Check(typeof(Actions).GetMethod("Post",
                    new[] { typeof(string), typeof(Emitter), typeof(ActionFiring).MakeByRefType() }) != null,
                    "Post(string) remains public");
                VariableGameplay.GameThreadProbe = () => false;
                VariableCatalogGuard.Query = (ref VnVariableCatalogInfoV1 info) =>
                    throw new InvalidOperationException("off-thread code reached native query");
                Check(Values.SetValue(HarkenVariableId.FromGenerated("VehicleRPM", HarkenVariables.VehicleRPM), 1) ==
                    VnResult.VN_INVALID_ARG, "thread guard precedes native entry");
                Check(Values.SetValue(typed, labelId) == VnResult.VN_INVALID_ARG,
                    "label identity thread guard precedes native entry");
                Check(Values.SetValue(typed, "class") == VnResult.VN_INVALID_ARG,
                    "label name thread guard precedes native entry");
                VariableCatalogGuard.Query = null;
                VariableGameplay.GameThreadProbe = null;

                VnVariableCatalogInfoV1 live = new VnVariableCatalogInfoV1
                {
                    structSize = 24,
                    executionCatalogFingerprint = HarkenVariables.ExecutionCatalogFingerprint,
                    sdkCatalogFingerprint = 1
                };
                Require(VariableCatalogGuard.Evaluate(in live));
                Check(VariableCatalogGuard.DisplayMismatchCount == 1,
                    "display-only mismatch reports through SDK callback without Unity");
                live.executionCatalogFingerprint ^= 1;
                Check(VariableCatalogGuard.Evaluate(in live) == VnResult.VN_VARIABLE_CATALOG_MISMATCH,
                    "execution mismatch refuses");
                VariableCatalogGuard.Query = (ref VnVariableCatalogInfoV1 info) =>
                {
                    info = live;
                    return VnResult.VN_OK;
                };
                Check(Values.SetValue(typed, labelId) == VnResult.VN_VARIABLE_CATALOG_MISMATCH,
                    "typed label identity write refuses execution mismatch before native entry");
                Check(Values.SetValue(typed, "class") == VnResult.VN_VARIABLE_CATALOG_MISMATCH,
                    "typed label name write refuses execution mismatch before native entry");

                VariableCatalogGuard.Query = (ref VnVariableCatalogInfoV1 info) =>
                {
                    info.executionCatalogFingerprint = HarkenVariables.ExecutionCatalogFingerprint;
                    VariableGameplay.NotifySessionChanged();
                    return VnResult.VN_OK;
                };
                Check(Values.SetValue(typed, 1) == VnResult.VN_NOT_INITIALIZED,
                    "catalog callbacks cannot submit a write into a replacement session");
                Check(Values.SetValue(typed, labelId) == VnResult.VN_NOT_INITIALIZED,
                    "catalog callbacks cannot submit label identity writes into a replacement session");
                Check(Values.SetValue(typed, "class") == VnResult.VN_NOT_INITIALIZED,
                    "catalog callbacks cannot submit label name writes into a replacement session");
                VariableCatalogGuard.ResetTestState();
                string nativeInit = Environment.GetEnvironmentVariable("HARKEN_VARIABLE_INIT");
                if (!string.IsNullOrEmpty(nativeInit)) RunNative(nativeInit, typed);
                Console.WriteLine("[Variable SDK] optional emitter, deprecation, session and catalog contracts PASS");
                return true;
            }
            catch (InvalidOperationException error)
            {
                Console.WriteLine("[Variable SDK] FAIL: " + error);
                return false;
            }
            finally
            {
                VariableGameplay.GameThreadProbe = null;
                VariableCatalogGuard.ResetTestState();
            }
        }

        private static unsafe void RunNative(string path, HarkenVariableId variable)
        {
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_NULL;
            config.maxEmitters = 384;
            Require(HarkenEngine.Init(config));
            try
            {
                ActivateInit(path);
                Require(Emitter.Create(out Emitter a));
                Require(Emitter.Create(out Emitter b));
                Check(Values.SetValue(variable, new VnObjectId(0, 721)) == VnResult.VN_NO_VARIABLE_LABEL,
                    "matching catalog lets typed label identity writes reach native validation");
                Check(Values.SetValue(variable, "Missing", a) == VnResult.VN_NO_VARIABLE_LABEL,
                    "matching catalog lets typed label name writes reach native validation");
                Require(Values.Resolve("VehicleRPM", out VnObjectId resolved));
                Check(resolved == variable.stableId, "Variable name resolves the complete generated GUID");
                Require(Values.SetValue(variable, 0.25f, a));
                Require(Values.SetValue(variable, 0.5f));
                Require(Values.SetValue(variable.stableId, 0.75f, b));
                Require(a.QueryVariable(variable, out VnVariableQueryResultV1 query));
                Check(query.pendingCount == 2, "local then global are both accepted synchronously");
                Require(b.QueryVariable(variable, out query));
                Check(query.pendingCount == 2, "global then local are both accepted synchronously");
#pragma warning disable CS0618
                Require(a.SetValue(variable, 0.25f));
                Require(Values.ResetValues(variable));
#pragma warning restore CS0618
                Require(Values.SetValue(variable, 0.5f));
                Require(Values.SetValue(variable, 0.5f, a));
                long before = GC.GetAllocatedBytesForCurrentThread();
                VnResult global = Values.SetValue(variable, 0.5f);
                VnResult local = Values.SetValue(variable, 0.5f, a);
                long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
                Require(global);
                Require(local);
                Check(allocated == 0, "successful unified native setters allocate no managed memory");
                Require(Values.SetValue("VehicleRPM", 0.5f));
                Require(Values.SetValue("VehicleRPM", 0.5f, a));
                before = GC.GetAllocatedBytesForCurrentThread();
                global = Values.SetValue("VehicleRPM", 0.5f);
                local = Values.SetValue("VehicleRPM", 0.5f, a);
                allocated = GC.GetAllocatedBytesForCurrentThread() - before;
                Require(global);
                Require(local);
                Check(allocated == 0, "name writes allocate no managed memory");
                Require(Values.SetValue("VehicleRPM", "00000000-0000-0000-0000-000000000001", 0.5f));
                Check(Values.SetValue("VehicleRPM", "00000000-0000-0000-0000-000000000002", 0.5f) ==
                    VnResult.VN_GAMEPLAY_WRONG_PROJECT, "project-qualified writes use the loaded Pack identity");
                Check(Values.SetValue("VehicleRPM", "00000000-0000-0000-0000-000000000001", "Missing") ==
                    VnResult.VN_NO_VARIABLE_LABEL, "name-based label writes resolve the current Variable");
                var emitters = new Emitter[320];
                for (int index = 0; index < emitters.Length; ++index)
                    Require(Emitter.Create(out emitters[index]));
                before = GC.GetAllocatedBytesForCurrentThread();
                for (int index = 0; index < emitters.Length; ++index)
                    Require(Values.SetValue(variable, 0.75f, emitters[index]));
                allocated = GC.GetAllocatedBytesForCurrentThread() - before;
                Check(allocated == 0, "320 scoped native writes allocate no managed memory");
                foreach (Emitter emitter in emitters)
                {
                    Require(emitter.QueryVariable(variable, out query));
                    Check(query.pendingCount > 0, "all 320 scoped writes are accepted before Update");
                }
                Require(emitters[0].QueryVariable(variable, out query));
                uint pendingBefore = query.pendingCount;
                before = GC.GetAllocatedBytesForCurrentThread();
                for (int index = 0; index < 10000; ++index)
                    Require(Values.SetValue(variable, (index & 1) == 0 ? 0.25f : 0.75f, emitters[0]));
                allocated = GC.GetAllocatedBytesForCurrentThread() - before;
                Check(allocated == 0, "10000 replacements allocate no managed memory");
                Require(emitters[0].QueryVariable(variable, out query));
                Check(query.pendingCount == pendingBefore,
                    "10000 unobserved replacements do not accumulate active writes");
                foreach (Emitter emitter in emitters) Require(emitter.Destroy());
                Require(a.Destroy());
                Check(Values.SetValue(variable, 0, a) == VnResult.VN_STALE_HANDLE,
                    "stale concrete emitter cannot fall back to global");
                Check(Values.SetValue("VehicleRPM", 0, a) == VnResult.VN_STALE_HANDLE,
                    "named stale concrete emitter cannot fall back to global");
                Console.WriteLine("[Variable SDK native] scoped320/replacements10000, order, invalid emitter and zero-GC PASS");
            }
            finally { HarkenEngine.Term(); }
            if (Environment.GetEnvironmentVariable("HARKEN_VARIABLE_REALTIME") == "1")
                RunAppliedCapacity(path, variable);
        }

        private static void RunAppliedCapacity(string path, HarkenVariableId variable)
        {
            VnConfig config = HarkenEngine.DefaultConfig();
            config.deviceMode = (uint)VnDeviceMode.VN_DEVICE_REAL;
            config.maxEmitters = 384;
            Require(HarkenEngine.Init(config));
            try
            {
                ActivateInit(path);
                var emitters = new Emitter[320];
                for (int index = 0; index < emitters.Length; ++index)
                    Require(Emitter.Create(out emitters[index]));
                for (int index = 0; index < emitters.Length; ++index)
                    Require(Values.SetValue(variable, 0.25f * (1 + index % 3), emitters[index]));
                foreach (Emitter emitter in emitters)
                {
                    Require(emitter.QueryVariable(variable, out VnVariableQueryResultV1 query));
                    Check(query.pendingCount != 0, "all 320 scoped writes remain pending before Update");
                }
                var deadline = Stopwatch.StartNew();
                bool settled;
                do
                {
                    Require(HarkenEngine.Update());
                    settled = true;
                    for (int index = 0; index < emitters.Length; ++index)
                    {
                        Require(emitters[index].QueryVariable(variable, out VnVariableQueryResultV1 query));
                        if (query.pendingCount != 0) settled = false;
                        else Check(query.currentValue == 0.25f * (1 + index % 3) && query.hasEmitterOverride != 0,
                            "applied scoped values retain their emitter identity");
                    }
                    if (!settled) Thread.Sleep(1);
                } while (!settled && deadline.ElapsedMilliseconds < 5000);
                Check(settled, "all 320 scoped writes apply through normal device callbacks and Update");
                foreach (Emitter emitter in emitters) Require(emitter.Destroy());
                Console.WriteLine("[Variable SDK native] device callbacks applied scoped320 with distinct values PASS");
            }
            finally { HarkenEngine.Term(); }
        }

        private static unsafe void ActivateInit(string path)
        {
            byte[] utf8 = Encoding.UTF8.GetBytes(Path.GetFullPath(path) + "\0");
            fixed (byte* p = utf8)
            {
                VnCanonicalPackSetMemberV1 member = default;
                member.structSize = (uint)sizeof(VnCanonicalPackSetMemberV1);
                member.packStableId = 11;
                member.packKind = VnConstants.VN_CANONICAL_PACK_SET_MEMBER_INIT;
                member.utf8Path = (IntPtr)p;
                VnCanonicalPackSetMemberResultV1 row = default;
                row.structSize = (uint)sizeof(VnCanonicalPackSetMemberResultV1);
                VnCanonicalPackSetActivationDescV1 desc = default;
                desc.structSize = (uint)sizeof(VnCanonicalPackSetActivationDescV1);
                desc.setRevision = 1;
                desc.memberCount = 1;
                desc.memberStructSize = member.structSize;
                desc.members = (IntPtr)(&member);
                desc.outputMemberCapacity = 1;
                desc.outputMemberStructSize = row.structSize;
                desc.outputMembers = (IntPtr)(&row);
                VnCanonicalPackSetActivationReportV1 report = default;
                report.structSize = (uint)sizeof(VnCanonicalPackSetActivationReportV1);
                Require(HarkenNative.Vn_ActivateCanonicalPackSetV1(in desc, ref report));
            }
        }

        private static void Require(VnResult result)
        {
            if (result != VnResult.VN_OK) throw new InvalidOperationException(result.ToString());
        }
        private static void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException(message);
        }
    }
}
