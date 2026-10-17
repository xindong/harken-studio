/* Wire-format primitives for the Harken remote-control channel: the frame
 * header, bounded payload reader/writer, the decode arena that owns
 * out-of-line desc storage, and the shared encoders for replies and wire
 * errors. Pure byte-level code: no transport, no engine calls — the layer
 * unit tests exercise it without a pipe.
 *
 * All wire integers are little-endian. The only supported target is
 * Windows x64 (little-endian), so scalar transcoding is a bounded memcpy;
 * the framing layer still never trusts a length field without checking it
 * against the bytes actually present.
 */
#ifndef HARKEN_REMOTE_WIRE_FORMAT_H
#define HARKEN_REMOTE_WIRE_FORMAT_H

#include <harken/Harken.h>

#include "harken_wire_ids.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace harkenremote {

/* One decoded frame header. On the wire: u32 payloadBytes, u16 msgType,
 * u16 flags, u32 sequence — kFrameHeaderBytes total. */
struct WireFrameHeader {
    std::uint32_t payloadBytes = 0;
    std::uint16_t msgType = 0;
    std::uint16_t flags = 0;
    std::uint32_t sequence = 0;
};

/* One frame as routed between threads: header facts plus an owned payload. */
struct WireFrame {
    std::uint16_t msgType = 0;
    std::uint16_t flags = 0;
    std::uint32_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

void EncodeFrameHeader(const WireFrameHeader& header,
                       std::uint8_t out[/* kFrameHeaderBytes */]);

/* Decodes kFrameHeaderBytes into *out. Always succeeds structurally (the
 * caller guarantees the byte count); the caller checks payloadBytes against
 * kMaxFrameBytes — an oversized length prefix is a session-close condition,
 * not a per-frame error. */
void DecodeFrameHeader(const std::uint8_t in[/* kFrameHeaderBytes */],
                       WireFrameHeader* out);

/* Serializes a frame into header + payload bytes, appended to `out`. */
void AppendFrameBytes(const WireFrame& frame, std::vector<std::uint8_t>* out);

/* Bounded cursor over one payload. Every read checks the remaining byte
 * count; a short payload fails the read instead of walking off the end. */
class WireReader {
public:
    WireReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool ReadU16(std::uint16_t* out) { return ReadScalar(out); }
    bool ReadU32(std::uint32_t* out) { return ReadScalar(out); }
    bool ReadU64(std::uint64_t* out) { return ReadScalar(out); }
    bool ReadObjectId(VnObjectId* out) {
        const auto* bytes = ReadBytes(16);
        if (bytes == nullptr) return false;
        *out = harken::ObjectIdFromBytes(std::span<const std::uint8_t, 16>(bytes, 16));
        return true;
    }
    bool ReadI32(std::int32_t* out) { return ReadScalar(out); }
    bool ReadF32(float* out) { return ReadScalar(out); }
    bool ReadF64(double* out) { return ReadScalar(out); }

    /* Returns the current cursor and advances, or nullptr when fewer than
     * `bytes` remain. */
    const std::uint8_t* ReadBytes(std::size_t bytes) {
        if (size_ - pos_ < bytes) {
            return nullptr;
        }
        const std::uint8_t* at = data_ + pos_;
        pos_ += bytes;
        return at;
    }

