#ifndef HARKEN_API_MONITOR_HOST_SYMBOL_CODEC_H
#define HARKEN_API_MONITOR_HOST_SYMBOL_CODEC_H

#include "MonitorHostSymbol.h"
#include "MonitorIdentityCodec.h"

namespace harken::monitor {

inline constexpr std::uint32_t kHostSymbolPayloadBytes = 312;
inline constexpr std::uint32_t kHostSymbolRecordBytes = PackedRecordBytes(kHostSymbolPayloadBytes);
static_assert(sizeof(VnMonitorHostObjectSymbolV1) == kHostSymbolPayloadBytes);
static_assert(kHostSymbolRecordBytes == 632);

template<class Visitor, class Value>
inline void VisitHostSymbol(Visitor& visit, Value& value) noexcept {
    visit(value.structSize); visit(value.factKind); visit(value.stateCutId); visit(value.cutRevision);
    visit(value.hostSession); visit(value.hostSequence); visit(value.registrationRevision);
    visit(value.field); visit(value.textBytes); visit(value.flags);
    for (auto& byte : value.text) visit(byte);
}

inline VnMonitorRecordHeader HostSymbolHeader(const MonitorHostSymbolSource& source,
    std::uint64_t cut, std::uint32_t streamKind) noexcept {
    auto header = IdentityCutHeader(source.engine, source.sourceSequence, 0,
        VN_MONITOR_RECORD_HOST_OBJECT_SYMBOL, kHostSymbolPayloadBytes);
    header.streamKind = streamKind;
    if (cut == 0) {
        header.producerId = kHostSymbolProducer;
        header.streamId = kHostSymbolStream;
    }
    // The record kind fixes the namespace; the legacy enum has no Emitter.
    header.entityKind = VN_MONITOR_ENTITY_NONE;
    header.instanceHandle = source.value.emitter;
    header.instanceGeneration = source.value.emitter >> 32;
    return header;
}

inline bool ValidateHostSymbol(const VnMonitorRecordHeader& header,
    const VnMonitorHostObjectSymbolV1& value) noexcept {
    const auto limit = value.field == VN_MONITOR_HOST_SYMBOL_NAME ? 64u :
        (value.field == VN_MONITOR_HOST_SYMBOL_PATH ? 256u : (value.field == VN_MONITOR_HOST_SYMBOL_SCENE ? 128u : 0u));
    if (value.structSize != kHostSymbolPayloadBytes || value.cutRevision == 0 ||
        value.hostSession == 0 || value.hostSequence == 0 || value.registrationRevision == 0 ||
        limit == 0 || value.textBytes > limit || (value.flags & ~1u) != 0 ||
        !ValidMonitorUtf8(value.text, value.textBytes) ||
        (value.factKind != VN_MONITOR_SYMBOL_UPSERT &&
            (value.factKind != VN_MONITOR_SYMBOL_TOMBSTONE || value.stateCutId != 0))) return false;
    for (unsigned i = value.textBytes; i != sizeof(value.text); ++i)
        if (value.text[i] != 0) return false;
    if (header.structSize != sizeof(header) || header.reserved0 != 0 ||
        header.engineMonitorInstanceId == 0 || header.sourceSequence == 0 ||
        header.instanceGeneration == 0 || header.instanceGeneration != header.instanceHandle >> 32 ||
        (header.streamKind != VN_MONITOR_STREAM_STATE &&
            (value.stateCutId != 0 || header.streamKind != VN_MONITOR_STREAM_EVENT))) return false;
    MonitorHostSymbolSource source{};
    source.engine = header.engineMonitorInstanceId;
    source.sourceSequence = header.sourceSequence;
    source.value.emitter = header.instanceHandle;
    auto expected = HostSymbolHeader(source, value.stateCutId, header.streamKind);
    expected.monitorSessionEpoch = header.monitorSessionEpoch;
    expected.deliverySequence = header.deliverySequence;
    std::uint8_t actualBytes[kPackedEnvelopeBytes]{}, expectedBytes[kPackedEnvelopeBytes]{};
    PackHeader(actualBytes, header);
    PackHeader(expectedBytes, expected);
    return std::memcmp(actualBytes, expectedBytes, sizeof(actualBytes)) == 0;
}

inline std::uint32_t PackHostSymbol(std::uint8_t* out, std::uint32_t capacity,
    const MonitorHostSymbolSource& source, std::uint64_t cut, std::uint32_t streamKind) noexcept {
    if (out == nullptr || capacity < kHostSymbolRecordBytes || source.value.textBytes > sizeof(source.value.text)) return 0;
    const auto header = HostSymbolHeader(source, cut, streamKind);
    VnMonitorHostObjectSymbolV1 value{};
    value.structSize = sizeof(value);
    value.factKind = source.value.factKind;
    value.stateCutId = cut;
    value.cutRevision = source.boundaryRevision;
    value.hostSession = source.value.sdkSession;
    value.hostSequence = source.value.localSequence;
    value.registrationRevision = source.value.registrationRevision;
    value.field = source.value.field;
    value.textBytes = source.value.textBytes;
    value.flags = source.value.flags;
    std::memcpy(value.text, source.value.text, value.textBytes);
    if (!ValidateHostSymbol(header, value)) return 0;
    std::memset(out, 0, kHostSymbolRecordBytes);
    PackHeader(out, header);
    ExecutionPayloadWriter writer{out + kPackedEnvelopeBytes};
    VisitHostSymbol(writer, value);
    return writer.offset == kHostSymbolPayloadBytes ? kHostSymbolRecordBytes : 0;
}

inline bool DecodeHostSymbol(const VnMonitorRecordHeader& header, const std::uint8_t* bytes,
    std::uint64_t size, VnMonitorHostObjectSymbolV1& out) noexcept {
    if (bytes == nullptr || size != kHostSymbolPayloadBytes) return false;
    VnMonitorHostObjectSymbolV1 value{};
    ExecutionPayloadReader reader{bytes};
    VisitHostSymbol(reader, value);
    if (reader.offset != size || !ValidateHostSymbol(header, value)) return false;
    out = value;
    return true;
}

inline std::uint64_t HostSymbolRowChecksum(std::uint64_t checksum,
    VnMonitorRecordHeader header, const VnMonitorHostObjectSymbolV1& value) noexcept {
    header.monitorSessionEpoch = header.deliverySequence = 0;
    std::uint8_t bytes[kHostSymbolRecordBytes]{};
    PackHeader(bytes, header);
    ExecutionPayloadWriter writer{bytes + kPackedEnvelopeBytes};
    VisitHostSymbol(writer, value);
    return IdentityChecksum(checksum, bytes, sizeof(bytes));
}

} // namespace harken::monitor
#endif
