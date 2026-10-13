#pragma once

#include "MonitorAdapter.h"
#include "Monitor/MonitorTypes.h"
#include "MonitorStore.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace VNX {
namespace Monitor {

struct MonitorPerformanceLiveSample
{
    uint64_t streamId = 0;
    uint64_t executeFrame = 0;
    uint64_t sourceSequence = 0;
    uint64_t capturePlanGeneration = 0;
    uint32_t metricId = 0;
    uint32_t valueBits = 0;
    uint32_t validityFlags = 0;
};

struct MonitorPerformanceLiveTick
{
    std::vector<MonitorPerformanceLiveSample> samples;
    bool hasLiveFrame = false;
    std::int64_t liveExecuteFrame = 0;
    uint64_t capturePlanGeneration = 0;
    uint64_t sourceSequence = 0;
    bool polledOffUiThread = true;
    MonitorResult lastResult = kNotInitialized;
};

class MonitorPerformanceLiveHost
{
public:
    using PollFn = std::function<MonitorResult(MonitorSessionHandle,
                                          const MonitorPollDesc*,
                                          MonitorPollResult*)>;
    using ThreadCheckFn = std::function<bool()>;
    using TickFn = std::function<void(const MonitorPerformanceLiveTick&)>;

    explicit MonitorPerformanceLiveHost(MonitorAdapter* adapter = nullptr);
    ~MonitorPerformanceLiveHost();

    MonitorPerformanceLiveHost(const MonitorPerformanceLiveHost&) = delete;
    MonitorPerformanceLiveHost& operator=(const MonitorPerformanceLiveHost&) = delete;

    void setPollOverride(PollFn poll);
    void setUiThreadCheck(ThreadCheckFn check);
    void setTickHandler(TickFn handler);
    bool openObservationSession();
    void closeObservationSession();
    bool hasOpenSession() const;
    MonitorSessionHandle sessionHandle() const;
    uint32_t acceptedPermissionMask() const;
    uint32_t resolvedStreamKindMask() const;

    MonitorPerformanceLiveTick ingestOnce();
    MonitorPerformanceLiveTick ingestPacked(const uint8_t* packedBytes,
                                            uint64_t bytesWritten,
                                            uint32_t expectedRecords);
    MonitorPerformanceLiveTick lastTick() const;

    void startPolling();
    void stopPolling();
    bool isPolling() const;
    bool lastPollWasOffUiThread() const;

    static bool isNumeric(uint32_t validityFlags);
    static const char* seriesIdForStream(uint64_t streamId);
    static bool isKnownStream(uint64_t streamId);

private:
    MonitorPerformanceLiveTick ingestDecoded(MonitorDecodeBatch&& batch, bool offUiThread);
    void pollLoop();
    MonitorCapabilities storeCaps() const;

    MonitorAdapter* adapter_ = nullptr;
    PollFn pollOverride_;
    ThreadCheckFn uiThreadCheck_;
    TickFn tickHandler_;
    MonitorStore store_;
    mutable std::mutex mutex_;
    MonitorPerformanceLiveTick lastTick_;
    std::thread pollThread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> polling_{false};
    uint32_t acceptedPermissionMask_ = 0;
    uint32_t resolvedStreamKindMask_ = 0;
};

} // namespace Monitor
} // namespace VNX
