using System;
using System.IO;

namespace Harken
{
    internal sealed class PackMetadataInput
    {
        internal byte[] Bytes;
        internal byte[] Json;
        internal PackMetadata Names;
        internal VnPackInspectReportV1 Facts;
        internal int ByteCount => Bytes.Length;
        internal ulong SourceRevision;
        internal PackDirectorySource.ResidentSelection Selection;
    }

    // Only this source knows about filesystem publication. Sessions consume owned snapshots.
    internal sealed partial class PackDirectorySource
    {
        internal readonly string Root;
        internal readonly string Language;
        internal bool IsResidentDirectory => resident;

        internal PackDirectorySource(string root, string language, bool? residentLayout = null)
        {
            Root = Path.GetFullPath(root);
            Language = language ?? string.Empty;
            resident = residentLayout ?? File.Exists(Path.Combine(Root, "Init.hrp"));
        }

        internal PackDirectorySource ForLanguage(string language) => new PackDirectorySource(Root, language, resident);

        internal static bool ValidLanguage(string value)
        {
            if (string.IsNullOrEmpty(value)) return true;
            if (value == "." || value == ".." || value.Trim() != value) return false;
            foreach (char c in value)
                if (c < 32 || "/\\:*?\"<>|".IndexOf(c) >= 0) return false;
            return true;
        }

        internal VnResult Capture(string name, VnObjectId project, byte[] init, out PackMetadataInput input)
        {
            input = null;
            if ((name != null && (!PackMetadata.IsName(name) || name == "Init")) || !ValidLanguage(Language))
                return VnResult.VN_INVALID_ARG;
            try
            {
                if (resident) return CaptureResident(name, project, init, out input);
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
                result = PackMetadata.Parse(json, bytes, name, scope, project, out PackMetadata names);
                if (result != VnResult.VN_OK) return result;
                result = PackMetadataNative.Inspect(bytes, init, names, out VnPackInspectReportV1 facts);
                if (result != VnResult.VN_OK) return result;
                names.ProjectId = facts.projectId;
                input = new PackMetadataInput { Bytes = bytes, Json = json, Names = names, Facts = facts };
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (PackStorage.InvalidMember) { return VnResult.VN_BAD_PACK; }
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
            using (FileStream bytes = Open(PathFor(name, language, ".bytes")))
            using (FileStream json = Open(PathFor(name, language, ".json")))
                return bytes == null && json == null;
        }

        private VnResult CapturePair(string name, string language, out byte[] bytes, out byte[] json)
        {
            bytes = json = null;
            string bytesPath = PathFor(name, language, ".bytes"), jsonPath = PathFor(name, language, ".json");
            using (FileStream binary = Open(bytesPath))
            using (FileStream metadata = Open(jsonPath))
            {
                if (binary == null && metadata == null) return VnResult.VN_CONTENT_NOT_LOADED;
                if (binary == null || metadata == null) return VnResult.VN_BAD_PACK;
                bytes = PackStorage.ReadBinary(binary);
                json = PackStorage.ReadPlain(metadata);
            }
            return VnResult.VN_OK;
        }
    }
}
