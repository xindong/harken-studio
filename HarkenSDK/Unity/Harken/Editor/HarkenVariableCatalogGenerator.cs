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
        {
            error = HarkenEditorPublicationSupport.Diagnostic;
            return VnResult.VN_CAPABILITY_UNAVAILABLE;
        }

        // Temporarily unregistered until symbol generation reads current Pack companions instead of a legacy manifest.
        // [MenuItem("Harken/Generate SDK Symbols From Publication")]
        public static void GenerateFromMenu()
        {
            EditorUtility.DisplayDialog("Harken SDK Symbols", HarkenEditorPublicationSupport.Diagnostic, "OK");
        }

        public static VnResult GeneratePublication(string jsonPath, string variablePath, out string error)
        {
            error = HarkenEditorPublicationSupport.Diagnostic;
            return VnResult.VN_CAPABILITY_UNAVAILABLE;
        }

        public static VnResult Generate(string jsonPath, string csharpPath, out string error)
        {
            error = HarkenEditorPublicationSupport.Diagnostic;
            return VnResult.VN_CAPABILITY_UNAVAILABLE;
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

    }
}
