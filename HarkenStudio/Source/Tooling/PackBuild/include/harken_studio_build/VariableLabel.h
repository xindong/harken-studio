#pragma once

#include <harken_studio/ObjectId.h>

#include <cstdint>
#include <string>

namespace harken::studio::build {

struct VariableLabelDefinitionV1 {
    std::int32_t anchor = 0;
    std::string name;
    harken::studio::types::ObjectId labelId{};
};

}
