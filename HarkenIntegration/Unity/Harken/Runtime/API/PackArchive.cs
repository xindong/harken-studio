using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

namespace Harken
{
    // One immutable publication, held open until its owning session shuts down.
    internal sealed class PackArchive : IDisposable
    {
        internal const string FileName = "Packs.zip";
        private readonly FileStream file;
        private readonly ZipArchive archive;
        private readonly Dictionary<string, ZipArchiveEntry> members =
            new Dictionary<string, ZipArchiveEntry>(StringComparer.Ordinal);

        internal string Root => Path.GetDirectoryName(file.Name);

        internal PackArchive(string path)
        {
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("A Pack archive must be a regular file.");
            file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
            try
            {
                archive = new ZipArchive(file, ZipArchiveMode.Read, true, Encoding.UTF8);
                foreach (ZipArchiveEntry entry in archive.Entries)
                {
                    if (!PackPaths.IsSafeRelativePath(entry.FullName) || members.ContainsKey(entry.FullName))
                        throw new InvalidDataException("Invalid or duplicated Pack archive member.");
                    members.Add(entry.FullName, entry);
                }
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        private static readonly uint[] CrcTable = CreateCrcTable();

        private static uint[] CreateCrcTable()
        {
            var table = new uint[256];
            for (uint i = 0; i < table.Length; ++i)
            {
                uint value = i;
                for (int bit = 0; bit < 8; ++bit)
                    value = (value >> 1) ^ ((value & 1) == 0 ? 0u : 0xedb88320u);
                table[i] = value;
            }
            return table;
        }

        private static uint Crc32(byte[] bytes)
        {
            uint crc = uint.MaxValue;
            foreach (byte value in bytes)
                crc = CrcTable[(byte)(crc ^ value)] ^ (crc >> 8);
            return ~crc;
        }

        internal byte[] Read(string path)
        {
            if (!members.TryGetValue(path, out ZipArchiveEntry entry)) return null;
            try
            {
                long length = entry.Length;
                // Large members may not grow beyond their physical input size.
                if (entry.CompressedLength > file.Length || length < 0 || length > int.MaxValue ||
                    (length > PackStorage.MaxGzipMemberBytes && length > entry.CompressedLength))
                    throw new InvalidDataException("Invalid Pack archive member size.");
                using (Stream input = entry.Open())
                {
                    byte[] bytes = PackStorage.ReadExactly(input, (int)length);
                    if (input.ReadByte() != -1) throw new InvalidDataException("Pack archive member exceeds its declared size.");
                    if (Crc32(bytes) != entry.Crc32) throw new InvalidDataException("Pack archive member checksum mismatch.");
                    return bytes;
                }
            }
            catch (InvalidDataException e) { throw new PackStorage.InvalidMember(e); }
            catch (EndOfStreamException e) { throw new PackStorage.InvalidMember(e); }
        }

        public void Dispose()
        {
            try { archive?.Dispose(); }
            finally { file.Dispose(); }
        }
    }
}
