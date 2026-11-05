#pragma once

#include "harken/media/PackFormatV2.h"

#include <set>
#include <string>

namespace harken {

inline constexpr std::uint32_t kVariableNameCatalogMaxRows = 8192;

struct PackVariableNameRow {
    VnObjectId variableId{};
    std::string name;
};

struct PackVariableNameCatalog {
    std::array<std::uint8_t, 16> projectGuid{};
    std::uint64_t initPackStableId = 0;
    std::uint64_t initContentRevision = 0;
    std::vector<PackVariableNameRow> rows;
};

inline bool IsSdkCallableName(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    const auto letter = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (!letter(static_cast<unsigned char>(name.front()))) return false;
    for (const unsigned char c : name) {
        if (!letter(c) && (c < '0' || c > '9') && c != '_') return false;
    }
    return true;
}

inline bool ValidateVariableNameCatalog(const PackVariableNameCatalog& catalog) {
    if (std::all_of(catalog.projectGuid.begin(), catalog.projectGuid.end(),
                    [](auto byte) { return byte == 0; }) ||
        catalog.initPackStableId == 0 || catalog.initContentRevision == 0 ||
        catalog.rows.size() > kVariableNameCatalogMaxRows) return false;
    std::set<VnObjectId> active;
    std::string_view prior;
    for (const auto& row : catalog.rows) {
        if (row.variableId == VnObjectId{} || !IsSdkCallableName(row.name) ||
            (!prior.empty() && prior >= row.name)) return false;
        if (!active.insert(row.variableId).second) return false;
        prior = row.name;
    }
    return true;
}

}
