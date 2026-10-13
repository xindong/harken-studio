#include "MonitorDecoder.h"
#include "Monitor/MonitorTypes.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace VNX {
namespace Monitor {
namespace {

constexpr uint32_t kPacked = kMonitorRecordEnvelopePackedBytes;

uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const uint8_t* p)
{
    return static_cast<uint64_t>(readU32(p)) |
           (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

bool addOverflow(uint64_t a, uint64_t b, uint64_t* out)
{
    if (a > std::numeric_limits<uint64_t>::max() - b)
        return true;
    *out = a + b;
    return false;
}

bool paddingZero(const uint8_t* p, uint64_t from, uint64_t to)
{
    for (uint64_t i = from; i < to; ++i)
    {
        if (p[i] != 0)
            return false;
    }
    return true;
}

MonitorResult readPackedHeader(const uint8_t* p, MonitorRecordHeader* out)
{
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(MonitorRecordHeader);
    size_t o = 0;
    out->recordKind = readU32(p + o); o += 4;
    out->recordVersion = readU32(p + o); o += 4;
    out->flags = readU32(p + o); o += 4;
    out->streamKind = readU32(p + o); o += 4;
    out->recordBytes = readU64(p + o); o += 8;
    out->streamId = readU64(p + o); o += 8;
    out->producerId = readU64(p + o); o += 8;
    out->sourceSequence = readU64(p + o); o += 8;
    out->deliverySequence = readU64(p + o); o += 8;
    out->engineMonitorInstanceId = readU64(p + o); o += 8;
    out->monitorSessionEpoch = readU64(p + o); o += 8;
    out->worldGeneration = readU64(p + o); o += 8;
    out->capturePlanGeneration = readU64(p + o); o += 8;
    out->renderPlanGeneration = readU64(p + o); o += 8;
    out->transportEpoch = readU64(p + o); o += 8;
    out->deviceEpoch = readU64(p + o); o += 8;
    out->clockAnchorGeneration = readU64(p + o); o += 8;
    out->measurementSegmentGeneration = readU64(p + o); o += 8;
    out->executeFrame = readU64(p + o); o += 8;
    out->graphFrame = readU64(p + o); o += 8;
    out->devicePresentationEstimate = readU64(p + o); o += 8;
    out->deviceUncertaintyFrames = readU32(p + o); o += 4;
    out->entityKind = readU32(p + o); o += 4;
    out->programSemanticFingerprint = readU64(p + o); o += 8;
    out->runtimeLineageFingerprint = readU64(p + o); o += 8;
    out->optionalSymbolFingerprint = readU64(p + o); o += 8;
    out->stableId = harkenstudio::ObjectIdFromBytes(std::span<const uint8_t, 16>(p + o, 16)); o += 16;
    out->instanceHandle = readU64(p + o); o += 8;
    out->instanceGeneration = readU64(p + o); o += 8;
    out->actionInstanceHandle = readU64(p + o); o += 8;
    out->playableInstanceHandle = readU64(p + o); o += 8;
    out->parentActionInstanceHandle = readU64(p + o); o += 8;
    out->traceCorrelationId = readU64(p + o); o += 8;
    out->reason = readU32(p + o); o += 4;
    out->validityFlags = readU32(p + o); o += 4;
    out->payloadBytes = readU64(p + o); o += 8;
    out->telemetry.structSize = sizeof(out->telemetry);
    out->telemetry.reserved0 = readU32(p + o); o += 4;
    out->telemetry.arrangingInstance = readU64(p + o); o += 8;
    out->telemetry.clipId = harkenstudio::ObjectIdFromBytes(std::span<const uint8_t, 16>(p + o, 16)); o += 16;
    out->telemetry.loopIteration = readU32(p + o); o += 4;
    out->telemetry.occurrenceOrdinal = readU32(p + o); o += 4;
    out->telemetry.logicCommitSequence = readU64(p + o); o += 8;
    out->telemetry.executeFrame = readU64(p + o); o += 8;
    if (o != kPacked)
        return kMonitorIncompatible;
    return kOk;
}

MonitorDecodeBatch fail(MonitorResult status, uint64_t nextBytes = kPacked)
{
    MonitorDecodeBatch out;
    out.status = status;
    out.nextRecordBytes = nextBytes;
    return out;
}

bool permit(const MonitorDecodeAllocator& allocator, std::size_t bytes)
{
    if (!allocator)
        return true;
    return allocator(bytes);
}

template <typename Work>
bool tryPermittedAlloc(const MonitorDecodeAllocator& allocator,
                       std::size_t bytes,
                       Work&& work)
{
    if (!permit(allocator, bytes))
        return false;
    try
    {
        work();
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}

} // namespace

MonitorDecodeBatch decodePackedRecords(const uint8_t* packedBytes,
                                       uint64_t bytesWritten,
                                       uint32_t expectedRecords,
                                       MonitorDecodeAllocator allocator)
{
    if (kPacked != 316u)
        return fail(kMonitorIncompatible);
    if (bytesWritten > 0 && packedBytes == nullptr)
        return fail(kInvalidArg, 0);
    if ((expectedRecords == 0) != (bytesWritten == 0))
        return fail(kMonitorIncompatible, 0);
    if (expectedRecords > 0 && bytesWritten / kPacked < expectedRecords)
        return fail(kMonitorIncompatible, 0);
    if (bytesWritten == 0)
    {
        MonitorDecodeBatch empty;
        empty.status = kOk;
        return empty;
    }
    if (bytesWritten > std::numeric_limits<std::size_t>::max())
        return fail(kMonitorIncompatible, 0);
    // Upper bound on owned copies: every successful batch copies at most
    // bytesWritten. Deny this before any reserve/assign.
    if (!permit(allocator, static_cast<std::size_t>(bytesWritten)))
        return fail(kMonitorIncompatible, 0);

    std::vector<MonitorDecodedRecord> staging;
    const std::size_t stagingBytes =
        static_cast<std::size_t>(expectedRecords) * sizeof(MonitorDecodedRecord);
    if (!tryPermittedAlloc(allocator, stagingBytes, [&] { staging.reserve(expectedRecords); }))
        return fail(kMonitorIncompatible, 0);

    uint64_t offset = 0;
    try
    {
        for (uint32_t i = 0; i < expectedRecords; ++i)
        {
            if ((offset & 7ull) != 0)
                return fail(kMonitorIncompatible);
            if (offset >= bytesWritten)
                return fail(kMonitorIncompatible);
            const uint64_t remaining = bytesWritten - offset;
            if (remaining < kPacked)
                return fail(kMonitorIncompatible, kPacked);

            MonitorRecordHeader header {};
            const MonitorResult headerStatus = readPackedHeader(packedBytes + offset, &header);
            if (headerStatus != kOk)
                return fail(headerStatus, kPacked);
            if ((header.recordBytes & 7ull) != 0 ||
                header.recordBytes < kPacked ||
                header.payloadBytes > header.recordBytes - kPacked)
                return fail(kMonitorIncompatible, header.recordBytes);
            if (header.recordBytes > remaining)
                return fail(kMonitorIncompatible, header.recordBytes);

            const uint64_t padFrom = kPacked + header.payloadBytes;
            if (padFrom > header.recordBytes)
                return fail(kMonitorIncompatible, header.recordBytes);
            if (!paddingZero(packedBytes + offset, padFrom, header.recordBytes))
                return fail(kMonitorIncompatible, header.recordBytes);

            const uint32_t required = header.flags & kMonitorRecordFlagRequired;
            const uint32_t optional = header.flags & kMonitorRecordFlagOptional;
            if ((required == 0 && optional == 0) || (required != 0 && optional != 0))
                return fail(kMonitorIncompatible, header.recordBytes);
            const uint32_t reservedFlags =
                header.flags & ~(kMonitorRecordFlagRequired | kMonitorRecordFlagOptional);
            const bool unknownKind =
                header.recordKind == kMonitorRecordUnknown ||
                (header.recordKind >= kMonitorRecordInvalid &&
                 header.recordKind != kMonitorRecordEventDetail);
            const bool unknownVersion = header.recordVersion != 1u &&
                !(header.recordKind == kMonitorRecordEventDetail && header.recordVersion == 2u);
            const bool unknown = unknownKind || unknownVersion || reservedFlags != 0;
            if (unknown && required)
                return fail(kMonitorIncompatible, header.recordBytes);

            const std::size_t copyBytes = unknown && optional
                ? static_cast<std::size_t>(header.recordBytes)
                : static_cast<std::size_t>(header.payloadBytes);

            MonitorDecodedRecord rec;
            rec.header = header;
            rec.unknownOptional = unknown && optional;
            if (copyBytes > 0)
            {
                if (!tryPermittedAlloc(allocator, copyBytes, [&] {
                        if (unknown && optional)
                        {
                            rec.skippedBytes.assign(
                                packedBytes + offset,
                                packedBytes + offset + static_cast<size_t>(header.recordBytes));
                        }
                        else
                        {
                            rec.payload.assign(
                                packedBytes + offset + kPacked,
                                packedBytes + offset + kPacked +
                                    static_cast<size_t>(header.payloadBytes));
                        }
                    }))
                    return fail(kMonitorIncompatible, 0);
            }
            staging.push_back(std::move(rec));

            uint64_t next = 0;
            if (addOverflow(offset, header.recordBytes, &next))
                return fail(kMonitorIncompatible, header.recordBytes);
            offset = next;
        }
        if (offset != bytesWritten || staging.size() != expectedRecords)
            return fail(kMonitorIncompatible);

        MonitorDecodeBatch batch;
        batch.status = kOk;
        batch.records = std::move(staging);
        batch.recordsWritten = static_cast<uint32_t>(batch.records.size());
        batch.bytesConsumed = offset;
        return batch;
    }
    catch (const std::bad_alloc&)
    {
        return fail(kMonitorIncompatible, 0);
    }
}

bool decodeEventDetail(const MonitorDecodedRecord& record, MonitorEventDetail* out,
                        MonitorEventDetailV2* identity)
{
    const bool version2 = record.header.recordVersion == 2;
    if (!out || record.unknownOptional || record.header.recordKind != kMonitorRecordEventDetail ||
        (record.header.recordVersion != 1 && !version2) || record.header.streamKind != kMonitorStreamEvent ||
        record.payload.size() != (version2 ? sizeof(MonitorEventDetailV2) : sizeof(MonitorEventDetail))) return false;
    const auto* p = record.payload.data();
    MonitorEventDetail value{};
    value.structSize = readU32(p);
    value.category = readU32(p + 4);
    value.code = readU32(p + 8);
    value.level = readU32(p + 12);
    value.result = static_cast<int32_t>(readU32(p + 16));
    value.reserved0 = readU32(p + 20);
    value.timestampNs = readU64(p + 24);
    value.arg0 = readU64(p + 32);
    value.arg1 = readU64(p + 40);
    value.valueBits = readU32(p + 48);
    value.textBytes = readU32(p + 52);
    if (value.structSize != record.payload.size() || value.reserved0 != 0 || value.timestampNs == 0 ||
        value.category < kMonitorEventCategoryOperation ||
        value.category > kMonitorEventCategoryDiagnostic || value.level > kLogError ||
        value.textBytes > sizeof(value.text) ||
        !paddingZero(p, 56 + value.textBytes, sizeof(value))) return false;
    std::copy_n(p + 56, value.textBytes, value.text);
    *out = value;
    if (identity) {
        *identity = {};
        identity->structSize = value.structSize;
        identity->category = value.category;
        identity->code = value.code;
        identity->level = value.level;
        identity->result = value.result;
        identity->timestampNs = value.timestampNs;
        identity->arg0 = value.arg0;
        identity->arg1 = value.arg1;
        identity->valueBits = value.valueBits;
        identity->textBytes = value.textBytes;
        std::copy_n(value.text, value.textBytes, identity->text);
        if (version2) {
            std::copy_n(p + sizeof(value), 16, identity->projectGuid);
        }
    }
    return true;
}

} // namespace Monitor
} // namespace VNX
