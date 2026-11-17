using System;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Globalization;
using System.IO;
using Harken;

namespace Harken.Test
{
    internal static class VariableApiContract
    {
        public static bool Run()
        {
            try
            {
                string catalog = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "VariableCatalog.json"));
                Require(VariableCatalogEmitter.EmitCSharpFromJson(catalog, "contract", out string generated, out _));
                ulong execution = GeneratedConstant(generated, "ExecutionCatalogFingerprint");
                ulong display = GeneratedConstant(generated, "CatalogFingerprint");
                VnObjectId vehicleRpm = VnObjectId.Parse("67e0c65b-f36e-40d9-80ee-75af00ecf712");
                Check(generated.Contains("Harken.Values.RegisterGeneratedCatalog(ExecutionCatalogFingerprint, " +
                    "CatalogFingerprint, typeof(HarkenVariables))"), "generated catalog registers itself");
                Check(generated.Contains("VehicleRPM = new VnObjectId(0x" +
                    vehicleRpm.high.ToString("x16", CultureInfo.InvariantCulture) + "UL, 0x" +
                    vehicleRpm.low.ToString("x16", CultureInfo.InvariantCulture) + "UL);"),
                    "variable generation preserves both GUID halves");
                string numericId = catalog.Replace("\"" + vehicleRpm + "\"", "7");
                Check(VariableCatalogEmitter.EmitCSharpFromJson(numericId, "contract", out _, out _) ==
                    VnResult.VN_BAD_PACK, "numeric variable identity is refused");
                string numericReplacement = catalog.Replace("\"00000000-0000-0000-0000-000000000000\"", "7");
                Check(VariableCatalogEmitter.EmitCSharpFromJson(numericReplacement, "contract", out _, out _) ==
                    VnResult.VN_BAD_PACK, "numeric replacement identity is refused");
                var typed = HarkenVariableId.FromGenerated("VehicleRPM", vehicleRpm);
                var labelId = new VnObjectId(0, 721);
                string labels = "[{\"anchor\":0,\"name\":\"class\",\"labelId\":\"00000000-0000-4000-8000-000000000001\"}," +
                    "{\"anchor\":1,\"name\":\"A-A\",\"labelId\":\"00000000-0000-4000-8000-000000000002\"}]";
                string named = catalog.Replace("\"labels\": []", "\"labels\": " + labels);
                Require(VariableCatalogEmitter.EmitCSharpFromJson(named, "labels", out string namedGenerated, out _));
                Check(namedGenerated.Contains("Label_VehicleRPM_class_00000000000040008000000000000001"),
                    "Smooth labels generate stable identities with safe identifiers");
                Check(namedGenerated.Contains("ExecutionCatalogFingerprint = " + execution + "UL;"),
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
                Check(Values.SetValue(vehicleRpm, 1, default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "raw explicit empty emitter never selects global");
                Check(typeof(Emitter).GetMethod("SetValue") == null &&
                    typeof(Emitter).GetMethod("SetValues") == null, "id-only emitter setters are not public");
                Check(Values.SetValue("VehicleRPM", 1, default(Emitter)) == VnResult.VN_INVALID_ARG,
                    "named explicit empty emitter never selects global");
                Check(Values.Resolve("Vehicle\0RPM", out _) == VnResult.VN_INVALID_ARG,
                    "embedded NUL cannot truncate a Variable name");
                Check(Values.Resolve("", out _) == VnResult.VN_INVALID_ARG, "empty Variable name refused");
                const string actions = "{\"actions\":[" +
                    "{\"name\":\"class\",\"actionStableId\":\"01234567-89ab-cdef-fedc-ba9876543210\"}," +
                    "{\"name\":\"A_class\",\"actionStableId\":\"12345678-9abc-def0-0123-456789abcdef\"}]}";
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
                Check(Values.SetValue(HarkenVariableId.FromGenerated("VehicleRPM", vehicleRpm), 1) ==
                    VnResult.VN_INVALID_ARG, "thread guard precedes native entry");
                Check(Values.SetValue(typed, labelId) == VnResult.VN_INVALID_ARG,
                    "label identity thread guard precedes native entry");
                Check(Values.SetValue(typed, "class") == VnResult.VN_INVALID_ARG,
                    "label name thread guard precedes native entry");
                VariableCatalogGuard.Query = null;
                VariableGameplay.GameThreadProbe = null;

                Require(Values.RegisterGeneratedCatalog(execution, display, typeof(VariableApiContract)));
                VnVariableCatalogInfoV1 live = new VnVariableCatalogInfoV1
                {
                    structSize = 24,
                    executionCatalogFingerprint = execution,
                    sdkCatalogFingerprint = 1
                };
                Require(VariableCatalogGuard.Evaluate(in live));
                Check(VariableCatalogGuard.DisplayMismatchCount == 1,
                    "display-only mismatch reports through managed callback without Unity");
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
                    info.executionCatalogFingerprint = execution;
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
                Console.WriteLine("[Variable API] optional emitter, session and catalog contracts PASS");
                return true;
            }
            catch (InvalidOperationException error)
            {
                Console.WriteLine("[Variable API] FAIL: " + error);
                return false;
            }
            finally
            {
                VariableGameplay.GameThreadProbe = null;
                VariableCatalogGuard.ResetTestState();
            }
        }

        private static ulong GeneratedConstant(string generated, string name)
        {
            Match match = Regex.Match(generated, @"\b" + name + @" = (\d+)UL;");
            if (!match.Success) throw new InvalidOperationException(name + " is missing from generated code");
            return ulong.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture);
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
