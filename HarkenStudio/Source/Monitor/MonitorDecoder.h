#pragma once

#include "Monitor/MonitorTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace VNX {
namespace Monitor {

struct MonitorDecodedRecord
{
    MonitorRecordHeader header {};
    std::vector<uint8_t> payload;
    std::vector<uint8_t> skippedBytes;
    bool unknownOptional = false;
};

struct MonitorDecodeBatch
{
    MonitorResult status = kNotInitialized;
    uint32_t recordsWritten = 0;
    uint64_t bytesConsumed = 0;
    uint64_t nextRecordBytes = 0;
    std::vector<MonitorDecodedRecord> records;
};

// Return false to deny an allocation. Default (empty) permits every size.
using MonitorDecodeAllocator = std::function<bool(std::size_t)>;

// Production entry: packed-record stream with no fixture count prefix.
// expectedRecords comes from MonitorPollResult.recordsWritten.
MonitorDecodeBatch decodePackedRecords(const uint8_t* packedBytes,
                                       uint64_t bytesWritten,
                                       uint32_t expectedRecords,
                                       MonitorDecodeAllocator allocator = {});

bool decodeExecutionFact(const MonitorDecodedRecord& record, MonitorExecutionFactV1* out);

bool decodeEventDetail(const MonitorDecodedRecord& record, MonitorEventDetail* out,
                        MonitorEventDetailV2* identity = nullptr);

} // namespace Monitor
} // namespace VNX
