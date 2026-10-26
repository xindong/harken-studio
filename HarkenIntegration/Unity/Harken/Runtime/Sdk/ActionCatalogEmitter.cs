using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace Harken
{
    internal static class ActionCatalogEmitter
    {
        private static readonly Regex NamePattern = new Regex(@"\A[A-Za-z][A-Za-z0-9_]{0,127}\z", RegexOptions.CultureInvariant);

        public static VnResult EmitCSharpFromJson(string json, out string csharp, out string error)
        {
            csharp = null;
            error = "Action catalog actions are missing or invalid.";
            if (string.IsNullOrEmpty(json)) return VnResult.VN_INVALID_ARG;
            try
            {
                var parser = new JsonParser(json);
                var root = parser.Read(0) as Dictionary<string, object>;
                if (root == null || !parser.End ||
                    !root.TryGetValue("actions", out object rawActions) ||
                    !(rawActions is List<object> actions)) return VnResult.VN_BAD_PACK;
                var rows = new SortedDictionary<string, VnObjectId>(StringComparer.Ordinal);
                var ids = new HashSet<VnObjectId>();
                foreach (object raw in actions)
                {
                    if (!(raw is Dictionary<string, object> row) ||
                        !row.TryGetValue("name", out object rawName) || !(rawName is string name) ||
                        !NamePattern.IsMatch(name) || rows.ContainsKey(name) ||
                        !row.TryGetValue("actionStableId", out object rawId) || !(rawId is string idText) ||
                        !VnObjectId.TryParse(idText, out VnObjectId id) || id.IsNull || !ids.Add(id))
                        return VnResult.VN_BAD_PACK;
                    rows.Add(name, id);
                }
                var text = new StringBuilder("namespace Harken.Generated\n{\n    public static class HarkenActions\n    {\n");
                foreach (var row in rows)
                    text.Append("        public static readonly VnObjectId ").Append(SdkSymbols.Name(row.Key, "A_"))
                        .Append(" = new VnObjectId(0x").Append(row.Value.high.ToString("x16"))
                        .Append("UL, 0x").Append(row.Value.low.ToString("x16")).Append("UL);\n");
                text.Append("    }\n}\n");
                csharp = text.ToString();
                error = null;
                return VnResult.VN_OK;
            }
            catch (FormatException) { return VnResult.VN_BAD_PACK; }
        }
    }
}
