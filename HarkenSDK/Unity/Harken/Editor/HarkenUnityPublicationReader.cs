using System;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace Harken.Unity.Editor
{
    internal static class HarkenUnityPublicationReader
    {
        public static bool ShouldCaptureUnityAssets(string directory, string relativeInitPack)
        {
            if (string.IsNullOrEmpty(directory) || !Directory.Exists(directory)) return false;
            if (!PackManifest.TryJoinContainedPath(directory, relativeInitPack, out string init)) return false;
            if (!HarkenPackFiles.CanHaveWrapper(relativeInitPack))
                return AssetDatabase.LoadAssetAtPath<TextAsset>(ToAssetPath(init)) != null;
            string bytes = init + ".bytes";
            return !File.Exists(init) && AssetDatabase.LoadAssetAtPath<TextAsset>(ToAssetPath(bytes)) != null;
        }

        public static bool HasAmbiguousPackFiles(string directory, string relativePack, out string error)
        {
            error = null;
            if (string.IsNullOrEmpty(directory) || string.IsNullOrEmpty(relativePack)) return false;
            if (!PackManifest.TryJoinContainedPath(directory, relativePack, out string packPath)) return false;
            if (!HarkenPackFiles.CanHaveWrapper(relativePack)) return false;
            string bytesPath = packPath + ".bytes";
            if (File.Exists(packPath) && File.Exists(bytesPath))
            {
                error = "Ambiguous Pack file: both " + relativePack + " and " + relativePack + ".bytes exist.";
                return true;
            }
            return false;
        }

        public static bool TryManifestPath(string directory, out string manifestPath)
        {
            manifestPath = null;
            if (string.IsNullOrEmpty(directory)) return false;
            string full = Path.GetFullPath(directory);
            string assets = Path.GetFullPath(Application.dataPath);
            if (!full.StartsWith(assets, StringComparison.OrdinalIgnoreCase)) return false;
            string relative = "Assets" + full.Substring(assets.Length).Replace('\\', '/');
            if (relative.EndsWith("/")) relative = relative.Substring(0, relative.Length - 1);
            manifestPath = relative + "/" + PackManifest.ManifestFileName;
            return PackManifest.IsSafeRelativePath(manifestPath);
        }

        public static byte[] Read(string logicalPath)
        {
            if (string.IsNullOrEmpty(logicalPath) || logicalPath.IndexOf('\\') >= 0) return null;
            bool wrapped = HarkenPackFiles.CanHaveWrapper(logicalPath);
            if (wrapped && File.Exists(logicalPath) && File.Exists(logicalPath + ".bytes"))
                throw new IOException("Ambiguous Pack file: both " + logicalPath + " and " + logicalPath + ".bytes exist.");
            TextAsset asset = AssetDatabase.LoadAssetAtPath<TextAsset>(logicalPath);
            if (asset != null) return (byte[])asset.bytes.Clone();
            if (wrapped)
            {
                asset = AssetDatabase.LoadAssetAtPath<TextAsset>(logicalPath + ".bytes");
                if (asset != null) return (byte[])asset.bytes.Clone();
            }
            if (!logicalPath.StartsWith("Assets/", StringComparison.Ordinal)) return null;
            string full = Path.Combine(Application.dataPath, logicalPath.Substring("Assets/".Length));
            return HarkenPackFiles.Read(full);
        }

        private static string ToAssetPath(string filesystemPath)
        {
            if (string.IsNullOrEmpty(filesystemPath)) return string.Empty;
            string full = Path.GetFullPath(filesystemPath);
            string assets = Path.GetFullPath(Application.dataPath);
            if (!full.StartsWith(assets, StringComparison.OrdinalIgnoreCase)) return string.Empty;
            return ("Assets" + full.Substring(assets.Length)).Replace('\\', '/');
        }
    }
}
