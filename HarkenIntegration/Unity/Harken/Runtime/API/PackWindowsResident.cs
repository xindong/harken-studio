using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Harken
{
    internal sealed partial class PackDirectorySource
    {
        private readonly bool resident;
        private const int HeaderBytes = 16;
        private const int DigestBytes = 32;
        private const int MaxPart = PackStorage.MaxGzipMemberBytes;

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
        [DllImport("cabinet.dll", ExactSpelling = true)]
        private static extern unsafe bool Decompress(IntPtr handle, byte* input, UIntPtr inputLength,
            byte* output, UIntPtr outputLength, out UIntPtr written);
        [DllImport("cabinet.dll", ExactSpelling = true)]
        private static extern bool CloseDecompressor(IntPtr handle);
        [DllImport("bcrypt.dll", ExactSpelling = true)]
        private static extern unsafe int BCryptHash(IntPtr algorithm, IntPtr secret, uint secretLength,
            byte* input, uint length, byte* output, uint outputLength);

        private static unsafe VnResult Unpack(byte[] image, out byte[] bytes, out byte[] json)
        {
            bytes = json = null;
            if (image.Length < HeaderBytes + DigestBytes || BitConverter.ToUInt32(image, 0) != 0x31505248)
                return VnResult.VN_BAD_PACK;
            uint length = BitConverter.ToUInt32(image, 4), meta = BitConverter.ToUInt32(image, 8);
            uint compressed = BitConverter.ToUInt32(image, 12);
            if (length == 0 || length > MaxPart || meta == 0 || meta > MaxPart || compressed == 0 ||
                HeaderBytes + (ulong)meta + compressed + DigestBytes != (ulong)image.Length)
                return VnResult.VN_BAD_PACK;
            fixed (byte* data = image)
            {
                byte* digest = stackalloc byte[DigestBytes];
                int count = image.Length - DigestBytes;
                if (BCryptHash(new IntPtr(0x41), IntPtr.Zero, 0, data, (uint)count, digest, DigestBytes) != 0)
                    return VnResult.VN_DEVICE_ERROR;
                for (int i = 0; i < DigestBytes; ++i)
                    if (data[count + i] != digest[i]) return VnResult.VN_BAD_PACK;
                bytes = new byte[length];
                if (!CreateDecompressor(4, IntPtr.Zero, out IntPtr decoder)) return VnResult.VN_DEVICE_ERROR;
                try
                {
                    fixed (byte* target = bytes)
                        if (!Decompress(decoder, data + HeaderBytes + meta, (UIntPtr)compressed,
                            target, (UIntPtr)length, out UIntPtr written) || written.ToUInt64() != length)
                            return VnResult.VN_BAD_PACK;
                }
                finally { CloseDecompressor(decoder); }
            }
            json = new byte[meta];
            Buffer.BlockCopy(image, HeaderBytes, json, 0, (int)meta);
            return VnResult.VN_OK;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern unsafe bool ReadFile(SafeFileHandle handle, byte* destination, uint size,
            out uint read, IntPtr overlapped);

        private static unsafe byte[] ReadImage(SafeFileHandle handle, int size)
        {
            var image = new byte[size];
            fixed (byte* start = image)
            {
                int offset = 0;
                while (offset < size)
                {
                    if (!ReadFile(handle, start + offset, (uint)(size - offset), out uint count, IntPtr.Zero) || count == 0)
                        throw new IOException("Cannot read the complete resident Pack.");
                    offset += (int)count;
                }
            }
            return image;
        }

        private VnResult CaptureResident(string name, VnObjectId project, byte[] init, out CompanionInput input)
        {
            input = null;
            if (!IsWindows) return VnResult.VN_CAPABILITY_UNAVAILABLE;
            if (Publishing()) return VnResult.VN_PACK_STALE_REVISION;
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
            if (handle == null) return Publishing() ? VnResult.VN_PACK_STALE_REVISION : VnResult.VN_CONTENT_NOT_LOADED;
            using (handle)
            {
                FileIdentity identity = FileIdentity.ReadWindows(handle);
                if (identity.Length > 3 * MaxPart || identity.Length < HeaderBytes + DigestBytes) return VnResult.VN_BAD_PACK;
                string actualPath = FileIdentity.WindowsPath(handle, path.Length);
                if (Path.GetFileName(actualPath) != basename + ".hrp" ||
                    (scope.Length != 0 && Path.GetFileName(Path.GetDirectoryName(actualPath)) != scope))
                    return VnResult.VN_BAD_PACK;
                var selection = new ResidentSelection { Path = path, MissingPath = missing, Identity = identity,
                    ChangeTime = ReadChangeTime(handle) };
                VnResult result = Unpack(ReadImage(handle, (int)identity.Length), out byte[] bytes, out byte[] json);
                if (result != VnResult.VN_OK) return result;
                result = PackCompanion.Parse(json, bytes, name, scope, project, out PackCompanion names);
                if (result != VnResult.VN_OK) return result;
                result = CompanionNative.Inspect(bytes, init, names, out VnPackInspectReportV1 facts);
                if (result != VnResult.VN_OK) return result;
                names.ProjectId = facts.projectId;
                input = new CompanionInput { Bytes = bytes, Json = json, Names = names, Facts = facts, Selection = selection };
                return VnResult.VN_OK;
            }
        }

        internal VnResult LockPublication(ResidentSelection init, ICollection<CompanionInput> changes,
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
                if (Publishing()) return VnResult.VN_PACK_STALE_REVISION;
                guards = acquired;
                return VnResult.VN_OK;
            }
            catch (IOException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
            catch (UnauthorizedAccessException) { return VnResult.VN_PACK_PROVIDER_FAILED; }
            finally { if (guards == null) acquired.Dispose(); }
        }
    }
}
