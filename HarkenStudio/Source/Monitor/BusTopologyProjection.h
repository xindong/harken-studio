#pragma once

#include "MonitorStore.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace VNX {
namespace Monitor {

enum class BusCellState
{
    Numeric = 0,
    Gap,
    Unsupported,
};

enum class MonitorBusesTableState
{
    UnsupportedLayout = 0,
    NoLegalFrame,
    EmptyFrame,
    Rows,
    Incomplete,
};

struct BusMonitorRow
{
    std::uint64_t instanceHandle = 0;
    std::uint64_t instanceGeneration = 0;
    harkenstudio::ObjectId stableId = {};
    std::uint64_t executeFrame = 0;
    harkenstudio::ObjectId routeBusId{};
    bool parentPublished = false;
    std::uint32_t mute01 = 0;
    bool mutePublished = false;
    std::uint32_t solo01 = 0;
    bool soloAuthoredPublished = false;
    BusCellState treeState = BusCellState::Unsupported;
    BusCellState tapPostEffects = BusCellState::Unsupported;
    BusCellState tapPreFader = BusCellState::Unsupported;
    BusCellState tapPostFader = BusCellState::Unsupported;
    std::uint64_t storeGeneration = 0;
};

struct BusMonitorProjection
{
    MonitorBusesTableState tableState = MonitorBusesTableState::UnsupportedLayout;
    bool treeViewSupported = false;
    std::vector<BusMonitorRow> rows;
};

bool busChildListPublished();
bool busTapLevelsPublished();
bool busExplicitImplicitSoloPublished();
BusMonitorProjection projectBusRows(
    const std::shared_ptr<const MonitorStoreSnapshot>& snapshot,
    std::optional<std::int64_t> executeFrame);

} // namespace Monitor
} // namespace VNX
