#include "MonitorGeneration.h"
#include "Monitor/MonitorTypes.h"

#include <algorithm>
#include <map>
#include <utility>

namespace VNX {
namespace Monitor {

MonitorGenerationProjection projectGeneration(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot)
{
    MonitorGenerationProjection out;
    if (!snapshot)
    {
        out.status = MonitorGenerationStatus::ActivationMissing;
        out.reason = "empty-snapshot";
        return out;
    }

    auto hits = snapshot->findByRecordKind(kMonitorRecordCapturePlanActivated);
    if (hits.empty())
    {
        out.status = MonitorGenerationStatus::ActivationMissing;
        out.reason = "activation-missing";
        return out;
    }

    std::stable_sort(hits.begin(), hits.end(),
                     [](const MonitorStoreQueryHit& a, const MonitorStoreQueryHit& b) {
                         if (a.header.monitorSessionEpoch != b.header.monitorSessionEpoch)
                             return a.header.monitorSessionEpoch < b.header.monitorSessionEpoch;
                         if (a.header.streamId != b.header.streamId)
                             return a.header.streamId < b.header.streamId;
                         if (a.header.sourceSequence != b.header.sourceSequence)
                             return a.header.sourceSequence < b.header.sourceSequence;
                         return a.header.executeFrame < b.header.executeFrame;
                     });

    std::map<std::pair<uint64_t, uint64_t>, MonitorGenerationBound> latest;
    for (const auto& hit : hits)
    {
        MonitorGenerationBound bound;
        bound.epoch = hit.header.monitorSessionEpoch;
        bound.streamId = hit.header.streamId;
        bound.capturePlanGeneration = hit.header.capturePlanGeneration;
        bound.executeFrame = hit.header.executeFrame;
        latest[{bound.epoch, bound.streamId}] = bound;
    }
    out.bounds.reserve(latest.size());
    for (const auto& kv : latest)
        out.bounds.push_back(kv.second);
    out.status = MonitorGenerationStatus::Bounded;
    out.reason = "activated-execute-frame";
    return out;
}

} // namespace Monitor
} // namespace VNX
