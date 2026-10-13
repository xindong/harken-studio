using System;
using System.IO;
using System.Globalization;

namespace Harken.Samples
{
    /// <summary>
    /// Name-only sample: explicit Pack load, then <see cref="Actions.Post(string, Emitter, out ActionFiring)"/>.
    /// No numeric Action id, no generated constants, no sidecar I/O at Post time.
    /// </summary>
    public static class NameFirstPost
    {
        public static VnResult LoadAndPost(
            string initPackPath,
            string platformRoot,
            ulong contentPackStableId,
            string actionName,
            out ActionFiring firing)
        {
            firing = default;
            VnResult result = Packs.InspectInitPack(initPackPath, out var inspection);
            if (result != VnResult.VN_OK)
                return result;
            if (!PackManifest.TryReadManifest(platformRoot, null, out _,
                    out var entries, out _, out ulong initId))
                return VnResult.VN_BAD_PACK;
            var init = Array.Find(entries, entry => entry.PackStableId == initId);
            if (inspection.PackStableId != initId ||
                !PackManifest.TryJoinContainedPath(platformRoot, init.Path, out string expectedInitPath))
                return VnResult.VN_PACK_STALE_REVISION;
            try
            {
                var comparison = Path.DirectorySeparatorChar == '\\'
                    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                if (!string.Equals(expectedInitPath, Path.GetFullPath(initPackPath), comparison))
                    return VnResult.VN_PACK_STALE_REVISION;
            }
            catch (ArgumentException) { return VnResult.VN_INVALID_ARG; }
            catch (NotSupportedException) { return VnResult.VN_INVALID_ARG; }
            catch (PathTooLongException) { return VnResult.VN_INVALID_ARG; }
            if (contentPackStableId == initId ||
                !PackManifest.TryResolvePackPath(platformRoot, contentPackStableId, out string contentPath))
                return VnResult.VN_UNKNOWN_ID;
            var content = Array.Find(entries, entry => entry.PackStableId == contentPackStableId);
            foreach (string dependency in content.Dependencies)
                if (dependency != initId.ToString(CultureInfo.InvariantCulture))
                    return VnResult.VN_BAD_PACK;
            var members = new[] {
                new CanonicalPackMember(initId, 2, expectedInitPath, platformRoot),
                new CanonicalPackMember(contentPackStableId, 3, contentPath, platformRoot)
            };
            result = Packs.ActivateCanonicalPackSet(members, 1, out _, out _);
            if (result != VnResult.VN_OK)
            {
                return result;
            }
            return Actions.Post(actionName, Emitter.None, out firing);
        }
    }
}
