#pragma once

#include "MonitorDecoder.h"

#include "Monitor/MonitorTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace VNX {
namespace Monitor {

enum class MonitorStoreReject
{
    None = 0,
    InvalidArg,
    DecoderFailed,
    IncompleteBatch,
    BadStreamKind,
    BadRecordKind,
    DuplicateKey,
    CapacitySession,
    CapacityProcess,
    AllocatorDenied,
    IndexBuildFailed,
};

struct MonitorStoreAppendResult
{
    MonitorResult status = kOk;
    MonitorStoreReject reject = MonitorStoreReject::None;
    uint64_t generation = 0;
    uint64_t saturationCount = 0;
    bool retryable = false;
};

struct MonitorStoreQueryHit
{
    MonitorRecordHeader header {};
    std::span<const uint8_t> payload;
    std::span<const uint8_t> skippedBytes;
};

using MonitorRecordId = uint32_t;

struct MonitorStoreSnapshot
{
    uint64_t generation = 0;
    uint64_t chargedBytes = 0;
    std::vector<MonitorDecodedRecord> records;
    std::vector<MonitorRecordId> byStream[4];
    std::unordered_multimap<uint64_t, MonitorRecordId> byInstance;
    std::unordered_multimap<uint64_t, MonitorRecordId> byTelemetry;
    std::unordered_multimap<uint64_t, MonitorRecordId> byExecuteFrame;
    std::unordered_multimap<uint32_t, MonitorRecordId> byRecordKind;

    std::vector<MonitorStoreQueryHit> hits(const std::vector<MonitorRecordId>& ids) const;
    std::vector<MonitorStoreQueryHit> findByRecordKind(uint32_t kind) const;
    std::vector<MonitorStoreQueryHit> findByExecuteFrame(uint64_t frame) const;
    std::vector<MonitorStoreQueryHit> findByInstance(uint64_t handle, uint64_t generation) const;
};

class MonitorStoreBudget
{
public:
    static MonitorStoreBudget& instance();
    void resetForTests();
    uint64_t reservedBytes() const;
    bool tryReserve(uint64_t bytes, uint64_t hardLimit);
    void release(uint64_t bytes);

private:
    mutable std::mutex mutex_;
    uint64_t reserved_ = 0;
};

class MonitorStore
{
public:
    MonitorStore(MonitorCapabilities capabilities, MonitorDecodeAllocator allocator = {});
    ~MonitorStore();

    MonitorStore(const MonitorStore&) = delete;
    MonitorStore& operator=(const MonitorStore&) = delete;

    MonitorStoreAppendResult appendBatch(MonitorDecodeBatch&& batch);
    std::shared_ptr<const MonitorStoreSnapshot> snapshot() const;
    uint64_t generation() const;
    uint64_t saturationCount() const;
    bool usable() const;

private:
    bool permit(std::size_t bytes) const;
    static int streamIndex(uint32_t streamKind);
    static uint64_t instanceKey(uint64_t handle, uint64_t generation);
    static uint64_t telemetryKey(const MonitorRecordHeader& header);
    static uint64_t recordCharge(const MonitorDecodedRecord& rec, bool indexed);
    static bool isOptionalUnknown(const MonitorDecodedRecord& rec);
    MonitorStoreAppendResult reject(MonitorStoreReject reason, bool retryable) const;

    MonitorCapabilities caps_ {};
    MonitorDecodeAllocator allocator_;
    bool usable_ = false;
    mutable std::mutex mutex_;
    std::shared_ptr<const MonitorStoreSnapshot> current_;
    uint64_t generation_ = 0;
    uint64_t saturationCount_ = 0;
};

} // namespace Monitor
} // namespace VNX
