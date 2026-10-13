#include "MonitorAdapter.h"
#include "Monitor/MonitorTypes.h"
#include "RuntimeBridge/MonitorBridge.h"
#include "MonitorCaptureControl.h"

#include <cstring>
#include <functional>
#include <utility>

namespace VNX {
namespace Monitor {
namespace {

bool principalUsable(const MonitorPrincipal& principal)
{
    return principal.authenticated && !principal.id.empty();
}

} // namespace

MonitorAdapter::MonitorAdapter(MonitorHostContext host)
    : host_(std::move(host))
{
}

MonitorResult MonitorAdapter::postControl(std::function<MonitorResult()> work)
{
    if (!host_.logicThreadPoster)
        return kMonitorWrongThread;

    MonitorResult result = kNotInitialized;
    bool ran = false;
    host_.logicThreadPoster([&]() {
        ran = true;
        result = work();
    });
    if (!ran)
        return kMonitorWrongThread;
    return result;
}

bool MonitorAdapter::isCurrentSession(MonitorSessionHandle handle) const
{
    return session_ && session_->handle == handle && handle != 0;
}

bool MonitorAdapter::isCurrentControl(MonitorSessionHandle handle, uint64_t epoch) const
{
    if (!isCurrentSession(handle))
        return false;
    if (epoch == 0)
        return true;
    return session_->epoch == epoch;
}

void MonitorAdapter::recordTrace(const char* entry)
{
    traces_.push_back(MonitorCallTrace{entry, std::this_thread::get_id()});
}

void MonitorAdapter::snapshotSession(MonitorSessionHandle* handle, uint64_t* epoch) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle != nullptr)
        *handle = session_ ? session_->handle : MonitorSessionHandle{0};
    if (epoch != nullptr)
        *epoch = session_ ? session_->epoch : uint64_t{0};
}

void MonitorAdapter::clearSessionLocked()
{
    session_.reset();
}

void MonitorAdapter::zeroSubscription(MonitorSubscriptionStatus* outStatus)
{
    if (outStatus == nullptr)
        return;

    std::memset(outStatus, 0, sizeof(*outStatus));

}

void MonitorAdapter::zeroPollResult(MonitorPollResult* outResult)
{
    if (outResult == nullptr)
        return;

    std::memset(outResult, 0, sizeof(*outResult));

}

void MonitorAdapter::zeroDropSummaries(MonitorDropSummary* outSummaries, uint32_t capacity)
{
    if (outSummaries == nullptr || capacity == 0)
        return;
    for (uint32_t i = 0; i < capacity; ++i)
    {
        const uint32_t structSize = outSummaries[i].structSize;
        std::memset(&outSummaries[i], 0, sizeof(outSummaries[i]));
        outSummaries[i].structSize = structSize;
    }
}

void MonitorAdapter::zeroCaptureControl(MonitorCaptureControlStatus* outStatus)
{
    zeroCaptureControlStatus(outStatus);
}

void MonitorAdapter::zeroEffective(MonitorEffectiveCapturePlan* outPlan)
{
    if (outPlan == nullptr)
        return;

    std::memset(outPlan, 0, sizeof(*outPlan));

}

void MonitorAdapter::zeroRecordingStatus(MonitorRecordingStatus* outStatus)
{
    if (outStatus == nullptr)
        return;

    std::memset(outStatus, 0, sizeof(*outStatus));

}

MonitorOpenResult MonitorAdapter::open(const MonitorSessionDesc* desc)
{
    MonitorOpenResult out;
    if (desc == nullptr)
    {
        out.result = kInvalidArg;
        return out;
    }
    if (!principalUsable(host_.principal))
    {
        out.result = kMonitorPermissionDenied;
        return out;
    }

    out.result = postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_)
            return kMonitorSessionCapacity;
        recordTrace("Vn_MonitorOpenSession");
        MonitorSessionHandle handle = 0;
        const MonitorResult opened = HarkenEditor::Bridge::Monitoring::openSession(desc, &handle);
        if (opened != kOk || handle == 0)
        {
            clearSessionLocked();
            return opened != kOk ? opened : kMonitorUnsupported;
        }

        Session session;
        session.handle = handle;
        session.epoch = nextEpoch_++;
        session.generation = nextGeneration_++;
        session_ = session;
        out.handle = session.handle;
        out.epoch = session.epoch;
        out.generation = session.generation;
        return opened;
    });
    if (out.result != kOk)
    {
        out.handle = 0;
        out.epoch = 0;
        out.generation = 0;
    }
    return out;
}

MonitorResult MonitorAdapter::close(MonitorSessionHandle handle)
{
    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentSession(handle))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorCloseSession");
        const MonitorResult closed = HarkenEditor::Bridge::Monitoring::closeSession(handle);
        acquiredPrincipalHash_ = 0;
        acquiredLeaseGeneration_ = 0;
        if (closed == kOk || closed == kMonitorUnsupported || closed == kNotInitialized
            || closed == kMonitorStaleSession)
        {
            clearSessionLocked();
        }
        return closed;
    });
}

