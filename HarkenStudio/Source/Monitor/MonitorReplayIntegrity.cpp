#include "MonitorReplayIntegrity.h"
#include "Monitor/MonitorTypes.h"

#include "MonitorDrop.h"
#include "MonitorStateCut.h"

#include <set>
#include <utility>

namespace VNX {
namespace Monitor {

bool entityReplayIsIncomplete(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot,
    std::uint32_t entityKind)
{
    if (!snapshot)
        return true;

    std::set<std::pair<std::uint64_t, std::uint64_t>> streams;
    for (const auto& rec : snapshot->records)
    {
        if (rec.header.entityKind != entityKind)
            continue;
        if (rec.header.recordKind != kMonitorRecordStateUpsert
            && rec.header.recordKind != kMonitorRecordStateTombstone)
            continue;
        if (rec.header.streamId == 0)
            continue;
        streams.emplace(rec.header.monitorSessionEpoch, rec.header.streamId);
    }

    const auto drops = projectDrop(snapshot);
    for (const auto& reason : drops.reasons)
    {
        switch (reason.kind)
        {
        case MonitorDropReasonKind::StageGap:
        case MonitorDropReasonKind::StoreLocalSaturation:
        case MonitorDropReasonKind::MalformedDrop:
            return true;
        case MonitorDropReasonKind::BaselineMissing:
            if (streams.count({reason.epoch, reason.streamId}) != 0)
                return true;
            break;
        default:
            break;
        }
    }
    return projectStateCut(snapshot).mandatoryResync;
}

} // namespace Monitor
} // namespace VNX
