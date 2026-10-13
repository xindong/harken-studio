using System.IO;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Editor
{
    public static class HarkenVariableCatalogGenerator
    {
        public const string GeneratedAssetPath = "Packages/com.harken.core/Runtime/Generated/HarkenVariables.g.cs";

        /// <summary>Validates a publication's Variable catalog without writing assets.</summary>
        public static VnResult ValidatePublication(string json, string sourceLabel, out string error)
            => VariableCatalogEmitter.EmitCSharpFromJson(json, sourceLabel, out _, out error);

        [MenuItem("Harken/Generate SDK Symbols From Publication")]
        public static void GenerateFromMenu()
        {
            string path = EditorUtility.OpenFilePanel("Select Harken publication JSON",
                Application.dataPath, "json");
            if (string.IsNullOrEmpty(path)) return;
            if (GeneratePublication(path, ResolveOutputPath(), out string error) != VnResult.VN_OK)
                EditorUtility.DisplayDialog("Harken SDK Symbols", error, "OK");
            else
                EditorUtility.DisplayDialog("Harken SDK Symbols",
                    "Generated Action and Variable symbols from " + path, "OK");
        }

        public static VnResult GeneratePublication(string jsonPath, string variablePath, out string error)
        {
            error = "A publication path and Variable output path are required.";
            if (string.IsNullOrEmpty(jsonPath) || string.IsNullOrEmpty(variablePath)) return VnResult.VN_INVALID_ARG;
            try
            {
                string json = File.ReadAllText(jsonPath);
                VnResult result = VariableCatalogEmitter.EmitCSharpFromJson(json, jsonPath, out string variables, out error);
                if (result != VnResult.VN_OK) return result;
                result = ActionCatalogEmitter.EmitCSharpFromJson(json, out string actions, out error);
                if (result != VnResult.VN_OK) return result;
                string directory = Path.GetDirectoryName(Path.GetFullPath(variablePath));
                string actionPath = Path.Combine(directory, "HarkenActions.g.cs");
                Directory.CreateDirectory(directory);
                File.WriteAllText(variablePath, variables, new System.Text.UTF8Encoding(false));
                File.WriteAllText(actionPath, actions, new System.Text.UTF8Encoding(false));
                AssetDatabase.ImportAsset(ToAssetPath(variablePath), ImportAssetOptions.ForceUpdate);
                AssetDatabase.ImportAsset(ToAssetPath(actionPath), ImportAssetOptions.ForceUpdate);
                return VnResult.VN_OK;
            }
            catch (IOException exception) { error = exception.Message; return VnResult.VN_BAD_PACK; }
            catch (System.UnauthorizedAccessException exception) { error = exception.Message; return VnResult.VN_BAD_PACK; }
        }

        public static VnResult Generate(string jsonPath, string csharpPath, out string error)
        {
            error = "A publication JSON path and C# output path are required.";
            if (string.IsNullOrEmpty(jsonPath) || string.IsNullOrEmpty(csharpPath)) return VnResult.VN_INVALID_ARG;
            string json;
            try { json = File.ReadAllText(jsonPath); }
            catch (IOException exception)
            {
                error = exception.Message;
                return VnResult.VN_BAD_PACK;
            }
            VnResult result = VariableCatalogEmitter.WriteCSharpFromJson(json, jsonPath, csharpPath, out error);
            if (result != VnResult.VN_OK) return result;
            AssetDatabase.ImportAsset(ToAssetPath(csharpPath), ImportAssetOptions.ForceUpdate);
            return VnResult.VN_OK;
        }

        public static string ResolveOutputPath()
        {
            foreach (string guid in AssetDatabase.FindAssets("HarkenVariables t:MonoScript"))
            {
                string asset = AssetDatabase.GUIDToAssetPath(guid);
                if (asset.EndsWith("/HarkenVariables.g.cs"))
                    return Path.GetFullPath(asset);
            }
            string package = Path.GetFullPath(GeneratedAssetPath);
            return File.Exists(package) ? package : Path.GetFullPath("Assets/Harken/Generated/HarkenVariables.g.cs");
        }

        private static string ToAssetPath(string filesystemPath)
        {
            string full = Path.GetFullPath(filesystemPath).Replace('\\', '/');
            string data = Path.GetFullPath(Application.dataPath).Replace('\\', '/');
            if (full.StartsWith(data, System.StringComparison.OrdinalIgnoreCase))
                return "Assets" + full.Substring(data.Length);
            string packages = Path.GetFullPath(Path.Combine(data, "..", "Packages")).Replace('\\', '/');
            if (full.StartsWith(packages, System.StringComparison.OrdinalIgnoreCase))
                return "Packages" + full.Substring(packages.Length);
            return filesystemPath.Replace('\\', '/');
        }
    }
}