MonitorResult MonitorAdapter::poll(MonitorSessionHandle handle,
                              const MonitorPollDesc* desc,
                              MonitorPollResult* outResult)
{
    zeroPollResult(outResult);
    if (desc == nullptr || outResult == nullptr)
        return kInvalidArg;

    if (desc->bufferBytes > 0 && desc->buffer == nullptr)
        return kInvalidArg;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!isCurrentSession(handle))
        return kMonitorStaleSession;
    if (session_->pollBound && session_->pollThread != std::this_thread::get_id())
        return kMonitorConcurrentPoll;
    session_->pollBound = true;
    session_->pollThread = std::this_thread::get_id();
    recordTrace("Vn_MonitorPoll");

    MonitorPollResult local = {};

    const MonitorResult polled = HarkenEditor::Bridge::Monitoring::poll(handle, desc, &local);
    if (polled == kOk)
        *outResult = local;
    else
        outResult->nextRecordBytes = local.nextRecordBytes;
    return polled;
}

MonitorResult MonitorAdapter::getSubscriptionStatus(MonitorSessionHandle handle,
                                               MonitorSubscriptionStatus* outStatus)
{
    zeroSubscription(outStatus);
    if (outStatus == nullptr)
        return kInvalidArg;

    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentSession(handle))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorGetSubscriptionStatus");
        MonitorSubscriptionStatus local = {};

        const MonitorResult status = HarkenEditor::Bridge::Monitoring::getSubscriptionStatus(handle, &local);
        if (status == kOk)
            *outStatus = local;
        return status;
    });
}

MonitorResult MonitorAdapter::getConsumerStatus(MonitorSessionHandle handle,
                                           MonitorDropSummary* outSummaries,
                                           uint32_t capacity,
                                           uint32_t* requiredCount)
{
    if (requiredCount != nullptr)
        *requiredCount = 0;
    zeroDropSummaries(outSummaries, capacity);
    if (capacity > 0 && outSummaries == nullptr)
        return kInvalidArg;

    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentSession(handle))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorGetConsumerStatus");
        return HarkenEditor::Bridge::Monitoring::getConsumerStatus(handle, outSummaries, capacity, requiredCount);
    });
}

MonitorResult MonitorAdapter::getCaptureControlStatus(MonitorSessionHandle handle,
                                                 MonitorCaptureControlStatus* outStatus)
{
    zeroCaptureControl(outStatus);
    if (outStatus == nullptr)
        return kInvalidArg;

    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentSession(handle))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorGetCaptureControlStatus");
        MonitorCaptureControlStatus local = {};

        const MonitorResult status = HarkenEditor::Bridge::Monitoring::getCaptureControlStatus(handle, &local);
        if (status == kOk)
            *outStatus = local;
        return status;
    });
}

MonitorResult MonitorAdapter::acquireCaptureControl(MonitorSessionHandle handle,
                                               MonitorCaptureControlStatus* outStatus,
                                               uint64_t epoch)
{
    zeroCaptureControl(outStatus);
    if (outStatus == nullptr)
        return kInvalidArg;

    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentControl(handle, epoch))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorAcquireCaptureControl");
        MonitorCaptureControlStatus local = {};

        const MonitorResult status = HarkenEditor::Bridge::Monitoring::acquireCaptureControl(handle, &local);
        if (status == kOk)
        {
            *outStatus = local;
            acquiredPrincipalHash_ = local.principalHash;
            acquiredLeaseGeneration_ = local.leaseGeneration;
        }
        return status;
    });
}

MonitorResult MonitorAdapter::releaseCaptureControl(MonitorSessionHandle handle,
                                               uint64_t leaseGeneration,
                                               uint64_t epoch)
{
    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentControl(handle, epoch))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorReleaseCaptureControl");
        const MonitorResult status = HarkenEditor::Bridge::Monitoring::releaseCaptureControl(handle, leaseGeneration);
        if (status == kOk)
        {
            acquiredPrincipalHash_ = 0;
            acquiredLeaseGeneration_ = 0;
        }
        return status;
    });
}

MonitorResult MonitorAdapter::validateCapturePlan(MonitorSessionHandle handle,
                                             uint64_t leaseGeneration,
                                             const MonitorCapturePlan* plan,
                                             MonitorEffectiveCapturePlan* outAccepted,
                                             uint64_t epoch)
{
    zeroEffective(outAccepted);
    if (plan == nullptr || outAccepted == nullptr)
        return kInvalidArg;


    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentControl(handle, epoch))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorValidateCapturePlan");
        MonitorEffectiveCapturePlan local = {};

        const MonitorResult status =
            HarkenEditor::Bridge::Monitoring::validateCapturePlan(handle, leaseGeneration, plan, &local);
        if (status == kOk)
            *outAccepted = local;
        return status;
    });
}

