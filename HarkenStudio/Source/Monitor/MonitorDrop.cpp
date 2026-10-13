#include "MonitorDrop.h"
#include "Monitor/MonitorTypes.h"

#include <cstring>
#include <map>
#include <utility>

namespace VNX {
namespace Monitor {
namespace {

template <typename T>
bool readPayload(const MonitorStoreQueryHit& hit, T* out)
{
    if (hit.payload.size() < sizeof(T))
        return false;
    std::memcpy(out, hit.payload.data(), sizeof(T));
    return out->structSize >= sizeof(T);
}

bool knownStage(uint32_t stage)
{
    return stage == kMonitorDropSourceProducer ||
           stage == kMonitorDropHubFanout ||
           stage == kMonitorDropRecorderQueue ||
           stage == kMonitorDropRemoteTransport ||
           stage == kMonitorDropDerivedInput;
}

using StreamKey = std::pair<uint64_t, uint64_t>;

} // namespace

MonitorDropProjection projectDrop(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot,
    const MonitorSubscriptionProjection& subscription,
    uint64_t storeLocalSaturationCount)
{
    MonitorDropProjection out;
    if (!snapshot)
        return out;

    if (subscription.state == MonitorSubscriptionProjectionState::Connected)
    {
        const auto mask = subscription.status.acceptedPermissionMask;
        if ((mask & kMonitorPermObserveState) == 0)
        {
            MonitorDropReason reason;
            reason.kind = MonitorDropReasonKind::UnauthorizedNotGap;
            reason.detail = "subscription-denied-not-gap";
            out.reasons.push_back(reason);
        }
    }

    if (storeLocalSaturationCount > 0)
    {
        MonitorDropReason reason;
        reason.kind = MonitorDropReasonKind::StoreLocalSaturation;
        reason.recordCount = storeLocalSaturationCount;
        reason.detail = "store-local-saturation";
        out.reasons.push_back(reason);
    }

    std::map<StreamKey, uint64_t> firstExpected;
    auto baselines = snapshot->findByRecordKind(kMonitorRecordStreamBaseline);
    for (const auto& hit : baselines)
    {
        MonitorStreamBaseline payload {};
        if (!readPayload(hit, &payload) || payload.streamId == 0)
        {
            MonitorDropReason reason;
            reason.kind = MonitorDropReasonKind::MalformedDrop;
            reason.epoch = hit.header.monitorSessionEpoch;
            reason.detail = "malformed-baseline";
            out.reasons.push_back(reason);
            continue;
        }
        firstExpected[{hit.header.monitorSessionEpoch, payload.streamId}] =
            payload.firstExpectedSourceSequence;
    }

    std::map<StreamKey, uint64_t> minSeq;
    for (const auto& rec : snapshot->records)
    {
        if (rec.header.streamId == 0)
            continue;
        const StreamKey key {rec.header.monitorSessionEpoch, rec.header.streamId};
        auto it = minSeq.find(key);
        if (it == minSeq.end() || rec.header.sourceSequence < it->second)
            minSeq[key] = rec.header.sourceSequence;
    }
    for (const auto& kv : minSeq)
    {
        if (firstExpected.find(kv.first) == firstExpected.end())
        {
            MonitorDropReason reason;
            reason.kind = MonitorDropReasonKind::BaselineMissing;
            reason.epoch = kv.first.first;
            reason.streamId = kv.first.second;
            reason.firstSourceSequence = kv.second;
            reason.detail = "baseline-missing";
            out.reasons.push_back(reason);
        }
    }

    auto drops = snapshot->findByRecordKind(kMonitorRecordDropSummary);
    for (const auto& hit : drops)
    {
        MonitorDropSummary payload {};
        MonitorDropReason reason;
        reason.epoch = hit.header.monitorSessionEpoch;
        if (!readPayload(hit, &payload) || !knownStage(payload.stage) ||
            payload.streamId == 0 ||
            payload.firstSourceSequence > payload.lastSourceSequence)
        {
            reason.kind = MonitorDropReasonKind::MalformedDrop;
            reason.detail = "malformed-drop";
            out.reasons.push_back(reason);
            continue;
        }
        reason.stage = payload.stage;
        reason.streamId = payload.streamId;
        reason.firstSourceSequence = payload.firstSourceSequence;
        reason.lastSourceSequence = payload.lastSourceSequence;
        reason.recordCount = payload.recordCount;
        if (payload.stage == kMonitorDropDerivedInput)
        {
            reason.kind = MonitorDropReasonKind::DerivedInput;
            reason.detail = "derived-input";
        }
        else
        {
            reason.kind = MonitorDropReasonKind::StageGap;
            reason.detail = "stage-gap";
        }
        out.reasons.push_back(reason);
    }

    return out;
}

} // namespace Monitor
} // namespace VNX
