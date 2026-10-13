#include "monitor_fixture_decode.h"

#include <cstring>

/* Decoder compile unit must stay a harness pure function: only Harken.h. */

namespace {

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64(const uint8_t* p) {
    return static_cast<uint64_t>(ReadU32(p)) |
           (static_cast<uint64_t>(ReadU32(p + 4)) << 32);
}

constexpr uint32_t kPackedHeaderBytes = VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;

bool PackedSizePinned() {
    return VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES == 316u;
}

VnResult ReadPackedHeader(const uint8_t* p, VnMonitorRecordHeader* out) {
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(VnMonitorRecordHeader);
    size_t o = 0;
    out->recordKind = ReadU32(p + o); o += 4;
    out->recordVersion = ReadU32(p + o); o += 4;
    out->flags = ReadU32(p + o); o += 4;
    out->streamKind = ReadU32(p + o); o += 4;
    out->recordBytes = ReadU64(p + o); o += 8;
    out->streamId = ReadU64(p + o); o += 8;
    out->producerId = ReadU64(p + o); o += 8;
    out->sourceSequence = ReadU64(p + o); o += 8;
    out->deliverySequence = ReadU64(p + o); o += 8;
    out->engineMonitorInstanceId = ReadU64(p + o); o += 8;
    out->monitorSessionEpoch = ReadU64(p + o); o += 8;
    out->worldGeneration = ReadU64(p + o); o += 8;
    out->capturePlanGeneration = ReadU64(p + o); o += 8;
    out->renderPlanGeneration = ReadU64(p + o); o += 8;
    out->transportEpoch = ReadU64(p + o); o += 8;
    out->deviceEpoch = ReadU64(p + o); o += 8;
    out->clockAnchorGeneration = ReadU64(p + o); o += 8;
    out->measurementSegmentGeneration = ReadU64(p + o); o += 8;
    out->executeFrame = ReadU64(p + o); o += 8;
    out->graphFrame = ReadU64(p + o); o += 8;
    out->devicePresentationEstimate = ReadU64(p + o); o += 8;
    out->deviceUncertaintyFrames = ReadU32(p + o); o += 4;
    out->entityKind = ReadU32(p + o); o += 4;
    out->programSemanticFingerprint = ReadU64(p + o); o += 8;
    out->runtimeLineageFingerprint = ReadU64(p + o); o += 8;
    out->optionalSymbolFingerprint = ReadU64(p + o); o += 8;
    out->stableId = harken::ObjectIdFromBytes(std::span<const uint8_t, 16>(p + o, 16)); o += 16;
    out->instanceHandle = ReadU64(p + o); o += 8;
    out->instanceGeneration = ReadU64(p + o); o += 8;
    out->actionInstanceHandle = ReadU64(p + o); o += 8;
    out->playableInstanceHandle = ReadU64(p + o); o += 8;
    out->parentActionInstanceHandle = ReadU64(p + o); o += 8;
    out->traceCorrelationId = ReadU64(p + o); o += 8;
    out->reason = ReadU32(p + o); o += 4;
    out->validityFlags = ReadU32(p + o); o += 4;
    out->payloadBytes = ReadU64(p + o); o += 8;
    out->telemetry.structSize = sizeof(out->telemetry);
    out->telemetry.reserved0 = ReadU32(p + o); o += 4;
    out->telemetry.arrangingInstance = ReadU64(p + o); o += 8;
    out->telemetry.clipId = harken::ObjectIdFromBytes(std::span<const uint8_t, 16>(p + o, 16)); o += 16;
    out->telemetry.loopIteration = ReadU32(p + o); o += 4;
    out->telemetry.occurrenceOrdinal = ReadU32(p + o); o += 4;
    out->telemetry.logicCommitSequence = ReadU64(p + o); o += 8;
    out->telemetry.executeFrame = ReadU64(p + o); o += 8;
    if (o != 316u)
        return VN_MONITOR_INCOMPATIBLE;
    return VN_OK;
}

bool PaddingZero(const uint8_t* p, uint64_t from, uint64_t to) {
    for (uint64_t i = from; i < to; ++i) {
        if (p[i] != 0)
            return false;
    }
    return true;
}

uint32_t NumericReadable(uint32_t flags) {
    const uint32_t armed = VN_MONITOR_VALID_ARMED;
    const uint32_t numeric = VN_MONITOR_VALID_NUMERIC;
    const uint32_t warmup = VN_MONITOR_VALID_WARMUP;
    const uint32_t unsupported = VN_MONITOR_VALID_UNSUPPORTED;
    if ((flags & numeric) == 0)
        return 0;
    if ((flags & armed) == 0)
        return 0;
    if ((flags & unsupported) != 0)
        return 0;
    if ((flags & warmup) != 0)
        return 0;
    return 1;
}

} // namespace

