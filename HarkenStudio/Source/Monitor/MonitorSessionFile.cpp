#include "MonitorSessionFile.h"
#include "Monitor/MonitorTypes.h"

#include "MonitorDecoder.h"

#include <fstream>
#include <limits>
#include <new>
#include <utility>

namespace VNX {
namespace Monitor {
namespace {

// The packed file header is a disk-only size; it is not a public ABI constant.
constexpr uint64_t kFileHeaderBytes = 64u;
constexpr uint64_t kChunkHeaderBytes = kMonitorSessionChunkHeaderBytes;
constexpr uint64_t kFooterBytes = kMonitorSessionCommitFooterBytes;
constexpr uint64_t kRecordEnvelopeBytes = kMonitorRecordEnvelopePackedBytes;

uint16_t readU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

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

uint32_t crc32c(const uint8_t* bytes, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78u & ((crc & 1u) != 0 ? 0xffffffffu : 0u));
    }
    return ~crc;
}

bool readAt(std::ifstream& input, uint64_t offset, uint8_t* destination, uint64_t bytes)
{
    if (bytes == 0)
        return true;
    if (bytes > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        return false;
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input)
        return false;
    input.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(bytes));
    return input.good() && static_cast<uint64_t>(input.gcount()) == bytes;
}

bool knownChunkKind(uint32_t kind)
{
    return kind >= kMonitorChunkSessionHeader && kind <= kMonitorChunkRecoveryIndex;
}

bool recordStreamKind(uint32_t kind)
{
    return kind == kMonitorChunkEvents || kind == kMonitorChunkState ||
           kind == kMonitorChunkTimeSeries || kind == kMonitorChunkOptionalAudio ||
           kind == kMonitorChunkDropSummaries;
}

bool requiredFlags(uint32_t flags)
{
    return (flags & kMonitorChunkRequired) != 0 &&
           (flags & kMonitorChunkOptional) == 0;
}

bool optionalFlags(uint32_t flags)
{
    return (flags & kMonitorChunkOptional) != 0 &&
           (flags & kMonitorChunkRequired) == 0;
}

bool countRecordEnvelopes(const std::vector<uint8_t>& payload, uint32_t* count)
{
    uint64_t offset = 0;
    uint64_t records = 0;
    while (offset < payload.size())
    {
        const uint64_t remaining = static_cast<uint64_t>(payload.size()) - offset;
        if (remaining < kRecordEnvelopeBytes)
            return false;
        const uint64_t recordBytes = readU64(payload.data() + offset + 16);
        if (recordBytes < kRecordEnvelopeBytes || (recordBytes & 7ull) != 0 ||
            recordBytes > remaining)
            return false;
        uint64_t next = 0;
        if (addOverflow(offset, recordBytes, &next))
            return false;
        offset = next;
        if (++records > std::numeric_limits<uint32_t>::max())
            return false;
    }
    if (offset != payload.size())
        return false;
    *count = static_cast<uint32_t>(records);
    return true;
}

bool stopOnStoreFailure(MonitorStoreReject reject)
{
    return reject == MonitorStoreReject::CapacitySession ||
           reject == MonitorStoreReject::CapacityProcess ||
           reject == MonitorStoreReject::AllocatorDenied ||
           reject == MonitorStoreReject::IndexBuildFailed ||
           reject == MonitorStoreReject::InvalidArg;
}

bool stopOnDecodeFailure(const MonitorDecodeBatch& batch)
{
    return batch.status != kOk && batch.bytesConsumed == 0 && batch.recordsWritten == 0;
}

void markAllocatorDenied(MonitorSessionFileResult* result)
{
    result->status = kMonitorIncompatible;
    result->reject = MonitorStoreReject::AllocatorDenied;
    result->retryable = true;
}

