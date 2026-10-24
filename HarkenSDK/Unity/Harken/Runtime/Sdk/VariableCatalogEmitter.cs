using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace Harken
{
    internal static class VariableCatalogEmitter
    {
        private static readonly Regex CodeName = new Regex(@"\A[A-Za-z][A-Za-z0-9_]{0,127}\z", RegexOptions.CultureInvariant);
        private const ulong FnvOffset = 14695981039346656037UL;
        private const ulong FnvPrime = 1099511628211UL;

        public static string SymbolName(string name) => SdkSymbols.Name(name, "V_");
        public static string AuthoredName(string symbol)
            => symbol.StartsWith("V_", StringComparison.Ordinal) ? symbol.Substring(2) : symbol;

        public static VnResult EmitCSharpFromJson(string json, string sourceLabel, out string csharp, out string error)
        {
            csharp = null;
            error = "Variable catalog JSON is missing or invalid.";
            if (string.IsNullOrEmpty(json)) return VnResult.VN_INVALID_ARG;
            Dictionary<string, object> root;
            try
            {
                var parser = new JsonParser(json);
                root = parser.Read(0) as Dictionary<string, object>;
                if (root == null || !parser.End) return VnResult.VN_BAD_PACK;
            }
            catch (FormatException)
            {
                return VnResult.VN_BAD_PACK;
            }
            if (!TryReadVariables(root, out List<Variable> variables, out error))
                return VnResult.VN_BAD_PACK;
            HashCatalog(variables, out ulong execution, out ulong sdk);
            csharp = EmitCSharp(variables, execution, sdk, sourceLabel ?? "variable catalog");
            error = null;
            return VnResult.VN_OK;
        }

        public static VnResult WriteCSharpFromJson(string json, string sourceLabel, string path, out string error)
        {
            VnResult result = EmitCSharpFromJson(json, sourceLabel, out string csharp, out error);
            if (result != VnResult.VN_OK) return result;
            try
            {
                string directory = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
                File.WriteAllText(path, csharp, new UTF8Encoding(false));
                error = null;
                return VnResult.VN_OK;
            }
            catch (IOException exception)
            {
                error = exception.Message;
                return VnResult.VN_BAD_PACK;
            }
            catch (UnauthorizedAccessException exception)
            {
                error = exception.Message;
                return VnResult.VN_BAD_PACK;
            }
        }

        private sealed class Label
        {
            public int Anchor;
            public string Name;
            public VnObjectId Id;
        }

        private sealed class Variable
        {
            public VnObjectId Id;
            public byte Kind;
            public byte Scope;
            public byte Writers;
            public uint RangeMinBits;
            public uint RangeMaxBits;
            public uint DefaultBits;
            public string CodeName;
            public string DisplayName;
            public string Description;
            public string Unit;
            public uint Precision;
            public byte DisplayScale;
            public byte Deprecated;
            public VnObjectId ReplacementId;
            public ulong OwnerInitPackId;
            public List<Label> Labels = new List<Label>();
            public ulong ExecutionFingerprint;
            public ulong CatalogFingerprint;
        }

        private static bool TryReadVariables(Dictionary<string, object> root, out List<Variable> variables, out string error)
        {
            variables = new List<Variable>();
            error = "variables is missing or invalid.";
            if (!root.TryGetValue("variables", out object raw) || !(raw is List<object> rows)) return false;
            var seenIds = new HashSet<VnObjectId>();
            var seenNames = new HashSet<string>(StringComparer.Ordinal);
            foreach (object row in rows)
            {
                if (!(row is Dictionary<string, object> item)) return false;
                var variable = new Variable();
                if (!TryObjectId(item, "variableId", out variable.Id) || variable.Id.IsNull) return false;
                variable.CodeName = Text(item, "codeName");
                if (!CodeName.IsMatch(variable.CodeName) || !seenIds.Add(variable.Id) ||
                    !seenNames.Add(variable.CodeName)) return false;
                if (!TryU8(item, "kind", out variable.Kind) || variable.Kind > 1) return false;
                if (!TryU8(item, "scopePolicy", out variable.Scope) || variable.Scope != 1) return false;
                if (!TryU8(item, "writers", out variable.Writers) || variable.Writers < 1 || variable.Writers > 3)
                    return false;
                if (!TryU32(item, "rangeMinBits", out variable.RangeMinBits) ||
                    !TryU32(item, "rangeMaxBits", out variable.RangeMaxBits) ||
                    !TryU32(item, "defaultBits", out variable.DefaultBits)) return false;
                variable.DisplayName = Text(item, "displayName");
                variable.Description = Text(item, "description");
                variable.Unit = Text(item, "unit");
                if (!TryOptionalU32(item, "precision", out variable.Precision) ||
                    !TryOptionalU8(item, "displayScale", out variable.DisplayScale) ||
                    !TryOptionalU8(item, "deprecated", out variable.Deprecated) ||
                    !TryOptionalObjectId(item, "replacementId", out variable.ReplacementId) ||
                    !TryOptionalU64(item, "ownerInitPackId", out variable.OwnerInitPackId))
                    return false;
                if (item.TryGetValue("labels", out object labels))
                {
                    if (!(labels is List<object> labelRows)) return false;
                    foreach (object labelRow in labelRows)
                    {
                        if (!(labelRow is Dictionary<string, object> labelItem)) return false;
                        if (!TryI32(labelItem, "anchor", out int anchor)) return false;
                        if (!TryObjectId(labelItem, "labelId", out VnObjectId id) || id.IsNull) return false;
                        variable.Labels.Add(new Label { Anchor = anchor, Name = Text(labelItem, "name"), Id = id });
                    }
                }
                if (!TryExecutionFingerprint(variable, out variable.ExecutionFingerprint)) return false;
                variable.CatalogFingerprint = ComputeCatalogFingerprint(variable);
                variables.Add(variable);
            }
            variables.Sort((left, right) => left.Id.CompareTo(right.Id));
            error = null;
            return true;
        }

        private static bool TryExecutionFingerprint(Variable variable, out ulong fingerprint)
        {
            fingerprint = 0;
            float min = BitsToF32(variable.RangeMinBits);
            float max = BitsToF32(variable.RangeMaxBits);
            float def = BitsToF32(variable.DefaultBits);
            if (F32Bits(min) != variable.RangeMinBits || F32Bits(max) != variable.RangeMaxBits ||
                F32Bits(def) != variable.DefaultBits || min < 0 || min > max || def < min || def > max)
                return false;
            var anchors = new List<int>();
            var ids = new HashSet<VnObjectId>();
            var names = new HashSet<string>(StringComparer.Ordinal);
            variable.Labels.Sort((left, right) => left.Anchor.CompareTo(right.Anchor));
            foreach (Label label in variable.Labels)
            {
                if (label.Anchor < 0 || label.Anchor > 16777216 || !ids.Add(label.Id) ||
                    label.Name != label.Name.Trim() || label.Name.IndexOf('\0') >= 0 ||
                    (label.Name.Length != 0 && !names.Add(label.Name))) return false;
                float value = label.Anchor;
                if (value < min || value > max) return false;
                if (anchors.Count != 0 && label.Anchor <= anchors[anchors.Count - 1]) return false;
                anchors.Add(label.Anchor);
            }
            if (variable.Kind == 1) anchors.Clear();
            var hash = new Hasher("HARKEN-VARIABLE-EXECUTION-V1\0");
            hash.AddU8(1);
            hash.AddObjectId(variable.Id);
            hash.AddU8(variable.Kind);
            hash.AddU8(variable.Scope);
            hash.AddU8(variable.Writers);
            hash.AddU32(variable.RangeMinBits);
            hash.AddU32(variable.RangeMaxBits);
            hash.AddU32(variable.DefaultBits);
            hash.AddU32((uint)anchors.Count);
            foreach (int anchor in anchors) hash.AddU32((uint)anchor);
            fingerprint = hash.Finish();
            return true;
        }

        private static ulong ComputeCatalogFingerprint(Variable variable)
        {
            var hash = new Hasher("HARKEN-VARIABLE-CATALOG-V1\0");
            hash.AddObjectId(variable.Id);
            hash.AddLenBytes(variable.CodeName);
            hash.AddLenBytes(variable.DisplayName);
            hash.AddLenBytes(variable.Description);
            hash.AddLenBytes(variable.Unit);
            hash.AddU32(variable.Precision);
            hash.AddU8(variable.DisplayScale);
            hash.AddU8(variable.Deprecated);
            hash.AddU16(0);
            hash.AddObjectId(variable.ReplacementId);
            hash.AddU32((uint)variable.Labels.Count);
            foreach (Label label in variable.Labels)
            {
                hash.AddU32((uint)label.Anchor);
                hash.AddObjectId(label.Id);
                hash.AddLenBytes(label.Name);
            }
            hash.AddU64(variable.OwnerInitPackId);
            return hash.Finish();
        }

        private static void HashCatalog(List<Variable> variables, out ulong execution, out ulong sdk)
        {
            var executionHash = new Hasher("HARKEN-VARIABLE-EXECUTION-CATALOG-V1\0");
            var sdkHash = new Hasher("HARKEN-VARIABLE-SDK-CATALOG-V1\0");
            foreach (Variable variable in variables)
            {
                executionHash.AddObjectId(variable.Id);
                executionHash.AddU64(variable.ExecutionFingerprint);
                sdkHash.AddU64(variable.CatalogFingerprint);
            }
            execution = executionHash.Finish();
            sdk = sdkHash.Finish();
        }

        private static string EmitCSharp(List<Variable> variables, ulong execution, ulong sdk, string source)
        {
            var text = new StringBuilder();
            text.AppendLine("// generated, do not edit.");
            text.AppendLine("// Generated by gen_variable_catalog.py from");
            text.Append("// ").Append(SanitizeCommentText(source)).AppendLine(".");
            text.AppendLine("// Regenerate: py -3 HarkenEngine/Source/Tooling/VariableCatalog/gen_variable_catalog.py");
            text.AppendLine("namespace Harken.Generated");
            text.AppendLine("{");
            text.AppendLine("    public static class HarkenVariables");
            text.AppendLine("    {");
            text.Append("        public const ulong ExecutionCatalogFingerprint = ").Append(execution.ToString(CultureInfo.InvariantCulture)).AppendLine("UL;");
            text.Append("        public const ulong CatalogFingerprint = ").Append(sdk.ToString(CultureInfo.InvariantCulture)).AppendLine("UL;");
            foreach (Variable variable in variables)
            {
                text.Append("        public static readonly VnObjectId ").Append(SymbolName(variable.CodeName))
                    .Append(" = new VnObjectId(0x").Append(variable.Id.high.ToString("x16", CultureInfo.InvariantCulture))
                    .Append("UL, 0x").Append(variable.Id.low.ToString("x16", CultureInfo.InvariantCulture))
                    .AppendLine("UL);");
                foreach (Label label in variable.Labels)
                {
                    string name = Regex.Replace(string.IsNullOrEmpty(label.Name) ? "Unnamed" : label.Name,
                                                @"[^A-Za-z0-9]+", "_").Trim('_');
                    if (name.Length == 0) name = "Unnamed";
                    string identity = label.Id.high.ToString("x16", CultureInfo.InvariantCulture) +
                                      label.Id.low.ToString("x16", CultureInfo.InvariantCulture);
                    text.Append("        public static readonly VnObjectId Label_").Append(variable.CodeName)
                        .Append('_').Append(name).Append('_').Append(identity)
                        .Append(" = new VnObjectId(0x").Append(label.Id.high.ToString("x16", CultureInfo.InvariantCulture))
                        .Append("UL, 0x").Append(label.Id.low.ToString("x16", CultureInfo.InvariantCulture))
                        .AppendLine("UL);");
                }
            }
            text.AppendLine("    }");
            text.AppendLine("}");
            return text.ToString();
        }

        private static float BitsToF32(uint bits)
        {
            var bytes = BitConverter.GetBytes(bits);
            return BitConverter.ToSingle(bytes, 0);
        }

        private static uint F32Bits(float value)
        {
            if (float.IsNaN(value) || float.IsInfinity(value) || (value == 0f && BitConverter.ToInt32(BitConverter.GetBytes(value), 0) < 0))
                return 0xFFFFFFFF;
            return (uint)BitConverter.ToInt32(BitConverter.GetBytes(value), 0);
        }

        private static string SanitizeCommentText(string source)
        {
            if (string.IsNullOrEmpty(source)) return "variable catalog";
            char[] chars = source.Replace('\\', '/').ToCharArray();
            for (int i = 0; i < chars.Length; i++)
            {
                if (chars[i] < ' ') chars[i] = ' ';
            }
            return new string(chars);
        }

        private static string Text(Dictionary<string, object> row, string key)
            => row.TryGetValue(key, out object value) ? Convert.ToString(value, CultureInfo.InvariantCulture) ?? string.Empty : string.Empty;

        private static bool TryObjectId(Dictionary<string, object> row, string key, out VnObjectId value)
        {
            value = default;
            return row.TryGetValue(key, out object raw) && raw is string text && VnObjectId.TryParse(text, out value);
        }

        private static bool TryOptionalObjectId(Dictionary<string, object> row, string key, out VnObjectId value)
        {
            value = default;
            return !row.ContainsKey(key) || TryObjectId(row, key, out value);
        }

        private static bool TryOptionalU64(Dictionary<string, object> row, string key, out ulong value)
        {
            value = 0;
            if (!row.TryGetValue(key, out object raw)) return true;
            if (raw == null) return false;
            return TryU64(row, key, out value);
        }

        private static bool TryOptionalU32(Dictionary<string, object> row, string key, out uint value)
        {
            value = 0;
            if (!row.TryGetValue(key, out object raw)) return true;
            if (raw == null) return false;
            return TryU32(row, key, out value);
        }

        private static bool TryOptionalU8(Dictionary<string, object> row, string key, out byte value)
        {
            value = 0;
            if (!row.TryGetValue(key, out object raw)) return true;
            if (raw == null) return false;
            return TryU8(row, key, out value);
        }

        private static bool TryU64(Dictionary<string, object> row, string key, out ulong value)
        {
            value = 0;
            if (!row.TryGetValue(key, out object raw) || raw == null) return false;
            if (raw is ulong unsigned) { value = unsigned; return true; }
            if (raw is long signed && signed >= 0) { value = (ulong)signed; return true; }
            return ulong.TryParse(Convert.ToString(raw, CultureInfo.InvariantCulture), NumberStyles.Integer,
                CultureInfo.InvariantCulture, out value);
        }

        private static bool TryU32(Dictionary<string, object> row, string key, out uint value)
        {
            value = 0;
            if (!TryU64(row, key, out ulong wide) || wide > uint.MaxValue) return false;
            value = (uint)wide;
            return true;
        }

        private static bool TryU8(Dictionary<string, object> row, string key, out byte value)
        {
            value = 0;
            if (!TryU64(row, key, out ulong wide) || wide > 255) return false;
            value = (byte)wide;
            return true;
        }

        private static bool TryI32(Dictionary<string, object> row, string key, out int value)
        {
            value = 0;
            if (!row.TryGetValue(key, out object raw) || raw == null) return false;
            if (raw is long signed && signed >= int.MinValue && signed <= int.MaxValue)
            {
                value = (int)signed;
                return true;
            }
            if (raw is ulong unsigned && unsigned <= int.MaxValue)
            {
                value = (int)unsigned;
                return true;
            }
            return int.TryParse(Convert.ToString(raw, CultureInfo.InvariantCulture), NumberStyles.Integer,
                CultureInfo.InvariantCulture, out value);
        }

        private struct Hasher
        {
            private ulong hash;
            public Hasher(string domain)
            {
                hash = FnvOffset;
                AddBytes(Encoding.ASCII.GetBytes(domain));
            }
            public void AddBytes(byte[] data)
            {
                for (int i = 0; i < data.Length; ++i)
                    hash = unchecked((hash ^ data[i]) * FnvPrime);
            }
            public void AddU8(byte value) => AddBytes(new[] { value });
            public void AddU16(ushort value) => AddBytes(new[] { (byte)(value & 0xFF), (byte)(value >> 8) });
            public void AddU32(uint value)
            {
                AddBytes(new[]
                {
                    (byte)(value & 0xFF), (byte)((value >> 8) & 0xFF),
                    (byte)((value >> 16) & 0xFF), (byte)((value >> 24) & 0xFF)
                });
            }
            public void AddU64(ulong value)
            {
                var bytes = new byte[8];
                for (int i = 0; i < 8; ++i)
                {
                    bytes[i] = (byte)(value & 0xFF);
                    value >>= 8;
                }
                AddBytes(bytes);
            }
            public void AddObjectId(VnObjectId value)
            {
                var bytes = new byte[16];
                for (int i = 0; i < 8; ++i)
                {
                    bytes[i] = (byte)(value.high >> (56 - i * 8));
                    bytes[i + 8] = (byte)(value.low >> (56 - i * 8));
                }
                AddBytes(bytes);
            }
            public void AddLenBytes(string text)
            {
                byte[] encoded = Encoding.UTF8.GetBytes(text ?? string.Empty);
                AddU32((uint)encoded.Length);
                if (encoded.Length != 0) AddBytes(encoded);
            }
            public ulong Finish() => hash == 0 ? 1UL : hash;
        }
    }
}
