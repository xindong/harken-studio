#pragma once

#include "MonitorStore.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace VNX {
namespace Monitor {

enum class EffectCellState
{
    Numeric = 0,
    Gap,
    Unsupported,
    Unavailable,
};

enum class MonitorEffectsTableState
{
    UnsupportedLayout = 0,
    NoLegalFrame,
    EmptyFrame,
    Rows,
    Incomplete,
};

struct EffectMonitorRow
{
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    harkenstudio::ObjectId stableId = {};
    std::uint64_t executeFrame = 0;
    std::uint32_t bypass01 = 0;
    bool bypassPublished = false;
    std::uint32_t tailFrames = 0;
    bool tailPublished = false;
    std::uint32_t latencyFrames = 0;
    EffectCellState latencyState = EffectCellState::Gap;
    EffectCellState paramsState = EffectCellState::Unavailable;
    EffectCellState grState = EffectCellState::Unsupported;
    std::uint64_t storeGeneration = 0;
};

struct EffectMonitorProjection
{
    MonitorEffectsTableState tableState = MonitorEffectsTableState::UnsupportedLayout;
    std::vector<EffectMonitorRow> rows;
};

bool effectParamTriStatePublished();
bool effectGainReductionRecordPublished();
EffectMonitorProjection projectEffectRows(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot,
    std::optional<std::int64_t> executeFrame);

} // namespace Monitor
} // namespace VNX