bool appendCommit(MonitorSessionFileResult* result,
                  uint64_t sequence,
                  uint32_t kind,
                  uint32_t flags,
                  uint64_t payloadBytes,
                  std::vector<uint8_t>&& skippedBytes = {})
{
    try
    {
        MonitorSessionCommit commit;
        commit.chunkSequence = sequence;
        commit.kind = kind;
        commit.flags = flags;
        commit.payloadBytes = payloadBytes;
        commit.skippedBytes = std::move(skippedBytes);
        result->commits.push_back(std::move(commit));
        result->commitsWritten = static_cast<uint32_t>(result->commits.size());
        return true;
    }
    catch (const std::bad_alloc&)
    {
        markAllocatorDenied(result);
        return false;
    }
}

} // namespace

MonitorSessionFileResult MonitorSessionFile::restore(const std::string& path,
                                                     MonitorStore* store,
                                                     MonitorDecodeAllocator allocator)
{
    MonitorSessionFileResult result;
    result.status = kOk;

    std::ifstream input(path, std::ios::binary | std::ios::in);
    if (!input)
    {
        result.status = kMonitorIncompatible;
        return result;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0)
    {
        result.status = kMonitorIncompatible;
        return result;
    }
    const uint64_t fileBytes = static_cast<uint64_t>(end);
    if (fileBytes > kMonitorHardSessionFileBytes ||
        fileBytes < kFileHeaderBytes)
    {
        result.status = kMonitorIncompatible;
        return result;
    }

    std::vector<uint8_t> fileHeader;
    try
    {
        fileHeader.resize(static_cast<size_t>(kFileHeaderBytes));
    }
    catch (const std::bad_alloc&)
    {
        markAllocatorDenied(&result);
        return result;
    }
    if (!readAt(input, 0, fileHeader.data(), kFileHeaderBytes))
    {
        result.status = kMonitorIncompatible;
        return result;
    }

    const uint32_t major = readU32(fileHeader.data());
    const uint32_t minor = readU32(fileHeader.data() + 4);
    const uint32_t reserved0 = readU32(fileHeader.data() + 8);
    const uint64_t magic = readU64(fileHeader.data() + 12);
    const uint64_t headerBytes = readU64(fileHeader.data() + 20);
    const uint32_t storedHeaderCrc = readU32(fileHeader.data() + 60);
    if (major != kMonitorSessionFormatMajor ||
        minor != kMonitorSessionFormatMinor || reserved0 != 0 ||
        magic != kMonitorSessionFileMagic || headerBytes != kFileHeaderBytes)
    {
        result.status = kMonitorIncompatible;
        return result;
    }
    std::vector<uint8_t> crcHeader = fileHeader;
    crcHeader[60] = 0;
    crcHeader[61] = 0;
    crcHeader[62] = 0;
    crcHeader[63] = 0;
    if (crc32c(crcHeader.data(), crcHeader.size()) != storedHeaderCrc)
    {
        result.status = kMonitorIncompatible;
        return result;
    }
    result.fileMagic = magic;
    result.engineMonitorInstanceId = readU64(fileHeader.data() + 36);
    result.capabilityFingerprint = readU64(fileHeader.data() + 52);

    uint64_t offset = kFileHeaderBytes;
    uint64_t lastSequence = 0;
    bool haveSequence = false;
    while (offset < fileBytes)
    {
        const uint64_t remaining = fileBytes - offset;
        if (remaining < kChunkHeaderBytes)
        {
            result.bytesConsumed = offset;
            return result;
        }

        std::vector<uint8_t> header;
        try
        {
            header.resize(static_cast<size_t>(kChunkHeaderBytes));
        }
        catch (const std::bad_alloc&)
        {
            markAllocatorDenied(&result);
            result.bytesConsumed = offset;
            return result;
        }
        if (!readAt(input, offset, header.data(), kChunkHeaderBytes))
        {
            result.bytesConsumed = offset;
            return result;
        }

        const uint32_t kind = readU16(header.data());
        const uint32_t flags = readU16(header.data() + 2);
        const uint32_t version = readU16(header.data() + 4);
        const uint32_t packedHeaderBytes = readU16(header.data() + 6);
        const uint64_t payloadBytes = readU64(header.data() + 8);
        const uint64_t chunkSequence = readU64(header.data() + 16);
        const uint32_t payloadCrc = readU32(header.data() + 24);
        const uint32_t headerCrc = readU32(header.data() + 28);
        if (packedHeaderBytes != kChunkHeaderBytes || version != 1u ||
            crc32c(header.data(), 28) != headerCrc)
        {
            result.bytesConsumed = offset;
            return result;
        }
        if (payloadBytes > kMonitorHardChunkPayloadBytes)
        {
            result.status = kMonitorIncompatible;
            result.bytesConsumed = offset;
            return result;
        }

        uint64_t afterPayload = 0;
        uint64_t commitEnd = 0;
        if (addOverflow(offset, kChunkHeaderBytes, &afterPayload) ||
            addOverflow(afterPayload, payloadBytes, &afterPayload) ||
            addOverflow(afterPayload, kFooterBytes, &commitEnd))
        {
            result.status = kMonitorIncompatible;
            result.bytesConsumed = offset;
            return result;
        }
        if (commitEnd > fileBytes)
        {
            result.bytesConsumed = offset;
            return result;
        }

        std::vector<uint8_t> footer;
        std::vector<uint8_t> payload;
        try
        {
            footer.resize(static_cast<size_t>(kFooterBytes));
            payload.resize(static_cast<size_t>(payloadBytes));
        }
        catch (const std::bad_alloc&)
        {
            markAllocatorDenied(&result);
            result.bytesConsumed = offset;
            return result;
        }
        if (!readAt(input, afterPayload, footer.data(), kFooterBytes) ||
            (payloadBytes > 0 && !readAt(input, offset + kChunkHeaderBytes,
                                          payload.data(), payloadBytes)))
        {
            result.bytesConsumed = offset;
            return result;
        }
        if (readU64(footer.data()) != kMonitorSessionCommitMagic ||
            readU64(footer.data() + 8) != chunkSequence ||
            readU64(footer.data() + 16) != payloadBytes)
        {
            result.bytesConsumed = offset;
            return result;
        }
        if (haveSequence)
        {
            uint64_t expected = 0;
            if (addOverflow(lastSequence, 1ull, &expected) || chunkSequence != expected)
            {
                result.status = kMonitorIncompatible;
                result.bytesConsumed = offset;
                return result;
            }
        }

        const bool required = requiredFlags(flags);
        const bool optional = optionalFlags(flags);
        if (!required && !optional)
        {
            result.status = kInvalidArg;
            result.bytesConsumed = offset;
            return result;
        }
        if (crc32c(payload.data(), payload.size()) != payloadCrc)
        {
            if (required)
                ++result.droppedRequired;
            else
                ++result.droppedOptional;
            haveSequence = true;
            lastSequence = chunkSequence;
            offset = commitEnd;
            result.bytesConsumed = offset;
            continue;
        }
        const bool known = knownChunkKind(kind);
        if (required && !known)
        {
            result.status = kMonitorIncompatible;
            result.bytesConsumed = offset;
            return result;
        }

        if (optional && !known)
        {
            MonitorDecodeBatch batch;
            batch.status = kOk;
            batch.recordsWritten = 1;
            batch.bytesConsumed = payloadBytes;
            try
            {
                MonitorDecodedRecord optionalRecord;
                optionalRecord.header.structSize = sizeof(MonitorRecordHeader);
                optionalRecord.header.recordKind = kind;
                optionalRecord.header.recordVersion = version;
                optionalRecord.header.flags = kMonitorRecordFlagOptional;
                optionalRecord.header.recordBytes = payloadBytes;
                optionalRecord.header.payloadBytes = payloadBytes;
                optionalRecord.skippedBytes = payload;
                optionalRecord.unknownOptional = true;
                batch.records.push_back(std::move(optionalRecord));
            }
            catch (const std::bad_alloc&)
            {
                markAllocatorDenied(&result);
                result.bytesConsumed = offset;
                return result;
            }
            if (store)
            {
                const auto append = store->appendBatch(std::move(batch));
                result.storeResult = append;
                if (append.status != kOk)
                {
                    result.storeFailed = true;
                    if (stopOnStoreFailure(append.reject))
                    {
                        result.status = append.status;
                        result.reject = append.reject;
                        result.retryable = append.retryable;
                        result.bytesConsumed = offset;
                        return result;
                    }
                    ++result.droppedOptional;
                    haveSequence = true;
                    lastSequence = chunkSequence;
                    offset = commitEnd;
                    result.bytesConsumed = offset;
                    continue;
                }
            }
            else
            {
                try
                {
                    for (auto& record : batch.records)
                        result.records.push_back(std::move(record));
                }
                catch (const std::bad_alloc&)
                {
                    markAllocatorDenied(&result);
                    result.bytesConsumed = offset;
                    return result;
                }
            }
            if (!appendCommit(&result, chunkSequence, kind, flags, payloadBytes, std::move(payload)))
            {
                result.bytesConsumed = store ? commitEnd : offset;
                return result;
            }
            haveSequence = true;
            lastSequence = chunkSequence;
            offset = commitEnd;
            result.bytesConsumed = offset;
            continue;
        }

        if (required && recordStreamKind(kind))
        {
            uint32_t expectedRecords = 0;
            if (!countRecordEnvelopes(payload, &expectedRecords))
            {
                ++result.droppedRequired;
                haveSequence = true;
                lastSequence = chunkSequence;
                offset = commitEnd;
                result.bytesConsumed = offset;
                continue;
            }
            bool allocatorDenied = false;
            auto trackingAllocator = allocator;
            if (allocator)
            {
                trackingAllocator = [&](std::size_t bytes) {
                    if (!allocator(bytes))
                    {
                        allocatorDenied = true;
                        return false;
                    }
                    return true;
                };
            }
            MonitorDecodeBatch batch;
            try
            {
                batch = decodePackedRecords(payload.data(), payloadBytes, expectedRecords,
                                            trackingAllocator);
            }
            catch (const std::bad_alloc&)
            {
                markAllocatorDenied(&result);
                result.bytesConsumed = offset;
                return result;
            }
            if (stopOnDecodeFailure(batch))
            {
                if (allocatorDenied)
                {
                    markAllocatorDenied(&result);
                }
                else if (allocator)
                {
                    const auto replay = decodePackedRecords(payload.data(), payloadBytes,
                                                            expectedRecords);
                    if (stopOnDecodeFailure(replay))
                        result.status = replay.status;
                    else
                        markAllocatorDenied(&result);
                }
                else
                {
                    result.status = batch.status;
                }
                result.bytesConsumed = offset;
                return result;
            }
            if (batch.status != kOk || batch.bytesConsumed != payloadBytes ||
                batch.recordsWritten != expectedRecords)
            {
                ++result.droppedRequired;
                haveSequence = true;
                lastSequence = chunkSequence;
                offset = commitEnd;
                result.bytesConsumed = offset;
                continue;
            }
            if (store)
            {
                const auto append = store->appendBatch(std::move(batch));
                result.storeResult = append;
                if (append.status != kOk)
                {
                    result.storeFailed = true;
                    if (stopOnStoreFailure(append.reject))
                    {
                        result.status = append.status;
                        result.reject = append.reject;
                        result.retryable = append.retryable;
                        result.bytesConsumed = offset;
                        return result;
                    }
                    ++result.droppedRequired;
                    haveSequence = true;
                    lastSequence = chunkSequence;
                    offset = commitEnd;
                    result.bytesConsumed = offset;
                    continue;
                }
            }
            else
            {
                try
                {
                    for (auto& record : batch.records)
                        result.records.push_back(std::move(record));
                }
                catch (const std::bad_alloc&)
                {
                    markAllocatorDenied(&result);
                    result.bytesConsumed = offset;
                    return result;
                }
            }
            if (!appendCommit(&result, chunkSequence, kind, flags, payloadBytes))
            {
                result.bytesConsumed = store ? commitEnd : offset;
                return result;
            }
        }
        else if (required || optional)
        {
            if (!appendCommit(&result, chunkSequence, kind, flags, payloadBytes))
            {
                result.bytesConsumed = offset;
                return result;
            }
        }

        haveSequence = true;
        lastSequence = chunkSequence;
        offset = commitEnd;
        result.bytesConsumed = offset;
    }
    result.bytesConsumed = offset;
    return result;
}

} // namespace Monitor
} // namespace VNX
