#include "MonitorPerformanceLiveHost.h"
#include "Monitor/MonitorTypes.h"

#include "MonitorDecoder.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace VNX {
namespace Monitor {
namespace {

constexpr uint64_t kCallbackCpu = 101;
constexpr uint64_t kStageDrainCpu = 102;
constexpr uint64_t kStageVoicesCpu = 103;
constexpr uint64_t kStageBusesCpu = 104;
constexpr uint64_t kArenaHighWater = 105;
constexpr uint32_t kNumericMask =
    kMonitorValidArmed | kMonitorValidNumeric;
constexpr uint64_t kPollBufferBytes = 65536;

} // namespace

MonitorPerformanceLiveHost::MonitorPerformanceLiveHost(MonitorAdapter* adapter)
    : adapter_(adapter)
    , store_(storeCaps())
{
}

MonitorPerformanceLiveHost::~MonitorPerformanceLiveHost()
{
    stopPolling();
    closeObservationSession();
}

void MonitorPerformanceLiveHost::setPollOverride(PollFn poll)
{
    pollOverride_ = std::move(poll);
}

void MonitorPerformanceLiveHost::setUiThreadCheck(ThreadCheckFn check)
{
    uiThreadCheck_ = std::move(check);
}

void MonitorPerformanceLiveHost::setTickHandler(TickFn handler)
{
    tickHandler_ = std::move(handler);
}

MonitorCapabilities MonitorPerformanceLiveHost::storeCaps() const
{
    MonitorCapabilities caps {};

    caps.resolvedConsumerQueueBytes = 1u << 20;
    caps.hardConsumerQueueBytes = 4u << 20;
    return caps;
}

bool MonitorPerformanceLiveHost::isNumeric(uint32_t validityFlags)
{
    return (validityFlags & kNumericMask) == kNumericMask;
}

bool MonitorPerformanceLiveHost::isKnownStream(uint64_t streamId)
{
    return streamId == kCallbackCpu || streamId == kStageDrainCpu
        || streamId == kStageVoicesCpu || streamId == kStageBusesCpu
        || streamId == kArenaHighWater;
}

const char* MonitorPerformanceLiveHost::seriesIdForStream(uint64_t streamId)
{
    switch (streamId)
    {
    case kCallbackCpu:
        return "callbackCpu";
    case kStageDrainCpu:
        return "stageDrainCpu";
    case kStageVoicesCpu:
        return "stageVoicesCpu";
    case kStageBusesCpu:
        return "stageBusesCpu";
    case kArenaHighWater:
        return "arenaHighWater";
    default:
        return nullptr;
    }
}

bool MonitorPerformanceLiveHost::openObservationSession()
{
    if (adapter_ == nullptr)
        return false;
    if (adapter_->hasOpenSession())
        return true;

    MonitorSessionDesc desc {};

    desc.requestedStreamKindMask = kMonitorStreamTimeSeries;
    desc.requestedPermissionMask = kMonitorPermObserveMeasurements;
    desc.consumerQueueBytes = kPollBufferBytes;
    const MonitorOpenResult opened = adapter_->open(&desc);
    if (opened.result != kOk || opened.handle == 0)
        return false;

    MonitorSubscriptionStatus status {};

    if (adapter_->getSubscriptionStatus(opened.handle, &status) == kOk)
    {
        acceptedPermissionMask_ = status.acceptedPermissionMask;
        resolvedStreamKindMask_ = status.resolvedStreamKindMask;
    }
    return true;
}

void MonitorPerformanceLiveHost::closeObservationSession()
{
    stopPolling();
    if (adapter_ == nullptr || !adapter_->hasOpenSession())
        return;
    adapter_->close(adapter_->sessionHandle());
    acceptedPermissionMask_ = 0;
    resolvedStreamKindMask_ = 0;
}

bool MonitorPerformanceLiveHost::hasOpenSession() const
{
    return adapter_ != nullptr && adapter_->hasOpenSession();
}

MonitorSessionHandle MonitorPerformanceLiveHost::sessionHandle() const
{
    return adapter_ != nullptr ? adapter_->sessionHandle() : MonitorSessionHandle{0};
}

uint32_t MonitorPerformanceLiveHost::acceptedPermissionMask() const
{
    return acceptedPermissionMask_;
}

uint32_t MonitorPerformanceLiveHost::resolvedStreamKindMask() const
{
    return resolvedStreamKindMask_;
}

MonitorPerformanceLiveTick MonitorPerformanceLiveHost::ingestDecoded(
    MonitorDecodeBatch&& batch, bool offUiThread)
{
    MonitorPerformanceLiveTick tick;
    tick.polledOffUiThread = offUiThread;
    tick.lastResult = batch.status;
    if (batch.status != kOk)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastTick_.polledOffUiThread = offUiThread;
        lastTick_.lastResult = batch.status;
        return lastTick_;
    }

