#pragma once

#include <harken/Harken.h>
#include "harken/core/CurrentWorld.h"
#include "harken/media/PackLogicImage.h"

#include <string>
#include <vector>

namespace harken {

struct PackCompanionNames {
    struct Variable {
        VnVariableId id{};
        std::string name;
    };
    struct Label {
        VnVariableId variableId{};
        VnObjectId id{};
        std::string name;
    };
    VnObjectId projectId{};
    VnObjectId rootActionId{};
    std::string rootName;
    std::string language;
    std::vector<Variable> variables;
    std::vector<Label> labels;
    std::uint64_t digest = 0;
};

// Capture caller memory before invoking any byte provider. The budget is for
// this capture, not an all-project catalog or a preallocated name table.
VnResult CapturePackCompanionNames(const VnPackNamesV1& input,
    std::uint64_t& byteBudget, PackCompanionNames* out);

// The producer's physical pack identity check is shared by inspection and
// activation. It does not derive object GUIDs from names.
std::uint64_t CompanionActionPackId(std::uint64_t initPackId,
    VnObjectId rootActionId, std::string_view language) noexcept;

VnResult ValidatePackCompanionNames(const PackCompanionNames& names,
    std::uint32_t kind, std::uint64_t packId, std::uint64_t initPackId,
    const ContainerWorldInstallViewV1& view,
    const std::vector<PackCanonicalImage::VariablePolicyOverlay>& variables);

} // namespace harken
