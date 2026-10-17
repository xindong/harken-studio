#ifndef HARKEN_API_MONITOR_IDENTITY_CODEC_H
#define HARKEN_API_MONITOR_IDENTITY_CODEC_H

#include "MonitorExecutionCodec.h"

namespace harken::monitor {

inline constexpr std::uint64_t kIdentityDeltaProducer = 5;
inline constexpr std::uint64_t kIdentityBaselineProducer = 6;
inline constexpr std::uint64_t kIdentityDeltaStream = 203;
inline constexpr std::uint64_t kIdentityBaselineStream = 204;
inline constexpr std::uint32_t kIdentityPayloadBytes = 456;
inline constexpr std::uint32_t kIdentityRecordBytes = PackedRecordBytes(kIdentityPayloadBytes);
inline constexpr std::uint32_t kIdentityBeginBytes = 136;
inline constexpr std::uint32_t kIdentityEndBytes = 104;
static_assert(sizeof(VnMonitorIdentityFactV1) == kIdentityPayloadBytes);
static_assert(sizeof(VnMonitorIdentityCutBeginV1) == kIdentityBeginBytes);
static_assert(sizeof(VnMonitorIdentityCutEndV1) == kIdentityEndBytes);
static_assert(kIdentityPayloadBytes <= 512 && kIdentityRecordBytes <= 832);

// Decode the unchanged packed envelope; validation remains record-specific.
inline bool ReadIdentityHeader(const std::uint8_t* bytes, std::uint64_t size,
    VnMonitorRecordHeader& header) noexcept {
    if (bytes == nullptr || size < kPackedEnvelopeBytes) return false;
    header = {};
    header.structSize = sizeof(header);
    header.telemetry.structSize = sizeof(header.telemetry);
    ExecutionPayloadReader read{bytes};
    read(header.recordKind);
    read(header.recordVersion);
    read(header.flags);
    read(header.streamKind);
    read(header.recordBytes);
    read(header.streamId);
    read(header.producerId);
    read(header.sourceSequence);
    read(header.deliverySequence);
    read(header.engineMonitorInstanceId);
    read(header.monitorSessionEpoch);
    read(header.worldGeneration);
    read(header.capturePlanGeneration);
    read(header.renderPlanGeneration);
    read(header.transportEpoch);
    read(header.deviceEpoch);
    read(header.clockAnchorGeneration);
    read(header.measurementSegmentGeneration);
    read(header.executeFrame);
    read(header.graphFrame);
    read(header.devicePresentationEstimate);
    read(header.deviceUncertaintyFrames);
    read(header.entityKind);
    read(header.programSemanticFingerprint);
    read(header.runtimeLineageFingerprint);
    read(header.optionalSymbolFingerprint);
    read(header.stableId);
    read(header.instanceHandle);
    read(header.instanceGeneration);
    read(header.actionInstanceHandle);
    read(header.playableInstanceHandle);
    read(header.parentActionInstanceHandle);
    read(header.traceCorrelationId);
    read(header.reason);
    read(header.validityFlags);
    read(header.payloadBytes);
    read(header.telemetry.reserved0);
    read(header.telemetry.arrangingInstance);
    read(header.telemetry.clipId);
    read(header.telemetry.loopIteration);
    read(header.telemetry.occurrenceOrdinal);
    read(header.telemetry.logicCommitSequence);
    read(header.telemetry.executeFrame);
    return read.offset == kPackedEnvelopeBytes && header.recordBytes == size &&
        header.payloadBytes <= size - kPackedEnvelopeBytes;
}

template<class Visitor, class Fact>
inline void VisitIdentityPayload(Visitor& visit, Fact& value) noexcept {
    visit(value.structSize); visit(value.reserved0);
    visit(value.stateCutId); visit(value.identityRevision);
    VisitExecutionPayload(visit, value.fact);
}

template<class Visitor, class Begin>
inline void VisitIdentityBegin(Visitor& visit, Begin& value) noexcept {
    visit(value.structSize); visit(value.reserved0); visit(value.stateCutId);
    visit(value.identityRevision); visit(value.deltaProducerId);
    visit(value.deltaSourceSequence); visit(value.symbolProducerId);
    visit(value.symbolSourceSequence); visit(value.capacityLimit);
    for (auto& count : value.namespaceTrueCount) visit(count);
}

template<class Visitor, class End>
inline void VisitIdentityEnd(Visitor& visit, End& value) noexcept {
    visit(value.structSize); visit(value.result); visit(value.stateCutId);
    visit(value.identityRevision); visit(value.checksum);
    for (auto& count : value.namespaceEmittedCount) visit(count);
}

inline bool IdentityNamespace(unsigned subject) noexcept {
    return subject == VN_MONITOR_SUBJECT_PLAYABLE || subject == VN_MONITOR_SUBJECT_ACTION ||
        subject == VN_MONITOR_SUBJECT_LOGICAL_VOICE || subject == VN_MONITOR_SUBJECT_EMITTER;
}

inline bool ValidateIdentityHeader(const VnMonitorRecordHeader& header,
    unsigned kind, unsigned payloadBytes, bool baseline) noexcept {
    return header.structSize == sizeof(header) && header.recordKind == kind && header.recordVersion == 1 &&
        header.flags == VN_MONITOR_RECORD_FLAG_OPTIONAL && header.streamKind == VN_MONITOR_STREAM_STATE &&
        header.payloadBytes == payloadBytes && header.recordBytes == PackedRecordBytes(payloadBytes) &&
        header.engineMonitorInstanceId != 0 && header.sourceSequence != 0 &&
        header.producerId == (baseline ? kIdentityBaselineProducer : kIdentityDeltaProducer) &&
        header.streamId == (baseline ? kIdentityBaselineStream : kIdentityDeltaStream) &&
        header.reserved0 == 0 && header.telemetry.reserved0 == 0;
}

inline bool ValidateIdentityFact(const VnMonitorRecordHeader& header,
    const VnMonitorIdentityFactV1& value) noexcept {
    const auto& fact = value.fact;
    return ValidateIdentityHeader(header, VN_MONITOR_RECORD_IDENTITY_FACT,
            kIdentityPayloadBytes, value.stateCutId != 0) &&
        value.structSize == kIdentityPayloadBytes && value.reserved0 == 0 && value.identityRevision != 0 &&
        (fact.factKind == VN_MONITOR_FACT_IDENTITY_UPSERT ||
            (value.stateCutId == 0 && fact.factKind == VN_MONITOR_FACT_IDENTITY_TOMBSTONE)) &&
        IdentityNamespace(fact.subjectNamespace) && FactHas(fact, VN_MONITOR_FACT_FIELD_SUBJECT) &&
        ValidateFactFields(header, fact);
}

inline std::uint32_t PackIdentityFact(std::uint8_t* out, std::uint32_t capacity,
    const MonitorExecutionFact& source, std::uint64_t cut, std::uint64_t revision) noexcept {
    if (out == nullptr || capacity < kIdentityRecordBytes || source.BodyKind() != MonitorFactBody::Execution) return 0;
    VnMonitorRecordHeader header{};
    VnMonitorIdentityFactV1 value{};
    if (!ProjectFactFields(source, cut != 0 ? kIdentityBaselineProducer : kIdentityDeltaProducer,
            cut != 0 ? kIdentityBaselineStream : kIdentityDeltaStream, header, value.fact)) return 0;
    header.recordKind = VN_MONITOR_RECORD_IDENTITY_FACT;
    header.recordVersion = 1;
    header.flags = VN_MONITOR_RECORD_FLAG_OPTIONAL;
    header.recordBytes = kIdentityRecordBytes;
    header.payloadBytes = kIdentityPayloadBytes;
    value.structSize = sizeof(value);
    value.stateCutId = cut;
    value.identityRevision = revision;
    if (!ValidateIdentityFact(header, value)) return 0;
    std::memset(out, 0, kIdentityRecordBytes);
    PackHeader(out, header);
    ExecutionPayloadWriter writer{out + kPackedEnvelopeBytes};
    VisitIdentityPayload(writer, value);
    return writer.offset == kIdentityPayloadBytes ? kIdentityRecordBytes : 0;
}

inline bool DecodeIdentityFact(const VnMonitorRecordHeader& header, const std::uint8_t* bytes,
    std::uint64_t size, VnMonitorIdentityFactV1& out) noexcept {
    if (bytes == nullptr || size != kIdentityPayloadBytes) return false;
    VnMonitorIdentityFactV1 value{};
    ExecutionPayloadReader reader{bytes};
    VisitIdentityPayload(reader, value);
    if (reader.offset != size || !ValidateIdentityFact(header, value)) return false;
    out = value;
    return true;
}

inline bool ValidateIdentityCounts(const std::uint64_t (&counts)[9], std::uint64_t capacity) noexcept {
    std::uint64_t total = 0;
    for (unsigned i = 0; i != 9; ++i) {
        if ((!IdentityNamespace(i) && i != VN_MONITOR_SUBJECT_HOST_SYMBOL && counts[i] != 0) || counts[i] > capacity - total) return false;
        total += counts[i];
    }
    return true;
}

inline VnMonitorRecordHeader IdentityCutHeader(std::uint64_t engine, std::uint64_t sequence,
    std::uint64_t epoch, unsigned kind, unsigned payloadBytes) noexcept {
    VnMonitorRecordHeader header{};
    header.structSize = sizeof(header);
    header.recordKind = kind;
    header.recordVersion = 1;
    header.flags = VN_MONITOR_RECORD_FLAG_OPTIONAL;
    header.streamKind = VN_MONITOR_STREAM_STATE;
    header.recordBytes = PackedRecordBytes(payloadBytes);
    header.payloadBytes = payloadBytes;
    header.streamId = kIdentityBaselineStream;
    header.producerId = kIdentityBaselineProducer;
    header.engineMonitorInstanceId = engine;
    header.sourceSequence = sequence;
    header.monitorSessionEpoch = epoch;
    header.telemetry.structSize = sizeof(header.telemetry);
    return header;
}

inline bool ValidateIdentityControlHeader(const VnMonitorRecordHeader& header,
    unsigned kind, unsigned payloadBytes) noexcept {
    if (!ValidateIdentityHeader(header, kind, payloadBytes, true)) return false;
    auto expected = IdentityCutHeader(header.engineMonitorInstanceId, header.sourceSequence,
        header.monitorSessionEpoch, kind, payloadBytes);
    expected.deliverySequence = header.deliverySequence;
    std::uint8_t actualBytes[kPackedEnvelopeBytes]{}, expectedBytes[kPackedEnvelopeBytes]{};
    PackHeader(actualBytes, header);
    PackHeader(expectedBytes, expected);
    return std::memcmp(actualBytes, expectedBytes, kPackedEnvelopeBytes) == 0;
}

inline bool ValidateIdentityBegin(const VnMonitorRecordHeader& header,
    const VnMonitorIdentityCutBeginV1& value) noexcept {
    return ValidateIdentityControlHeader(header, VN_MONITOR_RECORD_IDENTITY_CUT_BEGIN, kIdentityBeginBytes) &&
        value.structSize == kIdentityBeginBytes && value.reserved0 == 0 && value.stateCutId != 0 &&
        value.identityRevision != 0 && value.deltaProducerId == kIdentityDeltaProducer &&
        (value.symbolProducerId == 7 || (value.symbolProducerId == 0 && value.symbolSourceSequence == 0 &&
            value.namespaceTrueCount[VN_MONITOR_SUBJECT_HOST_SYMBOL] == 0)) &&
        value.namespaceTrueCount[VN_MONITOR_SUBJECT_HOST_SYMBOL] % 3 == 0 &&
        ValidateIdentityCounts(value.namespaceTrueCount, value.capacityLimit);
}

inline bool ValidateIdentityEnd(const VnMonitorRecordHeader& header,
    const VnMonitorIdentityCutEndV1& value) noexcept {
    return ValidateIdentityControlHeader(header, VN_MONITOR_RECORD_IDENTITY_CUT_END, kIdentityEndBytes) &&
        value.structSize == kIdentityEndBytes && value.stateCutId != 0 && value.identityRevision != 0 &&
        (value.result == VN_MONITOR_STATE_CUT_COMPLETE || value.result == VN_MONITOR_STATE_CUT_TRUNCATED ||
            value.result == VN_MONITOR_STATE_CUT_ABORTED) &&
        ValidateIdentityCounts(value.namespaceEmittedCount, UINT64_MAX);
}

inline std::uint32_t PackIdentityBegin(std::uint8_t* out, std::uint32_t capacity,
    const VnMonitorRecordHeader& header, const VnMonitorIdentityCutBeginV1& value) noexcept {
    const auto size = PackedRecordBytes(kIdentityBeginBytes);
    if (out == nullptr || capacity < size || !ValidateIdentityBegin(header, value)) return 0;
    std::memset(out, 0, size);
    PackHeader(out, header);
    ExecutionPayloadWriter writer{out + kPackedEnvelopeBytes};
    VisitIdentityBegin(writer, value);
    return writer.offset == kIdentityBeginBytes ? size : 0;
}

inline std::uint32_t PackIdentityEnd(std::uint8_t* out, std::uint32_t capacity,
    const VnMonitorRecordHeader& header, const VnMonitorIdentityCutEndV1& value) noexcept {
    const auto size = PackedRecordBytes(kIdentityEndBytes);
    if (out == nullptr || capacity < size || !ValidateIdentityEnd(header, value)) return 0;
    std::memset(out, 0, size);
    PackHeader(out, header);
    ExecutionPayloadWriter writer{out + kPackedEnvelopeBytes};
    VisitIdentityEnd(writer, value);
    return writer.offset == kIdentityEndBytes ? size : 0;
}

inline bool DecodeIdentityBegin(const VnMonitorRecordHeader& header, const std::uint8_t* bytes,
    std::uint64_t size, VnMonitorIdentityCutBeginV1& out) noexcept {
    if (bytes == nullptr || size != kIdentityBeginBytes) return false;
    VnMonitorIdentityCutBeginV1 value{};
    ExecutionPayloadReader reader{bytes};
    VisitIdentityBegin(reader, value);
    if (reader.offset != size || !ValidateIdentityBegin(header, value)) return false;
    out = value;
    return true;
}

inline bool DecodeIdentityEnd(const VnMonitorRecordHeader& header, const std::uint8_t* bytes,
    std::uint64_t size, VnMonitorIdentityCutEndV1& out) noexcept {
    if (bytes == nullptr || size != kIdentityEndBytes) return false;
    VnMonitorIdentityCutEndV1 value{};
    ExecutionPayloadReader reader{bytes};
    VisitIdentityEnd(reader, value);
    if (reader.offset != size || !ValidateIdentityEnd(header, value)) return false;
    out = value;
    return true;
}

// Hash canonical source bytes before session/delivery stamping. Native padding
// and consumer-local deliverySequence never contribute to the identity digest.
inline std::uint64_t IdentityChecksum(std::uint64_t checksum, const std::uint8_t* bytes,
    std::uint32_t size) noexcept {
    for (std::uint32_t i = 0; i != size; ++i) {
        checksum ^= bytes[i];
        checksum *= UINT64_C(1099511628211);
    }
    return checksum;
}
inline constexpr std::uint64_t kIdentityChecksumSeed = UINT64_C(14695981039346656037);

} // namespace harken::monitor

#endif
