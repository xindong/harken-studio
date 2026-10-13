#include "EffectMonitorProjection.h"
#include "Monitor/MonitorTypes.h"
#include "MonitorReplayIntegrity.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace VNX {
namespace Monitor {
namespace {

struct EffectKey
{
    std::uint64_t handle = 0;
    std::uint64_t generation = 0;

    bool operator<(const EffectKey& other) const
    {
        if (handle != other.handle)
            return handle < other.handle;
        return generation < other.generation;
    }
};

bool hasNumeric(std::uint32_t validityFlags)
{
    return (validityFlags & kMonitorValidNumeric) != 0
        && (validityFlags & kMonitorValidUnsupported) == 0;
}

} // namespace

bool effectParamTriStatePublished()
{
    return false;
}

bool effectGainReductionRecordPublished()
{
    return false;
}

EffectMonitorProjection projectEffectRows(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot,
    std::optional<std::int64_t> executeFrame)
{
    EffectMonitorProjection out;
    if (!snapshot)
        return out;

    std::vector<const MonitorDecodedRecord*> effectState;
    bool sawEffectLayout = false;
    for (const auto& rec : snapshot->records)
    {
        if (rec.header.entityKind != kMonitorEntityEffect)
            continue;
        if (rec.header.recordKind != kMonitorRecordStateUpsert
            && rec.header.recordKind != kMonitorRecordStateTombstone)
            continue;
        sawEffectLayout = true;
        effectState.push_back(&rec);
    }
    if (!sawEffectLayout)
        return out;
    if (entityReplayIsIncomplete(snapshot, kMonitorEntityEffect))
    {
        out.tableState = MonitorEffectsTableState::Incomplete;
        return out;
    }
    if (!executeFrame.has_value())
    {
        out.tableState = MonitorEffectsTableState::NoLegalFrame;
        return out;
    }

    const auto cursorFrame = static_cast<std::uint64_t>(*executeFrame);
    std::sort(effectState.begin(), effectState.end(),
              [](const MonitorDecodedRecord* a, const MonitorDecodedRecord* b) {
                  if (a->header.sourceSequence != b->header.sourceSequence)
                      return a->header.sourceSequence < b->header.sourceSequence;
                  return a->header.recordKind < b->header.recordKind;
              });

    std::map<EffectKey, EffectMonitorRow> live;
    for (const MonitorDecodedRecord* rec : effectState)
    {
        if (rec->header.executeFrame > cursorFrame)
            continue;
        EffectKey key{rec->header.instanceHandle, rec->header.instanceGeneration};
        if (rec->header.recordKind == kMonitorRecordStateTombstone)
        {
            live.erase(key);
            continue;
        }
        EffectMonitorRow row;
        row.instanceHandle = rec->header.instanceHandle;
        row.instanceGeneration = rec->header.instanceGeneration;
        row.stableId = rec->header.stableId;
        row.executeFrame = rec->header.executeFrame;
        row.storeGeneration = snapshot->generation;
        row.paramsState = EffectCellState::Unavailable;
        row.grState = EffectCellState::Unsupported;
        if (rec->payload.size() >= sizeof(MonitorStateUpsert))
        {
            MonitorStateUpsert payload {};
            std::memcpy(&payload, rec->payload.data(), sizeof(payload));
            row.bypass01 = payload.bypass01;
            row.bypassPublished = true;
            row.tailFrames = payload.tailFrames;
            row.tailPublished = true;
            row.latencyFrames = payload.latencyFrames;
            row.latencyState = hasNumeric(rec->header.validityFlags)
                ? EffectCellState::Numeric
                : EffectCellState::Gap;
        }
        else
        {
            row.latencyState = EffectCellState::Gap;
        }
        live[key] = row;
    }

    out.rows.reserve(live.size());
    for (auto& entry : live)
        out.rows.push_back(entry.second);
    out.tableState = out.rows.empty() ? MonitorEffectsTableState::EmptyFrame
                                      : MonitorEffectsTableState::Rows;
    return out;
}

} // namespace Monitor
} // namespace VNX