    bool AtEnd() const { return pos_ == size_; }
    std::size_t Remaining() const { return size_ - pos_; }

private:
    template <typename T>
    bool ReadScalar(T* out) {
        if (size_ - pos_ < sizeof(T)) {
            return false;
        }
        std::memcpy(out, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

/* Appends little-endian scalars and sections to a payload vector. */
class WireWriter {
public:
    explicit WireWriter(std::vector<std::uint8_t>* out) : out_(out) {}

    void U16(std::uint16_t v) { Append(&v, sizeof(v)); }
    void U32(std::uint32_t v) { Append(&v, sizeof(v)); }
    void U64(std::uint64_t v) { Append(&v, sizeof(v)); }
    void ObjectId(VnObjectId value) {
        const auto bytes = harken::ObjectIdBytes(value);
        Append(bytes.data(), bytes.size());
    }
    void I32(std::int32_t v) { Append(&v, sizeof(v)); }
    void F32(float v) { Append(&v, sizeof(v)); }
    void F64(double v) { Append(&v, sizeof(v)); }
    void Bytes(const void* data, std::size_t size) { Append(data, size); }

    /* Length-prefixed UTF-8 string: kWireNullLength encodes a NULL pointer,
     * 0 an empty string; no NUL travels on the wire. */
    void String(const char* utf8) {
        if (utf8 == nullptr) {
            U32(kWireNullLength);
            return;
        }
        const std::size_t len = std::strlen(utf8);
        U32(static_cast<std::uint32_t>(len));
        Append(utf8, len);
    }

private:
    void Append(const void* data, std::size_t size) {
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        out_->insert(out_->end(), bytes, bytes + size);
    }

    std::vector<std::uint8_t>* out_;
};

/* Owns the out-of-line storage one command decode produces: desc images,
 * child/step arrays, NUL-terminated strings. Allocations are zero-filled
 * and 16-byte aligned; everything is released when the arena goes out of
 * scope at the end of the dispatch. */
class WireArena {
public:
    void* Alloc(std::size_t bytes) {
        blocks_.emplace_back(bytes + kAlign, std::uint8_t{0});
        std::uint8_t* base = blocks_.back().data();
        const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(base);
        const std::uintptr_t aligned = (raw + kAlign - 1) & ~(kAlign - 1);
        return reinterpret_cast<void*>(aligned);
    }

private:
    static constexpr std::uintptr_t kAlign = 16;
    std::deque<std::vector<std::uint8_t>> blocks_;
};

/* Reads a VnVec3 as three f32. */
bool ReadWireVec3(WireReader& reader, VnVec3* out);

/* Reads a length-prefixed string into arena storage, NUL-terminated.
 * kWireNullLength yields a NULL pointer (the encoding's NULL sentinel).
 * Always assigns *out. */
bool ReadWireString(WireReader& reader, WireArena& arena, const char** out);

/* Desc-embedded string section: identical encoding to a string param. */
inline bool ReadStringSection(WireReader& reader, WireArena& arena,
                              const char** out) {
    return ReadWireString(reader, arena, out);
}

/* Desc-embedded array section: u32 byteLength + bytes. kWireNullLength
 * yields a NULL pointer. When the stride is known, byteLength must equal
 * count * stride exactly; an unknown stride vintage value passes the bytes
 * through for the engine's own validation to refuse. Always assigns *out. */
bool ReadArraySection(WireReader& reader, WireArena& arena,
                      std::uint32_t count, std::uint32_t stride,
                      bool strideKnown, const void** out);

/* Polymorphic effect desc (Vn_SetVoiceEffect / Vn_SetBusEffect): u32
 * declared structSize + that many raw bytes. The image is zero-padded to
 * the largest built-in effect descriptor so the engine's own
 * structSize-vs-type validation can never read past real storage. */
bool DecodeWirePolyDesc(WireReader& reader, WireArena& arena,
                        const VnEffectDesc** out);

/* Fills a Reply frame: u16 opcode, u16 reserved, i32 VnResult, then each
 * out param as u64. */
void EncodeReply(WireFrame* reply, std::uint32_t sequence,
                 std::uint16_t opcode, VnResult result,
                 const std::uint64_t* outs, std::size_t outCount);

/* EncodeReply plus one trailing array section (u32 byteLength + raw row
 * structs) — the outrows reply form of the capture mirrors. rows may be
 * NULL when rowBytes is 0. */
void EncodeReplyWithRows(WireFrame* reply, std::uint32_t sequence,
                         std::uint16_t opcode, VnResult result,
                         const std::uint64_t* outs, std::size_t outCount,
                         const void* rows, std::size_t rowBytes);

/* Fills an Error frame: u16 wireError, u16 opcode (0 when no opcode is
 * attributable). */
void EncodeWireError(WireFrame* reply, std::uint32_t sequence,
                     std::uint16_t wireError, std::uint16_t opcode);

} // namespace harkenremote

#endif // HARKEN_REMOTE_WIRE_FORMAT_H
