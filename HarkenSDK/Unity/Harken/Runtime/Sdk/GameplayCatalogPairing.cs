using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Harken
{
    internal static class GameplayCatalogPairing
    {
        private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);

        internal static VnResult Validate(GameplayPublication publication, byte[] bytes, out string error)
        {
            error = null;
            if (bytes == null || bytes.Length == 0)
                return Fail(VnResult.VN_CONTENT_NOT_LOADED, "Init bytes are missing.", out error);
            if (!PackManifest.VerifyPackBytes(bytes, publication.InitPack))
                return Fail(VnResult.VN_PACK_HASH_MISMATCH, "Init bytes do not match the selected manifest hash.", out error);
            try
            {
                using (var reader = new BinaryReader(new MemoryStream(bytes, false), Utf8))
                {
                    Check(bytes.Length >= 44 && reader.ReadUInt32() == 0x4b505356, "Invalid VSPK header.");
                    if (reader.ReadUInt32() != 4)
                        return Fail(VnResult.VN_PACK_INCOMPATIBLE, "Gameplay metadata requires Pack format 4.", out error);
                    Check(reader.ReadUInt32() == 44 && reader.ReadUInt64() == (ulong)bytes.Length &&
                        reader.ReadUInt32() == 2, "The selected bytes are not a complete Init Pack.");
                    uint count = reader.ReadUInt32();
                    ulong directory = reader.ReadUInt64();
                    ulong directoryHash = reader.ReadUInt64();
                    ulong directorySize = (ulong)count * 56;
                    Check(directory >= 44 && Range(directory, directorySize, bytes.Length), "Invalid Pack directory bounds.");
                    Check(Hash(bytes, (int)directory, (int)directorySize) == directoryHash, "Pack directory checksum mismatch.");
                    bool requirement = false, identity = false;
                    for (uint i = 0; i < count; ++i)
                    {
                        reader.BaseStream.Position = (long)(directory + i * 56UL);
                        uint kind = reader.ReadUInt32();
                        reader.ReadUInt32();
                        reader.ReadBytes(16);
                        ulong offset = reader.ReadUInt64(), size = reader.ReadUInt64();
                        ulong checksum = reader.ReadUInt64();
                        Check(offset >= directory + directorySize && (offset & 7) == 0 &&
                            Range(offset, size, bytes.Length), "Invalid Pack payload bounds.");
                        Check(Hash(bytes, (int)offset, (int)size) == checksum, "Pack payload checksum mismatch.");
                        if (kind == 22)
                        {
                            Check(!requirement && size >= 112, "Invalid Init requirement block.");
                            requirement = true;
                            reader.BaseStream.Position = (long)offset + 8;
                            if (reader.ReadUInt64() != publication.InitPackStableId ||
                                reader.ReadUInt64() != publication.InitContentRevision)
                                return Fail(VnResult.VN_PACK_STALE_REVISION, "Init and JSON base identities differ.", out error);
                        }
                        if (kind == 28 || (kind == 25 && !identity))
                        {
                            Check(size >= 64, "Invalid Init project identity.");
                            reader.BaseStream.Position = (long)offset + 8;
                            if (ReadGuid(reader) != publication.ProjectId)
                                return Fail(VnResult.VN_GAMEPLAY_WRONG_PROJECT, "Init and JSON belong to different projects.", out error);
                            identity = true;
                        }
                    }
                    Check(requirement && identity, "Init base identity is missing.");
                    return Encode(publication, out _, out error);
                }
            }
            catch (InvalidDataException exception) { return Fail(VnResult.VN_BAD_PACK, exception.Message, out error); }
            catch (EndOfStreamException exception) { return Fail(VnResult.VN_BAD_PACK, exception.Message, out error); }
            catch (ArgumentException exception) { return Fail(VnResult.VN_BAD_PACK, exception.Message, out error); }
        }

        internal static VnResult Encode(GameplayPublication publication, out byte[] payload, out string error)
        {
            payload = null;
            error = null;
            var active = new List<GameplayAction>(publication.Actions);
            var retired = new List<RetiredGameplayAction>(publication.RetiredActions);
            active.Sort((a, b) => CompareNames(a.Name, b.Name));
            retired.Sort((a, b) => CompareNames(a.Name, b.Name));
            int poolSize = 0;
            foreach (var row in active) poolSize += Utf8.GetByteCount(row.Name);
            foreach (var row in retired) poolSize += Utf8.GetByteCount(row.Name);
            int pool = 96 + active.Count * 72 + retired.Count * 40;
            byte[] bytes = new byte[pool + ((poolSize + 7) & ~7)];
            if (bytes.Length > 1048576)
                return Fail(VnResult.VN_PACK_CATALOG_CAPACITY, "Gameplay catalog exceeds its byte limit.", out error);
            using (var writer = new BinaryWriter(new MemoryStream(bytes, true), Utf8))
            {
                writer.Write(1U); writer.Write(1U); writer.Write(GuidBytes(publication.ProjectId));
                writer.Write(0UL);
                writer.Write((uint)active.Count); writer.Write((uint)retired.Count);
                writer.Write((uint)(bytes.Length - pool)); writer.Write(0U);
                writer.Write(0UL); writer.Write(0UL);
                writer.Write(publication.InitPackStableId); writer.Write(publication.InitContentRevision);
                writer.Write(retired.Count == 0 ? 0U : 1U); writer.Write(0U); writer.Write(0UL);
                uint cursor = 0;
                foreach (var row in active)
                {
                    int start = (int)writer.BaseStream.Position;
                    byte[] name = Utf8.GetBytes(row.Name);
                    writer.Write(GuidBytes(row.ActionStableId.ToString()));
                    writer.Write(row.NameBindingRevision); writer.Write(row.CallableFingerprint);
                    writer.Write(row.OwnerPackStableId); writer.Write(row.ProviderClosureFingerprint);
                    writer.Write(cursor); writer.Write((ushort)name.Length); writer.Write((ushort)0);
                    writer.Write(0UL); writer.Write(Hash(bytes, start, 64));
                    Buffer.BlockCopy(name, 0, bytes, pool + (int)cursor, name.Length);
                    cursor += (uint)name.Length;
                }
                foreach (var row in retired)
                {
                    byte[] name = Utf8.GetBytes(row.Name);
                    writer.Write(GuidBytes(row.LastActionStableId.ToString()));
                    writer.Write(row.LastNameBindingRevision); writer.Write(row.Reason);
                    writer.Write((ushort)name.Length); writer.Write((ushort)0);
                    writer.Write(cursor); writer.Write(0U);
                    Buffer.BlockCopy(name, 0, bytes, pool + (int)cursor, name.Length);
                    cursor += (uint)name.Length;
                }
                ulong digest = Hash(bytes, 0, bytes.Length);
                if (digest != publication.Digest)
                    return Fail(VnResult.VN_PACK_STALE_REVISION, "JSON gameplay catalog digest mismatch.", out error);
                writer.BaseStream.Position = 24; writer.Write(digest);
                ulong pairing = Hash(bytes, 8, 16);
                pairing = Hash(bytes, 64, 16, pairing);
                pairing = Hash(bytes, 24, 8, pairing);
                writer.BaseStream.Position = 56; writer.Write(pairing);
                writer.BaseStream.Position = 88; writer.Write(Hash(bytes, 0, 88));
            }
            payload = bytes;
            return VnResult.VN_OK;
        }

        private static int CompareNames(string a, string b)
        {
            byte[] left = Utf8.GetBytes(a), right = Utf8.GetBytes(b);
            for (int i = 0; i < Math.Min(left.Length, right.Length); ++i)
                if (left[i] != right[i]) return left[i].CompareTo(right[i]);
            return left.Length.CompareTo(right.Length);
        }

        private static byte[] GuidBytes(string id)
        {
            byte[] bytes = Guid.Parse(id).ToByteArray();
            Array.Reverse(bytes, 0, 4); Array.Reverse(bytes, 4, 2); Array.Reverse(bytes, 6, 2);
            return bytes;
        }

        private static string ReadGuid(BinaryReader reader)
        {
            byte[] bytes = reader.ReadBytes(16);
            Check(bytes.Length == 16, "Truncated gameplay GUID.");
            Array.Reverse(bytes, 0, 4); Array.Reverse(bytes, 4, 2); Array.Reverse(bytes, 6, 2);
            return new Guid(bytes).ToString("D");
        }

        private static bool Range(ulong offset, ulong count, int length)
            => offset <= (ulong)length && count <= (ulong)length - offset;
        private static ulong Hash(byte[] bytes, int offset, int length, ulong seed = 14695981039346656037UL)
            => PackManifest.HashBytes(bytes.AsSpan(offset, length), seed);
        private static void Check(bool condition, string error)
        {
            if (!condition) throw new InvalidDataException(error);
        }
        private static VnResult Fail(VnResult result, string message, out string error)
        {
            error = message;
            return result;
        }
    }
}