MonitorResult MonitorAdapter::applyCapturePlan(MonitorSessionHandle handle,
                                          uint64_t leaseGeneration,
                                          const MonitorCapturePlan* plan,
                                          MonitorEffectiveCapturePlan* outAccepted,
                                          uint64_t epoch)
{
    zeroEffective(outAccepted);
    if (plan == nullptr || outAccepted == nullptr)
        return kInvalidArg;


    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentControl(handle, epoch))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorApplyCapturePlan");
        MonitorEffectiveCapturePlan local = {};

        const MonitorResult status =
            HarkenEditor::Bridge::Monitoring::applyCapturePlan(handle, leaseGeneration, plan, &local);
        if (status == kOk)
            *outAccepted = local;
        return status;
    });
}

MonitorResult MonitorAdapter::stopCapture(MonitorSessionHandle handle,
                                     uint64_t leaseGeneration,
                                     MonitorEffectiveCapturePlan* outResult,
                                     uint64_t epoch)
{
    zeroEffective(outResult);
    if (outResult == nullptr)
        return kInvalidArg;

    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentControl(handle, epoch))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorStopCapture");
        MonitorEffectiveCapturePlan local = {};

        const MonitorResult status = HarkenEditor::Bridge::Monitoring::stopCapture(handle, leaseGeneration, &local);
        if (status == kOk)
            *outResult = local;
        return status;
    });
}

MonitorResult MonitorAdapter::startRecording(MonitorSessionHandle handle,
                                        uint64_t leaseGeneration,
                                        const MonitorRecordingDesc* desc,
                                        uint64_t epoch)
{
    if (desc == nullptr)
        return kInvalidArg;

    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentControl(handle, epoch))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorStartRecording");
        return HarkenEditor::Bridge::Monitoring::startRecording(handle, leaseGeneration, desc);
    });
}

MonitorResult MonitorAdapter::stopRecording(MonitorSessionHandle handle,
                                       uint64_t leaseGeneration,
                                       uint64_t epoch)
{
    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentControl(handle, epoch))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorStopRecording");
        return HarkenEditor::Bridge::Monitoring::stopRecording(handle, leaseGeneration);
    });
}

MonitorResult MonitorAdapter::getRecordingStatus(MonitorSessionHandle handle,
                                            MonitorRecordingStatus* outStatus)
{
    zeroRecordingStatus(outStatus);
    if (outStatus == nullptr)
        return kInvalidArg;

    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentSession(handle))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorGetRecordingStatus");
        MonitorRecordingStatus local = {};

        const MonitorResult status = HarkenEditor::Bridge::Monitoring::getRecordingStatus(handle, &local);
        if (status == kOk)
            *outStatus = local;
        return status;
    });
}

MonitorResult MonitorAdapter::getEffectiveCapturePlan(MonitorSessionHandle handle,
                                                 uint64_t capturePlanGeneration,
                                                 MonitorEffectiveCapturePlan* outPlan)
{
    zeroEffective(outPlan);
    if (outPlan == nullptr)
        return kInvalidArg;

    return postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCurrentSession(handle))
            return kMonitorStaleSession;
        recordTrace("Vn_MonitorGetEffectiveCapturePlan");
        MonitorEffectiveCapturePlan local = {};

        const MonitorResult status =
            HarkenEditor::Bridge::Monitoring::getEffectiveCapturePlan(handle, capturePlanGeneration, &local);
        if (status == kOk)
            *outPlan = local;
        return status;
    });
}

MonitorResult MonitorAdapter::readCapabilities(MonitorCapabilities* outCapabilities)
{
    if (outCapabilities == nullptr)
        return kInvalidArg;

    MonitorSessionHandle handleBefore = 0;
    uint64_t epochBefore = 0;
    snapshotSession(&handleBefore, &epochBefore);
    const MonitorResult result = postControl([&]() -> MonitorResult {
        std::lock_guard<std::mutex> lock(mutex_);
        recordTrace("Vn_MonitorGetCapabilities");
        return HarkenEditor::Bridge::Monitoring::getCapabilities(outCapabilities);
    });
    MonitorSessionHandle handleAfter = 0;
    uint64_t epochAfter = 0;
    snapshotSession(&handleAfter, &epochAfter);
    if (handleBefore != handleAfter || epochBefore != epochAfter)
        return kMonitorIncompatible;
    return result;
}

bool MonitorAdapter::hasOpenSession() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return session_.has_value();
}

MonitorSessionHandle MonitorAdapter::sessionHandle() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ ? session_->handle : MonitorSessionHandle{0};
}

uint64_t MonitorAdapter::sessionEpoch() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ ? session_->epoch : 0;
}

uint64_t MonitorAdapter::sessionGeneration() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ ? session_->generation : 0;
}

std::optional<MonitorPrincipal> MonitorAdapter::cachedPrincipal() const
{
    return std::nullopt;
}

uint64_t MonitorAdapter::acquiredPrincipalHash() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return acquiredPrincipalHash_;
}

uint64_t MonitorAdapter::acquiredLeaseGeneration() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return acquiredLeaseGeneration_;
}

std::vector<MonitorCallTrace> MonitorAdapter::callTrace() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return traces_;
}

} // namespace Monitor
} // namespace VNX
