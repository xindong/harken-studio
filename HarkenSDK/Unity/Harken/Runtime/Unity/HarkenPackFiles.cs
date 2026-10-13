using System;
using System.IO;

namespace Harken.Unity
{
    internal static class HarkenPackFiles
    {
        internal static bool CanHaveWrapper(string fullPath)
            => !fullPath.EndsWith(".bytes", StringComparison.OrdinalIgnoreCase) &&
                !fullPath.EndsWith(".json", StringComparison.OrdinalIgnoreCase);

        internal static bool HasWrapper(string fullPath)
            => CanHaveWrapper(fullPath) && File.Exists(fullPath + ".bytes");

        internal static byte[] Read(string fullPath)
        {
            if (string.IsNullOrEmpty(fullPath)) return null;
            string textAssetPath = fullPath + ".bytes";
            bool plain = File.Exists(fullPath);
            bool textAsset = HasWrapper(fullPath);
            if (plain && textAsset)
            {
                throw new IOException("Both " + Path.GetFileName(fullPath) + " and " +
                    Path.GetFileName(textAssetPath) + " exist.");
            }
            if (textAsset) return File.ReadAllBytes(textAssetPath);
            if (plain) return File.ReadAllBytes(fullPath);
            return null;
        }
    }
}
