#pragma once

#include "MonitorHostContext.h"

#include "Monitor/MonitorTypes.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace VNX {
namespace Monitor {

struct MonitorOpenResult
{
    MonitorResult result = kNotInitialized;
    MonitorSessionHandle handle = 0;
    uint64_t epoch = 0;
    uint64_t generation = 0;
};

struct MonitorCallTrace
{
    const char* entry = nullptr;
    std::thread::id threadId;
};

class MonitorAdapter
{
public:
    explicit MonitorAdapter(MonitorHostContext host);

    MonitorAdapter(const MonitorAdapter&) = delete;
    MonitorAdapter& operator=(const MonitorAdapter&) = delete;

    MonitorOpenResult open(const MonitorSessionDesc* desc);
    MonitorResult close(MonitorSessionHandle handle);
    MonitorResult poll(MonitorSessionHandle handle,
                  const MonitorPollDesc* desc,
                  MonitorPollResult* outResult);
    MonitorResult getSubscriptionStatus(MonitorSessionHandle handle,
                                   MonitorSubscriptionStatus* outStatus);
    MonitorResult getConsumerStatus(MonitorSessionHandle handle,
                               MonitorDropSummary* outSummaries,
                               uint32_t capacity,
                               uint32_t* requiredCount);
    MonitorResult getCaptureControlStatus(MonitorSessionHandle handle,
                                     MonitorCaptureControlStatus* outStatus);
    MonitorResult acquireCaptureControl(MonitorSessionHandle handle,
                                   MonitorCaptureControlStatus* outStatus,
                                   uint64_t epoch = 0);
    MonitorResult releaseCaptureControl(MonitorSessionHandle handle,
                                   uint64_t leaseGeneration,
                                   uint64_t epoch = 0);
    MonitorResult validateCapturePlan(MonitorSessionHandle handle,
                                 uint64_t leaseGeneration,
                                 const MonitorCapturePlan* plan,
                                 MonitorEffectiveCapturePlan* outAccepted,
                                 uint64_t epoch = 0);
    MonitorResult applyCapturePlan(MonitorSessionHandle handle,
                              uint64_t leaseGeneration,
                              const MonitorCapturePlan* plan,
                              MonitorEffectiveCapturePlan* outAccepted,
                              uint64_t epoch = 0);
    MonitorResult stopCapture(MonitorSessionHandle handle,
                         uint64_t leaseGeneration,
                         MonitorEffectiveCapturePlan* outResult,
                         uint64_t epoch = 0);
    MonitorResult getEffectiveCapturePlan(MonitorSessionHandle handle,
                                     uint64_t capturePlanGeneration,
                                     MonitorEffectiveCapturePlan* outPlan);
    MonitorResult startRecording(MonitorSessionHandle handle,
                            uint64_t leaseGeneration,
                            const MonitorRecordingDesc* desc,
                            uint64_t epoch = 0);
    MonitorResult stopRecording(MonitorSessionHandle handle,
                           uint64_t leaseGeneration,
                           uint64_t epoch = 0);
    MonitorResult getRecordingStatus(MonitorSessionHandle handle,
                                MonitorRecordingStatus* outStatus);

    MonitorResult readCapabilities(MonitorCapabilities* outCapabilities);
    uint64_t acquiredPrincipalHash() const;
    uint64_t acquiredLeaseGeneration() const;

    bool hasOpenSession() const;
    MonitorSessionHandle sessionHandle() const;
    uint64_t sessionEpoch() const;
    uint64_t sessionGeneration() const;
    std::optional<MonitorPrincipal> cachedPrincipal() const;
    std::vector<MonitorCallTrace> callTrace() const;

private:
    struct Session
    {
        MonitorSessionHandle handle = 0;
        uint64_t epoch = 0;
        uint64_t generation = 0;
        bool pollBound = false;
        std::thread::id pollThread;
    };

    MonitorResult postControl(std::function<MonitorResult()> work);
    bool isCurrentSession(MonitorSessionHandle handle) const;
    bool isCurrentControl(MonitorSessionHandle handle, uint64_t epoch) const;
    void recordTrace(const char* entry);
    void snapshotSession(MonitorSessionHandle* handle, uint64_t* epoch) const;
    void clearSessionLocked();
    static void zeroSubscription(MonitorSubscriptionStatus* outStatus);
    static void zeroPollResult(MonitorPollResult* outResult);
    static void zeroDropSummaries(MonitorDropSummary* outSummaries, uint32_t capacity);
    static void zeroCaptureControl(MonitorCaptureControlStatus* outStatus);
    static void zeroEffective(MonitorEffectiveCapturePlan* outPlan);
    static void zeroRecordingStatus(MonitorRecordingStatus* outStatus);

    MonitorHostContext host_;
    mutable std::mutex mutex_;
    std::optional<Session> session_;
    std::vector<MonitorCallTrace> traces_;
    uint64_t nextEpoch_ = 1;
    uint64_t nextGeneration_ = 1;
    uint64_t acquiredPrincipalHash_ = 0;
    uint64_t acquiredLeaseGeneration_ = 0;
};

} // namespace Monitor
} // namespace VNX
