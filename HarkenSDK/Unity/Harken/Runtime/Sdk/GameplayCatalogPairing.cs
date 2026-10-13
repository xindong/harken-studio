using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Harken
{
    internal static class GameplayCatalogPairing
    {
        private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
        private static readonly byte[] Zeros = new byte[8];

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
                    int payload = -1, payloadSize = 0;
                    for (uint i = 0; i < count; ++i)
                    {
                        reader.BaseStream.Position = (long)(directory + i * 56UL);
                        uint kind = reader.ReadUInt32(), vintage = reader.ReadUInt32();
                        VnObjectId id = VnObjectId.Parse(ReadGuid(reader));
                        ulong offset = reader.ReadUInt64(), size = reader.ReadUInt64();
                        ulong checksum = reader.ReadUInt64();
                        ulong ordinal = reader.ReadUInt64();
                        Check(offset >= directory + directorySize && (offset & 7) == 0 &&
                            Range(offset, size, bytes.Length), "Invalid Pack payload bounds.");
                        if (kind != 25) continue;
                        Check(payload == -1 && id.IsNull && ordinal == 0, "Gameplay catalog must occur exactly once with ID zero.");
                        if (vintage != 1)
                            return Fail(VnResult.VN_PACK_INCOMPATIBLE, "Unsupported gameplay catalog vintage.", out error);
                        if (size > 1048576)
                            return Fail(VnResult.VN_PACK_CATALOG_CAPACITY, "Gameplay catalog exceeds its byte limit.", out error);
                        payload = (int)offset; payloadSize = (int)size;
                        Check(size >= 96 && Hash(bytes, payload, payloadSize) == checksum, "Gameplay payload checksum mismatch.");
                    }
                    if (payload < 0)
                        return Fail(VnResult.VN_CAPABILITY_UNAVAILABLE, "Init does not contain kind25 gameplay metadata.", out error);
                    reader.BaseStream.Position = payload;
                    if (reader.ReadUInt32() != 1 || reader.ReadUInt32() != 1)
                        return Fail(VnResult.VN_PACK_INCOMPATIBLE, "Unsupported gameplay catalog schema.", out error);
                    string project = ReadGuid(reader);
                    ulong digest = reader.ReadUInt64();
                    uint active = reader.ReadUInt32(), retired = reader.ReadUInt32(), poolBytes = reader.ReadUInt32();
                    Check(reader.ReadUInt32() == 0, "Gameplay header reserved field is nonzero.");
                    reader.ReadUInt64();
                    ulong pairing = reader.ReadUInt64(), initId = reader.ReadUInt64(), revision = reader.ReadUInt64();
                    uint flags = reader.ReadUInt32();
                    Check(reader.ReadUInt32() == 0 && reader.ReadUInt64() == Hash(bytes, payload, 88),
                        "Gameplay header checksum mismatch.");
                    if (active > 4096 || retired > 4096)
                        return Fail(VnResult.VN_PACK_CATALOG_CAPACITY, "Gameplay catalog exceeds its row limit.", out error);
                    Check(96UL + active * 72UL + retired * 40UL + poolBytes == (ulong)payloadSize &&
                        (poolBytes & 7) == 0 && flags == (retired == 0 ? 0U : 1U), "Invalid gameplay catalog layout.");
                    ulong actualDigest = Hash(bytes, payload, 24);
                    actualDigest = PackManifest.HashBytes(Zeros, actualDigest);
                    actualDigest = Hash(bytes, payload + 32, 24, actualDigest);
                    actualDigest = PackManifest.HashBytes(Zeros, actualDigest);
                    actualDigest = Hash(bytes, payload + 64, 24, actualDigest);
                    actualDigest = PackManifest.HashBytes(Zeros, actualDigest);
                    actualDigest = Hash(bytes, payload + 96, payloadSize - 96, actualDigest);
                    ulong actualPairing = Hash(bytes, payload + 8, 16);
                    actualPairing = Hash(bytes, payload + 64, 16, actualPairing);
                    actualPairing = Hash(bytes, payload + 24, 8, actualPairing);
                    Check(digest == actualDigest && pairing == actualPairing, "Gameplay digest or pairing checksum mismatch.");
                    if (project != publication.ProjectId)
                        return Fail(VnResult.VN_GAMEPLAY_WRONG_PROJECT, "Init and JSON belong to different projects.", out error);
                    if (digest != publication.Digest || initId != publication.InitPackStableId ||
                        revision != publication.InitContentRevision || active != publication.Actions.Count ||
                        retired != publication.RetiredActions.Count)
                        return Fail(VnResult.VN_PACK_STALE_REVISION, "Init and JSON publication identities differ.", out error);
                    int pool = payload + 96 + (int)active * 72 + (int)retired * 40;
                    uint cursor = 0;
                    var names = new HashSet<string>(StringComparer.Ordinal);
                    var ids = new HashSet<VnObjectId>();
                    for (int i = 0; i < active; ++i)
                    {
                        int start = payload + 96 + i * 72;
                        reader.BaseStream.Position = start;
                        VnObjectId id = VnObjectId.Parse(ReadGuid(reader));
                        ulong binding = reader.ReadUInt64(), callable = reader.ReadUInt64();
                        ulong owner = reader.ReadUInt64(), closure = reader.ReadUInt64();
                        uint offset = reader.ReadUInt32();
                        ushort length = reader.ReadUInt16(), rowFlags = reader.ReadUInt16();
                        Check(!id.IsNull && ids.Add(id) && owner != 0 && binding != 0 && rowFlags == 0 &&
                            reader.ReadUInt64() == 0 && reader.ReadUInt64() == Hash(bytes, start, 64), "Invalid gameplay action row.");
                        string name = ReadName(bytes, pool, poolBytes, offset, length, ref cursor);
                        Check(names.Add(name), "Duplicate gameplay name.");
                        if (!publication.TryFindAction(name, out GameplayAction row) || row.ActionStableId != id ||
                            row.NameBindingRevision != binding || row.CallableFingerprint != callable ||
                            row.OwnerPackStableId != owner || row.ProviderClosureFingerprint != closure)
                            return Fail(VnResult.VN_PACK_STALE_REVISION, "Init and JSON action metadata differ for '" + name + "'.", out error);
                    }
                    for (int i = 0; i < retired; ++i)
                    {
                        reader.BaseStream.Position = payload + 96 + (int)active * 72 + i * 40;
                        VnObjectId id = VnObjectId.Parse(ReadGuid(reader));
                        ulong binding = reader.ReadUInt64();
                        uint reason = reader.ReadUInt32();
                        ushort length = reader.ReadUInt16();
                        Check(!id.IsNull && reader.ReadUInt16() == 0, "Invalid retired gameplay row.");
                        uint offset = reader.ReadUInt32();
                        Check(reader.ReadUInt32() == 0 && reason >= 1 && reason <= 3, "Invalid retirement reason or reserved field.");
                        string name = ReadName(bytes, pool, poolBytes, offset, length, ref cursor);
                        Check(names.Add(name), "Duplicate gameplay name.");
                        if (!publication.TryFindRetired(name, out RetiredGameplayAction row) || row.LastActionStableId != id ||
                            row.LastNameBindingRevision != binding || row.Reason != reason)
                            return Fail(VnResult.VN_PACK_STALE_REVISION, "Init and JSON retired metadata differ for '" + name + "'.", out error);
                    }
                    Check(((cursor + 7U) & ~7U) == poolBytes, "Invalid gameplay name pool size.");
                    for (uint i = cursor; i < poolBytes; ++i)
                        Check(bytes[pool + i] == 0, "Nonzero gameplay name padding.");
                    return VnResult.VN_OK;
                }
            }
            catch (InvalidDataException exception) { return Fail(VnResult.VN_BAD_PACK, exception.Message, out error); }
            catch (EndOfStreamException exception) { return Fail(VnResult.VN_BAD_PACK, exception.Message, out error); }
            catch (ArgumentException exception) { return Fail(VnResult.VN_BAD_PACK, exception.Message, out error); }
        }

        private static string ReadName(byte[] bytes, int pool, uint poolSize, uint offset, ushort length, ref uint cursor)
        {
            Check(offset == cursor && offset <= poolSize && length > 0 && length <= 128 && length <= poolSize - offset,
                "Invalid gameplay name bounds.");
            string name = Utf8.GetString(bytes, pool + (int)offset, length);
            Check(name.IndexOf('\0') < 0, "Gameplay name contains NUL.");
            cursor += length;
            return name;
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
