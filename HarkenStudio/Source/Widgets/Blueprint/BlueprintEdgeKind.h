
#pragma once

#include "../../Core/ProjectTypes.h"

#include <cstdint>
#include <vector>

namespace VNX {

struct ProjectData;

inline const char* sendModeLabel(SendMode mode)
{
    switch (mode) {
    case SendMode::Mix: return "Mix";
    case SendMode::Sidechain: return "Sidechain";
    }
    return "";
}

enum class BlueprintEdgeKind : uint8_t
{
    Containment = 0,
    BusRoute    = 1,
    Override    = 2,
    Send        = 3,
    Placement   = 4,
    Organization = 5,
    Through      = 6
};

struct BlueprintEdgeView
{
    uint32_t          fromItemId = 0;
    uint32_t          toItemId   = 0;
    BlueprintEdgeKind kind       = BlueprintEdgeKind::Containment;

    uint32_t          sendIndex  = 0;
    SendMode          sendMode = SendMode::Mix;
};

std::vector<BlueprintEdgeView> deriveAllEdges(const ProjectItem& root);

std::vector<BlueprintEdgeView> deriveEdgesForItem(const ProjectItem& item,
                                                  const ProjectItem* parent);

std::vector<BlueprintEdgeView> deriveAllEdges(const ProjectData& project);

}
