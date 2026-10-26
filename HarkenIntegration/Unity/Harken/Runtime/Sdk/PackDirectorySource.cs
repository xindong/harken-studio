using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Harken
{
    internal sealed class CompanionInput
    {
        internal byte[] Bytes;
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
                input = new CompanionInput { Bytes = bytes, Names = names, Facts = facts };
                return VnResult.VN_OK;
            }
            catch (OutOfMemoryException) { return VnResult.VN_DEVICE_ERROR; }
            catch (PlatformNotSupportedException) { return VnResult.VN_CAPABILITY_UNAVAILABLE; }
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
                FileIdentity beforeBinary = FileIdentity.Read(binary), beforeMetadata = FileIdentity.Read(metadata);
                if (!FileIdentity.ExactName(binary, name + ".bytes", language) ||
                    !FileIdentity.ExactName(metadata, name + ".json", language)) return VnResult.VN_BAD_PACK;
                bytes = Read(binary);
                json = Read(metadata);
                using (FileStream currentBinary = Open(bytesPath))
                using (FileStream currentMetadata = Open(jsonPath))
                {
                    if (currentBinary == null || currentMetadata == null || Publishing() ||
                        !beforeBinary.Equals(FileIdentity.Read(binary)) ||
                        !beforeMetadata.Equals(FileIdentity.Read(metadata)) ||
                        !beforeBinary.Equals(FileIdentity.Read(currentBinary)) ||
                        !beforeMetadata.Equals(FileIdentity.Read(currentMetadata)))
                        return VnResult.VN_PACK_STALE_REVISION;
                }
                return VnResult.VN_OK;
            }
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

        private readonly struct FileIdentity : IEquatable<FileIdentity>
        {
            private readonly ulong device, inode, length, modified, modifiedNs, changed, changedNs;
            private FileIdentity(ulong device, ulong inode, ulong length, ulong modified,
                ulong modifiedNs = 0, ulong changed = 0, ulong changedNs = 0)
            {
                this.device = device; this.inode = inode; this.length = length;
                this.modified = modified; this.modifiedNs = modifiedNs;
                this.changed = changed; this.changedNs = changedNs;
            }
            public bool Equals(FileIdentity other) => device == other.device && inode == other.inode &&
                length == other.length && modified == other.modified && modifiedNs == other.modifiedNs &&
                changed == other.changed && changedNs == other.changedNs;

            internal static FileIdentity Read(FileStream stream)
            {
                if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                {
                    if (!GetFileInformationByHandle(stream.SafeFileHandle, out WindowsInfo info))
                        throw new IOException("Cannot identify the opened Pack file.");
                    return new FileIdentity(info.Volume, ((ulong)info.IndexHigh << 32) | info.IndexLow,
                        ((ulong)info.SizeHigh << 32) | info.SizeLow, ((ulong)info.WriteHigh << 32) | info.WriteLow);
                }
                if (IntPtr.Size != 8) throw new PlatformNotSupportedException();
                long descriptor = stream.SafeFileHandle.DangerousGetHandle().ToInt64();
                if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
                {
                    if (RuntimeInformation.ProcessArchitecture != Architecture.X64) throw new PlatformNotSupportedException();
                    if (LinuxStat((int)descriptor, out LinuxInfo info) != 0) throw new IOException("Cannot stat Pack input.");
                    return new FileIdentity(info.Device, info.Inode, info.Length, info.Modified,
                        info.ModifiedNs, info.Changed, info.ChangedNs);
                }
                if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
                {
                    if (RuntimeInformation.ProcessArchitecture != Architecture.X64 &&
                        RuntimeInformation.ProcessArchitecture != Architecture.Arm64) throw new PlatformNotSupportedException();
                    int result = RuntimeInformation.ProcessArchitecture == Architecture.X64
                        ? MacStat64((int)descriptor, out MacInfo info) : MacStat((int)descriptor, out info);
                    if (result != 0) throw new IOException("Cannot stat Pack input.");
                    return new FileIdentity(info.Device, info.Inode, info.Length, info.Modified,
                        info.ModifiedNs, info.Changed, info.ChangedNs);
                }
                throw new PlatformNotSupportedException();
            }

            internal static bool ExactName(FileStream stream, string name, string language)
            {
                string path;
                if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                {
                    var buffer = new StringBuilder(32768);
                    uint count = GetFinalPathNameByHandle(stream.SafeFileHandle, buffer, (uint)buffer.Capacity, 0);
                    if (count == 0 || count >= buffer.Capacity) throw new IOException("Cannot resolve Pack file identity.");
                    path = buffer.ToString();
                }
                else if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
                {
                    var buffer = new byte[1024];
                    int descriptor = stream.SafeFileHandle.DangerousGetHandle().ToInt32();
                    const int getPath = 50; // F_GETPATH
                    int result = RuntimeInformation.ProcessArchitecture == Architecture.Arm64
                        ? MacPathArm64(descriptor, getPath, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero,
                            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, buffer)
                        : MacPathX64(descriptor, getPath, buffer);
                    if (result != 0)
                        throw new IOException("Cannot resolve Pack file identity.");
                    path = Encoding.UTF8.GetString(buffer, 0, Array.IndexOf(buffer, (byte)0));
                }
                else return true;
                return string.Equals(Path.GetFileName(path), name, StringComparison.Ordinal) &&
                    (language.Length == 0 || string.Equals(Path.GetFileName(Path.GetDirectoryName(path)), language, StringComparison.Ordinal));
            }

            [StructLayout(LayoutKind.Sequential)]
            private struct WindowsInfo
            {
                internal uint Attributes, CreationLow, CreationHigh, AccessLow, AccessHigh, WriteLow, WriteHigh;
                internal uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
            }
            // Field offsets from the supported Linux x86_64 stat ABI.
            [StructLayout(LayoutKind.Explicit, Size = 256)]
            private struct LinuxInfo
            {
                [FieldOffset(0)] internal ulong Device;
                [FieldOffset(8)] internal ulong Inode;
                [FieldOffset(48)] internal ulong Length;
                [FieldOffset(88)] internal ulong Modified;
                [FieldOffset(96)] internal ulong ModifiedNs;
                [FieldOffset(104)] internal ulong Changed;
                [FieldOffset(112)] internal ulong ChangedNs;
            }
            [StructLayout(LayoutKind.Explicit, Size = 256)]
            private struct MacInfo
            {
                [FieldOffset(0)] internal uint Device;
                [FieldOffset(8)] internal ulong Inode;
                [FieldOffset(48)] internal ulong Modified;
                [FieldOffset(56)] internal ulong ModifiedNs;
                [FieldOffset(64)] internal ulong Changed;
                [FieldOffset(72)] internal ulong ChangedNs;
                [FieldOffset(96)] internal ulong Length;
            }
            [DllImport("kernel32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out WindowsInfo info);
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            private static extern uint GetFinalPathNameByHandle(SafeFileHandle handle, StringBuilder path, uint length, uint flags);
            [DllImport("libc", EntryPoint = "fstat", SetLastError = true)]
            private static extern int LinuxStat(int descriptor, out LinuxInfo info);
            [DllImport("libSystem.B.dylib", EntryPoint = "fstat", SetLastError = true)]
            private static extern int MacStat(int descriptor, out MacInfo info);
            [DllImport("libSystem.B.dylib", EntryPoint = "fstat$INODE64", SetLastError = true)]
            private static extern int MacStat64(int descriptor, out MacInfo info);
            [DllImport("libSystem.B.dylib", EntryPoint = "fcntl", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
            private static extern int MacPathX64(int descriptor, int command, [Out] byte[] path);
            // Darwin ARM64 passes fcntl's variadic argument on the stack. Occupy x2-x7 so the
            // path pointer reaches the first stack slot without relying on managed varargs support.
            // https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms
            [DllImport("libSystem.B.dylib", EntryPoint = "fcntl", CallingConvention = CallingConvention.Cdecl, SetLastError = true)]
            private static extern int MacPathArm64(int descriptor, int command, IntPtr x2, IntPtr x3,
                IntPtr x4, IntPtr x5, IntPtr x6, IntPtr x7, [Out] byte[] path);
        }
    }
}
