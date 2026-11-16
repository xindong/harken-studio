#pragma once

#include "MonitorStore.h"

#include "Monitor/MonitorTypes.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace VNX {
namespace Monitor {

struct MonitorSessionCommit
{
    uint64_t chunkSequence = 0;
    uint32_t kind = 0;
    uint32_t flags = 0;
    uint64_t payloadBytes = 0;
    std::vector<uint8_t> skippedBytes;
};

struct MonitorSessionFileResult
{
    MonitorResult status = kNotInitialized;
    uint64_t bytesConsumed = 0;
    uint64_t fileMagic = 0;
    uint64_t capabilityFingerprint = 0;
    uint64_t engineMonitorInstanceId = 0;
    std::string studioVersion;
    std::string engineVersion;
    uint32_t remoteProtocolMajor = 0;
    uint32_t commitsWritten = 0;
    uint32_t droppedRequired = 0;
    uint32_t droppedOptional = 0;
    bool storeFailed = false;
    bool retryable = false;
    MonitorStoreReject reject = MonitorStoreReject::None;
    MonitorStoreAppendResult storeResult {};
    std::vector<MonitorSessionCommit> commits;
    std::vector<MonitorDecodedRecord> records;
};

class MonitorSessionFile
{
public:
    static MonitorSessionFileResult restore(
        const std::string& path,
        MonitorStore* store = nullptr,
        MonitorDecodeAllocator allocator = {});
};

struct MonitorSessionSource
{
    std::string engineVersion;
    uint32_t remoteProtocolMajor = 0;
    uint64_t engineMonitorInstanceId = 0;
    uint64_t capabilityFingerprint = 0;
};

class MonitorSessionWriter
{
public:
    bool open(const std::string& path, const MonitorSessionSource& source, uint64_t fileByteBudget);
    bool append(const uint8_t* packedRecords, uint64_t bytes);
    void close();
    bool isOpen() const { return output_.is_open(); }
    MonitorRecordingStatus status() const { return status_; }

private:
    bool fail();

    std::ofstream output_;
    uint64_t budget_ = 0;
    uint64_t sequence_ = 0;
    MonitorRecordingStatus status_ {};
};

} // namespace Monitor
} // namespace VNX
