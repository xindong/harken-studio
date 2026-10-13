#include "MonitorStore.h"
#include "Monitor/MonitorTypes.h"

#include <new>
#include <string>
#include <unordered_set>

namespace VNX {
namespace Monitor {
namespace {

constexpr uint32_t kStreamMask =
    kMonitorStreamState | kMonitorStreamEvent |
    kMonitorStreamTimeSeries | kMonitorStreamOptionalAudio;

constexpr std::size_t kIndexIdBytes = sizeof(MonitorRecordId);

struct SnapshotReleaser
{
    uint64_t charged = 0;
    void operator()(const MonitorStoreSnapshot* snapshot) const
    {
        delete snapshot;
        if (charged > 0)
            MonitorStoreBudget::instance().release(charged);
    }
};

std::string uniqueKey(uint64_t streamId, uint32_t streamKind, uint64_t sourceSequence)
{
    return std::to_string(streamId) + "|" + std::to_string(streamKind) + "|" +
           std::to_string(sourceSequence);
}

} // namespace

std::vector<MonitorStoreQueryHit>
MonitorStoreSnapshot::hits(const std::vector<MonitorRecordId>& ids) const
{
    std::vector<MonitorStoreQueryHit> out;
    out.reserve(ids.size());
    for (const auto id : ids)
    {
        if (id >= records.size())
            continue;
        const auto& rec = records[id];
        MonitorStoreQueryHit hit;
        hit.header = rec.header;
        hit.payload = rec.payload;
        hit.skippedBytes = rec.skippedBytes;
        out.push_back(hit);
    }
    return out;
}

std::vector<MonitorStoreQueryHit>
MonitorStoreSnapshot::findByRecordKind(uint32_t kind) const
{
    std::vector<MonitorRecordId> ids;
    const auto range = byRecordKind.equal_range(kind);
    for (auto it = range.first; it != range.second; ++it)
        ids.push_back(it->second);
    return hits(ids);
}

std::vector<MonitorStoreQueryHit>
MonitorStoreSnapshot::findByExecuteFrame(uint64_t frame) const
{
    std::vector<MonitorRecordId> ids;
    const auto range = byExecuteFrame.equal_range(frame);
    for (auto it = range.first; it != range.second; ++it)
        ids.push_back(it->second);
    return hits(ids);
}

std::vector<MonitorStoreQueryHit>
MonitorStoreSnapshot::findByInstance(uint64_t handle, uint64_t generation) const
{
    std::vector<MonitorRecordId> ids;
    const uint64_t key = handle ^ (generation * 0x9e3779b97f4a7c15ull);
    const auto range = byInstance.equal_range(key);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second >= records.size())
            continue;
        const auto& rec = records[it->second];
        if (rec.header.instanceHandle != handle || rec.header.instanceGeneration != generation)
            continue;
        ids.push_back(it->second);
    }
    return hits(ids);
}

MonitorStoreBudget& MonitorStoreBudget::instance()
{
    static MonitorStoreBudget budget;
    return budget;
}

void MonitorStoreBudget::resetForTests()
{
    std::lock_guard<std::mutex> lock(mutex_);
    reserved_ = 0;
}

uint64_t MonitorStoreBudget::reservedBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return reserved_;
}

bool MonitorStoreBudget::tryReserve(uint64_t bytes, uint64_t hardLimit)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > hardLimit)
        return false;
    if (reserved_ > hardLimit - bytes)
        return false;
    reserved_ += bytes;
    return true;
}

void MonitorStoreBudget::release(uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > reserved_)
        reserved_ = 0;
    else
        reserved_ -= bytes;
}

MonitorStore::MonitorStore(MonitorCapabilities capabilities, MonitorDecodeAllocator allocator)
    : caps_(capabilities)
    , allocator_(std::move(allocator))
{
    usable_ = caps_.consumerQueueBudgetAvailable;
}

MonitorStore::~MonitorStore() = default;

bool MonitorStore::permit(std::size_t bytes) const
{
    if (!allocator_)
        return true;
    return allocator_(bytes);
}

int MonitorStore::streamIndex(uint32_t streamKind)
{
    switch (streamKind)
    {
    case kMonitorStreamState:
        return 0;
    case kMonitorStreamEvent:
        return 1;
    case kMonitorStreamTimeSeries:
        return 2;
    case kMonitorStreamOptionalAudio:
        return 3;
    default:
        return -1;
    }
}

uint64_t MonitorStore::instanceKey(uint64_t handle, uint64_t generation)
{
    return handle ^ (generation * 0x9e3779b97f4a7c15ull);
}

uint64_t MonitorStore::telemetryKey(const MonitorRecordHeader& header)
{
    return header.traceCorrelationId ^
           (header.telemetry.arrangingInstance * 0x9e3779b97f4a7c15ull) ^
           harkenstudio::HashObjectId(header.telemetry.clipId);
}

uint64_t MonitorStore::recordCharge(const MonitorDecodedRecord& rec, bool indexed)
{
    uint64_t bytes = sizeof(MonitorRecordHeader);
    bytes += rec.payload.size();
    bytes += rec.skippedBytes.size();
    if (indexed)
        bytes += kIndexIdBytes * 5u;
    return bytes;
}

