#pragma once

#include <harken/ObjectId.h>

#include <cstdint>

namespace harken {

struct VariableLabelDefinitionV1 {
    std::int32_t anchor = 0;
    VnObjectId labelId{};
};

}