    uint64_t latestFrame = 0;
    uint64_t latestSeq = 0;
    uint64_t latestPlan = 0;
    bool haveLive = false;
    for (const auto& rec : batch.records)
    {
        if (rec.header.streamKind != kMonitorStreamTimeSeries)
            continue;
        if (rec.header.recordKind != kMonitorRecordTimeSeries)
            continue;
        if (!isKnownStream(rec.header.streamId))
            continue;
        if (rec.payload.size() < sizeof(MonitorTimeSeriesSample))
            continue;
        MonitorTimeSeriesSample payload {};
        std::memcpy(&payload, rec.payload.data(), sizeof(payload));
        MonitorPerformanceLiveSample sample;
        sample.streamId = rec.header.streamId;
        sample.executeFrame = rec.header.executeFrame;
        sample.sourceSequence = rec.header.sourceSequence;
        sample.capturePlanGeneration = rec.header.capturePlanGeneration;
        sample.metricId = payload.metricId;
        sample.valueBits = payload.valueBits;
        sample.validityFlags = rec.header.validityFlags;
        tick.samples.push_back(sample);
        if (isNumeric(sample.validityFlags)
            && (!haveLive || sample.executeFrame > latestFrame
                || (sample.executeFrame == latestFrame && sample.sourceSequence >= latestSeq)))
        {
            haveLive = true;
            latestFrame = sample.executeFrame;
            latestSeq = sample.sourceSequence;
            latestPlan = sample.capturePlanGeneration;
        }
    }

    const auto appended = store_.appendBatch(std::move(batch));
    tick.lastResult = appended.status;

    tick.hasLiveFrame = haveLive;
    tick.liveExecuteFrame = static_cast<std::int64_t>(latestFrame);
    tick.capturePlanGeneration = latestPlan;
    tick.sourceSequence = latestSeq;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastTick_ = tick;
    }
    if (tickHandler_)
        tickHandler_(tick);
    return lastTick_;
}

MonitorPerformanceLiveTick MonitorPerformanceLiveHost::ingestPacked(
    const uint8_t* packedBytes, uint64_t bytesWritten, uint32_t expectedRecords)
{
    auto batch = decodePackedRecords(packedBytes, bytesWritten, expectedRecords);
    return ingestDecoded(std::move(batch), true);
}

MonitorPerformanceLiveTick MonitorPerformanceLiveHost::ingestOnce()
{
    const bool offUi = uiThreadCheck_ ? !uiThreadCheck_() : true;
    MonitorPerformanceLiveTick failed;
    failed.polledOffUiThread = offUi;
    if (!offUi)
    {
        failed.lastResult = kMonitorWrongThread;
        std::lock_guard<std::mutex> lock(mutex_);
        lastTick_.polledOffUiThread = false;
        lastTick_.lastResult = kMonitorWrongThread;
        return lastTick_;
    }
    if (adapter_ == nullptr || !adapter_->hasOpenSession())
    {
        failed.lastResult = kMonitorStaleSession;
        return failed;
    }

    std::vector<uint8_t> buffer(static_cast<std::size_t>(kPollBufferBytes));
    MonitorPollDesc desc {};

    desc.buffer = buffer.data();
    desc.bufferBytes = buffer.size();
    MonitorPollResult result {};

    const MonitorResult polled = pollOverride_
        ? pollOverride_(adapter_->sessionHandle(), &desc, &result)
        : adapter_->poll(adapter_->sessionHandle(), &desc, &result);
    failed.lastResult = polled;
    if (polled != kOk || result.recordsWritten == 0 || result.bytesWritten == 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastTick_.lastResult = polled;
        lastTick_.polledOffUiThread = offUi;
        return lastTick_;
    }
    auto batch = decodePackedRecords(buffer.data(), result.bytesWritten, result.recordsWritten);
    return ingestDecoded(std::move(batch), offUi);
}

MonitorPerformanceLiveTick MonitorPerformanceLiveHost::lastTick() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastTick_;
}

void MonitorPerformanceLiveHost::startPolling()
{
    if (polling_.exchange(true))
        return;
    stop_ = false;
    pollThread_ = std::thread([this]() { pollLoop(); });
}

void MonitorPerformanceLiveHost::stopPolling()
{
    stop_ = true;
    if (pollThread_.joinable())
        pollThread_.join();
    polling_ = false;
}

bool MonitorPerformanceLiveHost::isPolling() const
{
    return polling_.load();
}

bool MonitorPerformanceLiveHost::lastPollWasOffUiThread() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastTick_.polledOffUiThread;
}

void MonitorPerformanceLiveHost::pollLoop()
{
    while (!stop_.load())
    {
        ingestOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

} // namespace Monitor
} // namespace VNX
