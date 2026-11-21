using System;
using System.IO;
using System.IO.Compression;

namespace Harken
{
    // The publishing tool only wraps members up to this size. Larger canonical Packs pass through.
    internal static class PackStorage
    {
        internal const int MaxGzipMemberBytes = 16 * 1024 * 1024;

        internal static byte[] ReadPlain(Stream stream)
        {
            if (stream.Length > int.MaxValue) throw new IOException("Pack input exceeds the managed byte limit.");
            return ReadExactly(stream, (int)stream.Length);
        }

        internal static byte[] CaptureBinary(byte[] bytes)
        {
            if (bytes.Length < 2 || bytes[0] != 0x1f || bytes[1] != 0x8b) return (byte[])bytes.Clone();
            using (var stream = new MemoryStream(bytes, false)) return Inflate(stream);
        }

        internal static byte[] ReadBinary(Stream file)
        {
            bool compressed = file.ReadByte() == 0x1f && file.ReadByte() == 0x8b;
            file.Position = 0;
            return compressed ? Inflate(file) : ReadPlain(file);
        }

        internal sealed class InvalidMember : IOException
        {
            internal InvalidMember(Exception cause) : base("Invalid compressed Pack.", cause) { }
        }

        private static byte[] Inflate(Stream stream)
        {
            try { return ReadGzipMember(stream); }
            catch (InvalidDataException error) { throw new InvalidMember(error); }
            catch (EndOfStreamException error) { throw new InvalidMember(error); }
        }

        private static byte[] ReadGzipMember(Stream stream)
        {
            // The writer emits one gzip member only when it is smaller than its canonical input.
            if (stream.Length < 18 || stream.Length > MaxGzipMemberBytes)
                throw new InvalidDataException("Invalid compressed Pack size.");
            stream.Position = stream.Length - 4;
            uint length = (uint)(stream.ReadByte() | (stream.ReadByte() << 8) |
                (stream.ReadByte() << 16) | (stream.ReadByte() << 24));
            if (length <= stream.Length || length > MaxGzipMemberBytes)
                throw new InvalidDataException("Invalid expanded Pack size.");
            stream.Position = 0;
            using (var gzip = new GZipStream(stream, CompressionMode.Decompress, true))
            {
                byte[] result = ReadExactly(gzip, (int)length);
                if (gzip.ReadByte() != -1) throw new InvalidDataException("Expanded Pack exceeds its declared size.");
                return result;
            }
        }

        internal static byte[] ReadExactly(Stream stream, int length)
        {
            var bytes = new byte[length];
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
