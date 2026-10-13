#include "BusTopologyProjection.h"
#include "Monitor/MonitorTypes.h"
#include "MonitorReplayIntegrity.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace VNX {
namespace Monitor {
namespace {

struct BusKey
{
    std::uint64_t handle = 0;
    std::uint64_t generation = 0;

    bool operator<(const BusKey& other) const
    {
        if (handle != other.handle)
            return handle < other.handle;
        return generation < other.generation;
    }
};

} // namespace

bool busChildListPublished()
{
    return false;
}

bool busTapLevelsPublished()
{
    return false;
}

bool busExplicitImplicitSoloPublished()
{
    return false;
}

BusMonitorProjection projectBusRows(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot,
    std::optional<std::int64_t> executeFrame)
{
    BusMonitorProjection out;
    if (!snapshot)
        return out;

    std::vector<const MonitorDecodedRecord*> busState;
    bool sawBusLayout = false;
    for (const auto& rec : snapshot->records)
    {
        if (rec.header.entityKind != kMonitorEntityBus)
            continue;
        if (rec.header.recordKind != kMonitorRecordStateUpsert
            && rec.header.recordKind != kMonitorRecordStateTombstone)
            continue;
        sawBusLayout = true;
        busState.push_back(&rec);
    }
    if (!sawBusLayout)
        return out;
    if (entityReplayIsIncomplete(snapshot, kMonitorEntityBus))
    {
        out.tableState = MonitorBusesTableState::Incomplete;
        return out;
    }
    if (!executeFrame.has_value())
    {
        out.tableState = MonitorBusesTableState::NoLegalFrame;
        return out;
    }

    const auto cursorFrame = static_cast<std::uint64_t>(*executeFrame);
    std::sort(busState.begin(), busState.end(),
              [](const MonitorDecodedRecord* a, const MonitorDecodedRecord* b) {
                  if (a->header.sourceSequence != b->header.sourceSequence)
                      return a->header.sourceSequence < b->header.sourceSequence;
                  return a->header.recordKind < b->header.recordKind;
              });

    std::map<BusKey, BusMonitorRow> live;
    for (const MonitorDecodedRecord* rec : busState)
    {
        if (rec->header.executeFrame > cursorFrame)
            continue;
        BusKey key{rec->header.instanceHandle, rec->header.instanceGeneration};
        if (rec->header.recordKind == kMonitorRecordStateTombstone)
        {
            live.erase(key);
            continue;
        }
        BusMonitorRow row;
        row.instanceHandle = rec->header.instanceHandle;
        row.instanceGeneration = rec->header.instanceGeneration;
        row.stableId = rec->header.stableId;
        row.executeFrame = rec->header.executeFrame;
        row.storeGeneration = snapshot->generation;
        row.treeState = BusCellState::Unsupported;
        row.tapPostEffects = BusCellState::Unsupported;
        row.tapPreFader = BusCellState::Unsupported;
        row.tapPostFader = BusCellState::Unsupported;
        if (rec->payload.size() >= sizeof(MonitorStateUpsert))
        {
            MonitorStateUpsert payload {};
            std::memcpy(&payload, rec->payload.data(), sizeof(payload));
            row.routeBusId = payload.routeBusId;
            row.parentPublished = true;
            row.mute01 = payload.mute01;
            row.mutePublished = true;
            row.solo01 = payload.solo01;
            row.soloAuthoredPublished = true;
        }
        live[key] = row;
    }

    out.rows.reserve(live.size());
    for (auto& entry : live)
        out.rows.push_back(entry.second);
    out.treeViewSupported = false;
    out.tableState = out.rows.empty() ? MonitorBusesTableState::EmptyFrame
                                      : MonitorBusesTableState::Rows;
    return out;
}

} // namespace Monitor
} // namespace VNX