bool MonitorStore::isOptionalUnknown(const MonitorDecodedRecord& rec)
{
    return rec.unknownOptional || !rec.skippedBytes.empty();
}

MonitorStoreAppendResult MonitorStore::reject(MonitorStoreReject reason, bool retryable) const
{
    MonitorStoreAppendResult out;
    out.status = (reason == MonitorStoreReject::InvalidArg) ? kInvalidArg
                                                            : kMonitorIncompatible;
    out.reject = reason;
    out.generation = generation_;
    out.saturationCount = saturationCount_;
    out.retryable = retryable;
    return out;
}

MonitorStoreAppendResult MonitorStore::appendBatch(MonitorDecodeBatch&& batch)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!usable_)
        return reject(MonitorStoreReject::InvalidArg, false);
    if (batch.status != kOk)
        return reject(MonitorStoreReject::DecoderFailed, false);
    if (batch.records.size() != batch.recordsWritten)
        return reject(MonitorStoreReject::IncompleteBatch, false);

    try
    {
        auto next = std::make_unique<MonitorStoreSnapshot>();
        next->generation = generation_ + 1;
        if (current_)
            next->records = current_->records;

        std::unordered_set<std::string> seenKeys;
        seenKeys.reserve(next->records.size() + batch.records.size());
        if (current_)
        {
            for (const auto& rec : current_->records)
            {
                if (isOptionalUnknown(rec))
                    continue;
                seenKeys.insert(uniqueKey(rec.header.streamId, rec.header.streamKind,
                                          rec.header.sourceSequence));
            }
        }

        if (!permit(sizeof(MonitorStoreSnapshot)))
            return reject(MonitorStoreReject::AllocatorDenied, false);

        for (auto& rec : batch.records)
        {
            const bool unknown = isOptionalUnknown(rec);
            if (!unknown && rec.header.recordKind >= kMonitorRecordInvalid)
                return reject(MonitorStoreReject::BadRecordKind, false);
            if (!unknown)
            {
                const int stream = streamIndex(rec.header.streamKind);
                if (stream < 0 ||
                    (rec.header.streamKind & kStreamMask) != rec.header.streamKind ||
                    (rec.header.streamKind & (rec.header.streamKind - 1u)) != 0 ||
                    rec.header.streamKind == 0)
                    return reject(MonitorStoreReject::BadStreamKind, false);
                const auto key = uniqueKey(rec.header.streamId, rec.header.streamKind,
                                           rec.header.sourceSequence);
                if (!seenKeys.insert(key).second)
                    return reject(MonitorStoreReject::DuplicateKey, false);
            }

            if (!permit(static_cast<std::size_t>(recordCharge(rec, !unknown))))
                return reject(MonitorStoreReject::AllocatorDenied, false);
            next->records.push_back(std::move(rec));
        }

        next->byInstance.reserve(next->records.size());
        next->byTelemetry.reserve(next->records.size());
        next->byExecuteFrame.reserve(next->records.size());
        next->byRecordKind.reserve(next->records.size());
        for (MonitorRecordId id = 0; id < next->records.size(); ++id)
        {
            const auto& rec = next->records[id];
            if (isOptionalUnknown(rec))
                continue;
            const int stream = streamIndex(rec.header.streamKind);
            if (stream < 0)
                return reject(MonitorStoreReject::IndexBuildFailed, false);
            next->byStream[stream].push_back(id);
            next->byInstance.emplace(
                instanceKey(rec.header.instanceHandle, rec.header.instanceGeneration), id);
            next->byTelemetry.emplace(telemetryKey(rec.header), id);
            next->byExecuteFrame.emplace(rec.header.executeFrame, id);
            next->byRecordKind.emplace(rec.header.recordKind, id);
        }

        next->chargedBytes = 0;
        for (const auto& rec : next->records)
            next->chargedBytes += recordCharge(rec, !isOptionalUnknown(rec));
        if (next->chargedBytes > caps_.resolvedConsumerQueueBytes)
        {
            ++saturationCount_;
            auto out = reject(MonitorStoreReject::CapacitySession, true);
            out.saturationCount = saturationCount_;
            return out;
        }

        const uint64_t extra = next->chargedBytes;
        if (!MonitorStoreBudget::instance().tryReserve(extra, caps_.hardConsumerQueueBytes))
        {
            ++saturationCount_;
            auto out = reject(MonitorStoreReject::CapacityProcess, true);
            out.saturationCount = saturationCount_;
            return out;
        }

        std::shared_ptr<const MonitorStoreSnapshot> published(next.release(),
                                                              SnapshotReleaser{extra});
        current_ = std::move(published);
        generation_ = current_->generation;
        MonitorStoreAppendResult ok;
        ok.status = kOk;
        ok.reject = MonitorStoreReject::None;
        ok.generation = generation_;
        ok.saturationCount = saturationCount_;
        return ok;
    }
    catch (const std::bad_alloc&)
    {
        return reject(MonitorStoreReject::AllocatorDenied, false);
    }
}

std::shared_ptr<const MonitorStoreSnapshot> MonitorStore::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

uint64_t MonitorStore::generation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

uint64_t MonitorStore::saturationCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return saturationCount_;
}

bool MonitorStore::usable() const
{
    return usable_;
}

} // namespace Monitor
} // namespace VNX
