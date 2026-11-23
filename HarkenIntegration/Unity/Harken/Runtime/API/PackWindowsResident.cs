using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Harken
{
    internal sealed partial class PackDirectorySource
    {
        private readonly bool resident;
        private const int HeaderBytes = 16;
        private const int DigestBytes = 32;
        // HRP input and expanded parts are each held in a single byte array.
        private const int MaxArrayBytes = 0x7FFFFFC7;

        private static readonly bool IsWindows = RuntimeInformation.IsOSPlatform(OSPlatform.Windows);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        private static extern SafeFileHandle CreateFileW(string path, uint access, FileShare share,
            IntPtr securityAttributes, FileMode mode, uint flags, IntPtr template);

        private static SafeFileHandle OpenForIdentity(string path, FileShare share)
        {
            const uint genericRead = 0x80000000, openReparsePoint = 0x00200000;
            var handle = CreateFileW(path, genericRead, share,
                IntPtr.Zero, FileMode.Open, openReparsePoint, IntPtr.Zero);
            if (!handle.IsInvalid) return handle;
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            if (error == 2 || error == 3) return null;
            throw new IOException("Cannot reopen Pack input.", new System.ComponentModel.Win32Exception(error));
        }

        internal readonly struct FileIdentity : IEquatable<FileIdentity>
        {
            private readonly ulong device, inode, length, modified;
            internal ulong Length => length;
            internal void SetBatchIdentity(ref VnResidentPackVisitDescV1 directory, long changeTime)
            {
                directory.initVolumeId = device;
                directory.initFileId = inode;
                directory.initFileSize = length;
                directory.initWriteTime = modified;
                directory.initChangeTime = (ulong)changeTime;
            }
            private FileIdentity(ulong device, ulong inode, ulong length, ulong modified)
            {
                this.device = device;
                this.inode = inode;
                this.length = length;
                this.modified = modified;
            }
            public bool Equals(FileIdentity other) => device == other.device && inode == other.inode &&
                length == other.length && modified == other.modified;

            internal static FileIdentity ReadWindows(SafeFileHandle handle)
            {
                if (!GetFileInformationByHandle(handle, out WindowsInfo info))
                    throw new IOException("Cannot identify the opened Pack file.");
                if ((info.Attributes & (uint)FileAttributes.ReparsePoint) != 0) throw new IOException("A Pack input must be a regular file.");
                return new FileIdentity(info.Volume, ((ulong)info.IndexHigh << 32) | info.IndexLow,
                    ((ulong)info.SizeHigh << 32) | info.SizeLow, ((ulong)info.WriteHigh << 32) | info.WriteLow);
            }

            internal static string WindowsPath(SafeFileHandle handle, int pathLength)
            {
                const int maxPathChars = 32768;
                var buffer = new StringBuilder(Math.Min(maxPathChars, Math.Max(260, pathLength + 16)));
                uint count = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, 0);
                if (count >= buffer.Capacity && count <= maxPathChars)
                {
                    buffer = new StringBuilder((int)count);
                    count = GetFinalPathNameByHandle(handle, buffer, count, 0);
                }
                if (count == 0 || count >= buffer.Capacity) throw new IOException("Cannot resolve Pack file identity.");
                return buffer.ToString();
            }

            [StructLayout(LayoutKind.Sequential)]
            private struct WindowsInfo
            {
                internal uint Attributes, CreationLow, CreationHigh, AccessLow, AccessHigh, WriteLow, WriteHigh;
                internal uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
            }
            [DllImport("kernel32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out WindowsInfo info);
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            private static extern uint GetFinalPathNameByHandle(SafeFileHandle handle, StringBuilder path, uint length, uint flags);
        }

        internal sealed class ResidentSelection
        {
            internal string Path, MissingPath;
            internal FileIdentity Identity;
            internal long ChangeTime;
        }

        internal sealed class ResidentGuards : IDisposable
        {
            private readonly List<SafeFileHandle> handles = new List<SafeFileHandle>();
            internal void Add(SafeFileHandle handle)
            {
                try { handles.Add(handle); }
                catch { handle.Dispose(); throw; }
            }
            public void Dispose()
            {
                foreach (var handle in handles) handle.Dispose();
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct BasicInfo
        {
            internal long CreationTime, AccessTime, WriteTime, ChangeTime;
            internal uint Attributes;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetFileInformationByHandleEx(SafeFileHandle handle, int infoClass,
            out BasicInfo info, uint length);

        private static long ReadChangeTime(SafeFileHandle handle)
        {
            if (!GetFileInformationByHandleEx(handle, 0, out BasicInfo info, (uint)Marshal.SizeOf<BasicInfo>()))
                throw new IOException("Cannot identify the change time of a resident Pack.");
            return info.ChangeTime;
        }

        [DllImport("cabinet.dll", ExactSpelling = true)]
        private static extern bool CreateDecompressor(uint algorithm, IntPtr allocation, out IntPtr handle);
        [DllImport("cabinet.dll", ExactSpelling = true, SetLastError = true)]
        private static extern unsafe bool Decompress(IntPtr handle, byte* input, UIntPtr inputLength,
            byte* output, UIntPtr outputLength, out UIntPtr written);
        [DllImport("cabinet.dll", ExactSpelling = true)]
        private static extern bool CloseDecompressor(IntPtr handle);
        [DllImport("bcrypt.dll", ExactSpelling = true)]
        private static extern unsafe int BCryptHash(IntPtr algorithm, IntPtr secret, uint secretLength,
            byte* input, uint length, byte* output, uint outputLength);

        private static unsafe VnResult ReadResident(SafeFileHandle handle, int size, ulong available,
            out byte[] bytes, out byte[] json)
        {
            bytes = json = null;
            uint* header = stackalloc uint[4];
            ReadExactly(handle, (byte*)header, HeaderBytes);
            uint format = header[0], length = header[1], meta = header[2], compressed = header[3];
            if ((format != 0x31505248 && format != 0x32505248) ||
                length == 0 || length > MaxArrayBytes || meta == 0 || compressed == 0 ||
                HeaderBytes + (ulong)meta + compressed + DigestBytes != (ulong)size)
                return VnResult.VN_BAD_PACK;
            if ((ulong)size + length + meta > available)
                return VnResult.VN_PACK_RESIDENT_CAPACITY;

            // The locked file's header and budget now cover every full-size array below.
            var image = new byte[size];
            fixed (byte* data = image)
            {
                Buffer.MemoryCopy(header, data, HeaderBytes, HeaderBytes);
                ReadExactly(handle, data + HeaderBytes, size - HeaderBytes);
                byte* digest = stackalloc byte[DigestBytes];
                int count = image.Length - DigestBytes;
                if (BCryptHash(new IntPtr(0x41), IntPtr.Zero, 0, data, (uint)count, digest, DigestBytes) != 0)
                    return VnResult.VN_DEVICE_ERROR;
                for (int i = 0; i < DigestBytes; ++i)
                    if (data[count + i] != digest[i]) return VnResult.VN_BAD_PACK;
                if (!CreateDecompressor(4, IntPtr.Zero, out IntPtr decoder)) return VnResult.VN_DEVICE_ERROR;
                try
                {
                    int packedOffset = HeaderBytes + (int)meta;
                    if (format == 0x32505248)
                    {
                        // Check the encoded block lengths before allocating the declared output.
                        if (!UnpackResidentBlocks(image, packedOffset, count, decoder, data, null, (int)length))
                            return VnResult.VN_BAD_PACK;
                    }
                    else if (Decompress(decoder, data + packedOffset, (UIntPtr)compressed,
                        null, UIntPtr.Zero, out UIntPtr needed) || Marshal.GetLastWin32Error() != 122 || needed.ToUInt64() != length)
                        return VnResult.VN_BAD_PACK;
                    bytes = new byte[length];
                    fixed (byte* target = bytes)
                    {
                        if (format == 0x32505248)
                        {
                            if (!UnpackResidentBlocks(image, packedOffset, count, decoder, data, target, (int)length))
                                return VnResult.VN_BAD_PACK;
                        }
                        else if (!Decompress(decoder, data + HeaderBytes + meta, (UIntPtr)compressed,
                            target, (UIntPtr)length, out UIntPtr written) || written.ToUInt64() != length)
                            return VnResult.VN_BAD_PACK;
                    }
                }
                finally { CloseDecompressor(decoder); }
            }
            json = new byte[meta];
            Buffer.BlockCopy(image, HeaderBytes, json, 0, (int)meta);
            return VnResult.VN_OK;
        }

        private static unsafe bool UnpackResidentBlocks(byte[] image, int offset, int end, IntPtr decoder,
            byte* data, byte* target, int length)
        {
            for (int output = 0; output < length;)
            {
                int expected = Math.Min(8192, length - output);
                if (end - offset < sizeof(uint)) return false;
                uint stored = BitConverter.ToUInt32(image, offset);
                offset += sizeof(uint);
                if (stored == 0 || stored > expected || stored > end - offset) return false;
                if (target != null)
                {
                    if (stored == expected)
                        Buffer.MemoryCopy(data + offset, target + output, expected, expected);
                    else if (!Decompress(decoder, data + offset, (UIntPtr)stored,
                        target + output, (UIntPtr)(uint)expected, out UIntPtr written) || written.ToUInt64() != (uint)expected)
                        return false;
                }
                offset += (int)stored;
                output += expected;
            }
            return offset == end;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern unsafe bool ReadFile(SafeFileHandle handle, byte* destination, uint size,
            out uint read, IntPtr overlapped);

        private static unsafe void ReadExactly(SafeFileHandle handle, byte* destination, int length)
        {
            while (length > 0)
            {
                if (!ReadFile(handle, destination, (uint)length, out uint count, IntPtr.Zero) || count == 0)
                    throw new IOException("Cannot read the complete resident Pack.");
                destination += count;
                length -= (int)count;
            }
        }

        private VnResult CaptureResident(string name, VnObjectId project, byte[] init, ulong retained, out PackMetadataInput input)
        {
            input = null;
            if (!IsWindows) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            string basename = name ?? "Init";
            string scope = name == null ? string.Empty : Language;
            string path = PathFor(basename, scope, ".hrp"), missing = null;
            SafeFileHandle handle = OpenForIdentity(path, FileShare.Read);
            if (handle == null && scope.Length != 0)
            {
                missing = path;
                scope = string.Empty;
                path = PathFor(basename, scope, ".hrp");
                handle = OpenForIdentity(path, FileShare.Read);
            }
            if (handle == null) return VnResult.VN_CONTENT_NOT_LOADED;
            using (handle)
            {
                FileIdentity identity = FileIdentity.ReadWindows(handle);
                if (identity.Length > MaxArrayBytes || identity.Length < HeaderBytes + DigestBytes) return VnResult.VN_BAD_PACK;
                ulong available = inputBudget == 0 ? ulong.MaxValue : inputBudget;
                ulong initLength = (ulong)(init?.Length ?? 0);
                if (initLength > available || retained > available - initLength)
                    return VnResult.VN_PACK_RESIDENT_CAPACITY;
                available -= initLength + retained;
                string actualPath = FileIdentity.WindowsPath(handle, path.Length);
                if (Path.GetFileName(actualPath) != basename + ".hrp" ||
                    (scope.Length != 0 && Path.GetFileName(Path.GetDirectoryName(actualPath)) != scope))
                    return VnResult.VN_BAD_PACK;
                var selection = new ResidentSelection { Path = path, MissingPath = missing, Identity = identity,
                    ChangeTime = ReadChangeTime(handle) };
                VnResult result = ReadResident(handle, (int)identity.Length, available, out byte[] bytes, out byte[] json);
                if (result != VnResult.VN_OK) return result;
                result = PackMetadata.Parse(json, bytes, name, scope, project, out PackMetadata names);
                if (result != VnResult.VN_OK) return result;
                result = PackMetadataNative.Inspect(bytes, init, names, out VnPackInspectReportV1 facts);
                if (result != VnResult.VN_OK) return result;
                names.ProjectId = facts.projectId;
                input = new PackMetadataInput { Bytes = bytes, Json = json, Names = names, Facts = facts, Selection = selection };
                return VnResult.VN_OK;
            }
        }

        internal VnResult LockPublication(ResidentSelection init, ICollection<PackMetadataInput> changes,
            out ResidentGuards guards)
        {
            guards = null;
            if (!resident) return VnResult.VN_OK;
            if (init == null) return VnResult.VN_PACK_STALE_REVISION;
            var acquired = new ResidentGuards();
            try
            {
                bool Hold(ResidentSelection item)
                {
                    if (item == null) return false;
                    var handle = OpenForIdentity(item.Path, FileShare.Read);
                    if (handle == null) return false;
                    acquired.Add(handle);
                    return item.Identity.Equals(FileIdentity.ReadWindows(handle)) && item.ChangeTime == ReadChangeTime(handle);
                }
                if (!Hold(init)) return VnResult.VN_PACK_STALE_REVISION;
                if (changes != null)
                {
                    foreach (var input in changes)
                        if (!Hold(input.Selection)) return VnResult.VN_PACK_STALE_REVISION;
                    foreach (var input in changes)
                        if (input.Selection.MissingPath != null && Exists(input.Selection.MissingPath))
                            return VnResult.VN_PACK_STALE_REVISION;
                }
                guards = acquired;
                return VnResult.VN_OK;
            }
            catch (IOException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
            catch (UnauthorizedAccessException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
            finally { if (guards == null) acquired.Dispose(); }
        }
    }
}