uint32_t vn_monitor_crc32c(const void* data, size_t length) {
    static const uint32_t kPoly = 0x82F63B78u;
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (kPoly & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

VnMonitorDecodeResult vn_monitor_decode_records(
    const uint8_t* input,
    uint64_t inputBytes,
    VnMonitorDecodedRecord* outRecords,
    uint32_t outCapacity,
    uint8_t* skipBuffer,
    uint64_t skipCapacity) {
    VnMonitorDecodeResult result{};
    result.status = VN_OK;
    if (!PackedSizePinned()) {
        result.status = VN_MONITOR_INCOMPATIBLE;
        return result;
    }
    if (input == nullptr && inputBytes != 0) {
        result.status = VN_INVALID_ARG;
        return result;
    }
    if (outCapacity > 0 && outRecords == nullptr) {
        result.status = VN_INVALID_ARG;
        return result;
    }

    uint64_t offset = 0;
    uint64_t skipUsed = 0;
    while (offset < inputBytes) {
        if ((offset & 7ull) != 0) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.nextRecordBytes = kPackedHeaderBytes;
            return result;
        }
        const uint64_t remaining = inputBytes - offset;
        if (remaining < kPackedHeaderBytes) {
            result.nextRecordBytes = kPackedHeaderBytes;
            if (result.recordsWritten == 0 && remaining > 0)
                result.status = VN_OK;
            return result;
        }
        VnMonitorRecordHeader header{};
        VnResult hr = ReadPackedHeader(input + offset, &header);
        if (hr != VN_OK) {
            result.status = hr;
            result.nextRecordBytes = kPackedHeaderBytes;
            return result;
        }
        if ((header.recordBytes & 7ull) != 0 ||
            header.recordBytes < kPackedHeaderBytes ||
            header.payloadBytes > header.recordBytes - kPackedHeaderBytes) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.nextRecordBytes = header.recordBytes;
            return result;
        }
        if (header.recordBytes > remaining) {
            result.nextRecordBytes = header.recordBytes;
            return result;
        }
        const uint64_t padFrom = kPackedHeaderBytes + header.payloadBytes;
        if (padFrom > header.recordBytes) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.nextRecordBytes = header.recordBytes;
            return result;
        }
        if (!PaddingZero(input + offset, padFrom, header.recordBytes)) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.nextRecordBytes = header.recordBytes;
            return result;
        }
        if (header.sourceSequence == 0 || header.deliverySequence != 0) {
            result.status = VN_INVALID_ARG;
            result.nextRecordBytes = header.recordBytes;
            return result;
        }

        const uint32_t required = header.flags & VN_MONITOR_RECORD_FLAG_REQUIRED;
        const uint32_t optional = header.flags & VN_MONITOR_RECORD_FLAG_OPTIONAL;
        if ((required == 0 && optional == 0) || (required != 0 && optional != 0)) {
            result.status = VN_INVALID_ARG;
            result.nextRecordBytes = header.recordBytes;
            return result;
        }
        const uint32_t reservedFlags =
            header.flags & ~(VN_MONITOR_RECORD_FLAG_REQUIRED |
                             VN_MONITOR_RECORD_FLAG_OPTIONAL);
        const bool unknownKind =
            header.recordKind == VN_MONITOR_RECORD_UNKNOWN ||
            header.recordKind >= VN_MONITOR_RECORD_INVALID;
        const bool unknownVersion = header.recordVersion != 1u;
        const bool unknown = unknownKind || unknownVersion || reservedFlags != 0;

        if (unknown && required) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.nextRecordBytes = header.recordBytes;
            return result;
        }
        if (unknown && optional) {
            if (skipCapacity - skipUsed < header.recordBytes) {
                result.status = VN_MONITOR_BUFFER_TOO_SMALL;
                result.nextRecordBytes = header.recordBytes;
                return result;
            }
            if (skipBuffer != nullptr)
                std::memcpy(skipBuffer + skipUsed, input + offset,
                            static_cast<size_t>(header.recordBytes));
            if (result.recordsWritten >= outCapacity) {
                result.status = VN_MONITOR_BUFFER_TOO_SMALL;
                result.nextRecordBytes = header.recordBytes;
                return result;
            }
            VnMonitorDecodedRecord rec{};
            rec.header = header;
            rec.skippedBytes = skipBuffer + skipUsed;
            rec.skippedBytesCount = header.recordBytes;
            rec.payload = nullptr;
            rec.payloadBytes = 0;
            rec.numericReadable = 0;
            outRecords[result.recordsWritten] = rec;
            result.recordsWritten += 1;
            skipUsed += header.recordBytes;
            offset += header.recordBytes;
            result.bytesConsumed = offset;
            continue;
        }

        if (header.recordKind == VN_MONITOR_RECORD_UNKNOWN ||
            header.recordKind >= VN_MONITOR_RECORD_INVALID) {
            result.status = VN_MONITOR_INCOMPATIBLE;
            result.nextRecordBytes = header.recordBytes;
            return result;
        }
        if (result.recordsWritten >= outCapacity) {
            result.status = VN_MONITOR_BUFFER_TOO_SMALL;
            result.nextRecordBytes = header.recordBytes;
            return result;
        }
        VnMonitorDecodedRecord rec{};
        rec.header = header;
        rec.payload = input + offset + kPackedHeaderBytes;
        rec.payloadBytes = header.payloadBytes;
        rec.numericReadable = NumericReadable(header.validityFlags);
        rec.skippedBytes = nullptr;
        rec.skippedBytesCount = 0;
        outRecords[result.recordsWritten] = rec;
        result.recordsWritten += 1;
        offset += header.recordBytes;
        result.bytesConsumed = offset;
    }
    result.bytesConsumed = offset;
    return result;
}
