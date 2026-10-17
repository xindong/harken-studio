/* Wire-format primitive implementations. See WireFormat.h for the
 * contracts; everything here is bounded byte movement with explicit
 * length validation. */
#include "harken_remote/WireFormat.h"

namespace harkenremote {

void EncodeFrameHeader(const WireFrameHeader& header, std::uint8_t* out) {
    std::memcpy(out + 0, &header.payloadBytes, 4);
    std::memcpy(out + 4, &header.msgType, 2);
    std::memcpy(out + 6, &header.flags, 2);
    std::memcpy(out + 8, &header.sequence, 4);
}

void DecodeFrameHeader(const std::uint8_t* in, WireFrameHeader* out) {
    std::memcpy(&out->payloadBytes, in + 0, 4);
    std::memcpy(&out->msgType, in + 4, 2);
    std::memcpy(&out->flags, in + 6, 2);
    std::memcpy(&out->sequence, in + 8, 4);
}

void AppendFrameBytes(const WireFrame& frame, std::vector<std::uint8_t>* out) {
    WireFrameHeader header;
    header.payloadBytes = static_cast<std::uint32_t>(frame.payload.size());
    header.msgType = frame.msgType;
    header.flags = frame.flags;
    header.sequence = frame.sequence;
    std::uint8_t bytes[kFrameHeaderBytes];
    EncodeFrameHeader(header, bytes);
    out->insert(out->end(), bytes, bytes + kFrameHeaderBytes);
    out->insert(out->end(), frame.payload.begin(), frame.payload.end());
}

bool ReadWireVec3(WireReader& reader, VnVec3* out) {
    return reader.ReadF32(&out->x) && reader.ReadF32(&out->y) &&
           reader.ReadF32(&out->z);
}

bool ReadWireString(WireReader& reader, WireArena& arena, const char** out) {
    *out = nullptr;
    std::uint32_t length = 0;
    if (!reader.ReadU32(&length)) {
        return false;
    }
    if (length == kWireNullLength) {
        return true; // NULL pointer sentinel
    }
    const std::uint8_t* bytes = reader.ReadBytes(length);
    if (bytes == nullptr) {
        return false;
    }
    char* copy = static_cast<char*>(arena.Alloc(static_cast<std::size_t>(length) + 1));
    std::memcpy(copy, bytes, length);
    copy[length] = '\0';
    *out = copy;
    return true;
}

bool ReadArraySection(WireReader& reader, WireArena& arena,
                      std::uint32_t count, std::uint32_t stride,
                      bool strideKnown, const void** out) {
    *out = nullptr;
    std::uint32_t byteLength = 0;
    if (!reader.ReadU32(&byteLength)) {
        return false;
    }
    if (byteLength == kWireNullLength) {
        return true; // NULL pointer sentinel; the engine refuses null arrays
    }
    if (strideKnown) {
        const std::uint64_t expected =
            static_cast<std::uint64_t>(count) * stride;
        if (byteLength != expected) {
            return false;
        }
    }
    const std::uint8_t* bytes = reader.ReadBytes(byteLength);
    if (bytes == nullptr) {
        return false;
    }
    void* copy = arena.Alloc(byteLength == 0 ? 1 : byteLength);
    std::memcpy(copy, bytes, byteLength);
    *out = copy;
    return true;
}

bool DecodeWirePolyDesc(WireReader& reader, WireArena& arena,
                        const VnEffectDesc** out) {
    *out = nullptr;
    std::uint32_t declared = 0;
    if (!reader.ReadU32(&declared)) {
        return false;
    }
    if (declared == kWireNullLength) {
        return true; // NULL desc; the engine refuses it as VN_INVALID_ARG
    }
    if (declared < sizeof(VnEffectDesc) || declared > kMaxFrameBytes) {
        return false; // the base header must at least exist
    }
    const std::uint8_t* raw = reader.ReadBytes(declared);
    if (raw == nullptr) {
        return false;
    }
    /* The struct's own vintage stamp and the wire byte count must agree,
     * or the image is internally inconsistent. */
    std::uint32_t stamped = 0;
    std::memcpy(&stamped, raw, sizeof(stamped));
    if (stamped != declared) {
        return false;
    }
    /* Zero-pad to the widest built-in descriptor: the engine validates the
     * declared size against the concrete type before reading fields, and
     * the padding guarantees even a misbehaving read stays inside owned,
     * zeroed storage. */
    std::size_t storage = declared;
    if (storage < sizeof(VnFeedbackDelayDesc)) {
        storage = sizeof(VnFeedbackDelayDesc);
    }
    if (storage < sizeof(VnThreeBandToneDesc)) {
        storage = sizeof(VnThreeBandToneDesc);
    }
    void* buf = arena.Alloc(storage);
    std::memcpy(buf, raw, declared);
    *out = static_cast<const VnEffectDesc*>(buf);
    return true;
}

void EncodeReply(WireFrame* reply, std::uint32_t sequence,
                 std::uint16_t opcode, VnResult result,
                 const std::uint64_t* outs, std::size_t outCount) {
    reply->msgType = kMsgReply;
    reply->flags = 0;
    reply->sequence = sequence;
    reply->payload.clear();
    WireWriter writer(&reply->payload);
    writer.U16(opcode);
    writer.U16(0);
    writer.I32(result);
    for (std::size_t i = 0; i < outCount; ++i) {
        writer.U64(outs[i]);
    }
}

void EncodeReplyWithRows(WireFrame* reply, std::uint32_t sequence,
                         std::uint16_t opcode, VnResult result,
                         const std::uint64_t* outs, std::size_t outCount,
                         const void* rows, std::size_t rowBytes) {
    EncodeReply(reply, sequence, opcode, result, outs, outCount);
    WireWriter writer(&reply->payload);
    writer.U32(static_cast<std::uint32_t>(rowBytes));
    if (rows != nullptr && rowBytes > 0) {
        writer.Bytes(rows, rowBytes);
    }
}

void EncodeWireError(WireFrame* reply, std::uint32_t sequence,
                     std::uint16_t wireError, std::uint16_t opcode) {
    reply->msgType = kMsgError;
    reply->flags = 0;
    reply->sequence = sequence;
    reply->payload.clear();
    WireWriter writer(&reply->payload);
    writer.U16(wireError);
    writer.U16(opcode);
}

} // namespace harkenremote
