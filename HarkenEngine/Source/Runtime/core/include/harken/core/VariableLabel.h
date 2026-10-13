#pragma once

#include <harken/ObjectId.h>

#include <cstdint>
#include <string>

namespace harken {

struct VariableLabelDefinitionV1 {
    std::int32_t anchor = 0;
    std::string name;
    VnObjectId labelId{};
};

}
