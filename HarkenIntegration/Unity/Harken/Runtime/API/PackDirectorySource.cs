using System;
using System.IO;

namespace Harken
{
    internal sealed class CompanionInput
    {
        internal byte[] Bytes;
        internal byte[] Json;
        internal PackCompanion Names;
        internal VnPackInspectReportV1 Facts;
        internal ulong SourceRevision;
    }

    // Only this source knows about filesystem publication. Sessions consume owned snapshots.
    internal sealed class PackDirectorySource
    {
        internal readonly string Root;
        internal readonly string Language;

        internal PackDirectorySource(string root, string language)
        {
            Root = Path.GetFullPath(root);
            Language = language ?? string.Empty;
        }

        internal static bool ValidLanguage(string value)
        {
            if (string.IsNullOrEmpty(value)) return true;
            if (value == "." || value == ".." || value.Trim() != value) return false;
            foreach (char c in value)
                if (c < 32 || "/\\:*?\"<>|".IndexOf(c) >= 0) return false;
            return true;
        }

        internal VnResult Capture(string name, VnObjectId project, byte[] init, out CompanionInput input)
        {
            input = null;
            if ((name != null && (!PackCompanion.IsName(name) || name == "Init")) || !ValidLanguage(Language))
                return VnResult.VN_INVALID_ARG;
            try
            {
                string scope = name == null ? string.Empty : Language;
                VnResult result = CapturePair(name ?? "Init", scope, out byte[] bytes, out byte[] json);
                bool fallback = name != null && scope.Length != 0 && result == VnResult.VN_CONTENT_NOT_LOADED;
                if (fallback)
                {
                    result = CapturePair(name, string.Empty, out bytes, out json);
                    scope = string.Empty;
                    // The original absence is part of the same source decision.
                    if (!PairStillMissing(name, Language)) return VnResult.VN_PACK_STALE_REVISION;
                }
                if (result != VnResult.VN_OK) return result;
                result = PackCompanion.Parse(json, bytes, name, scope, project, out PackCompanion names);
                if (result != VnResult.VN_OK) return result;
                result = CompanionNative.Inspect(bytes, init, names, out VnPackInspectReportV1 facts);
                if (result != VnResult.VN_OK) return result;
                names.ProjectId = facts.projectId;
                input = new CompanionInput { Bytes = bytes, Json = json, Names = names, Facts = facts };
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (IOException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
            catch (UnauthorizedAccessException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
            catch (ArgumentException) { return VnResult.VN_INVALID_ARG; }
        }

        private string PathFor(string name, string language, string suffix)
        {
            string directory = language.Length == 0 ? Root : Path.Combine(Root, language);
            if (language.Length != 0 && Exists(directory) &&
                (File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("A Pack language directory must not redirect outside its source.");
            return Path.Combine(directory, name + suffix);
        }

        private bool Publishing() => Exists(Path.Combine(Root, ".harken-pack-publishing"));

        private static bool Exists(string path)
        {
            try { File.GetAttributes(path); return true; }
            catch (FileNotFoundException) { return false; }
            catch (DirectoryNotFoundException) { return false; }
        }

        private static FileStream Open(string path)
        {
            try
            {
                if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("A Pack input must be a regular source file.");
                return new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            }
            catch (FileNotFoundException) { return null; }
            catch (DirectoryNotFoundException) { return null; }
        }

        private bool PairStillMissing(string name, string language)
        {
            if (Publishing()) return false;
            using (FileStream bytes = Open(PathFor(name, language, ".bytes")))
            using (FileStream json = Open(PathFor(name, language, ".json")))
                return bytes == null && json == null && !Publishing();
        }

        private VnResult CapturePair(string name, string language, out byte[] bytes, out byte[] json)
        {
            bytes = json = null;
            if (Publishing()) return VnResult.VN_PACK_STALE_REVISION;
            string bytesPath = PathFor(name, language, ".bytes"), jsonPath = PathFor(name, language, ".json");
            using (FileStream binary = Open(bytesPath))
            using (FileStream metadata = Open(jsonPath))
            {
                if (binary == null && metadata == null)
                    return PairStillMissing(name, language) ? VnResult.VN_CONTENT_NOT_LOADED : VnResult.VN_PACK_STALE_REVISION;
                if (binary == null || metadata == null) return VnResult.VN_BAD_PACK;
                bytes = Read(binary);
                json = Read(metadata);
            }
            return Publishing() ? VnResult.VN_PACK_STALE_REVISION : VnResult.VN_OK;
        }

        private static byte[] Read(FileStream stream)
        {
            if (stream.Length > int.MaxValue) throw new IOException("Pack input exceeds the managed byte limit.");
            var bytes = new byte[(int)stream.Length];
            int offset = 0;
            while (offset < bytes.Length)
            {
                int count = stream.Read(bytes, offset, bytes.Length - offset);
                if (count == 0) throw new EndOfStreamException();
                offset += count;
            }
            return bytes;
        }
    }
}
