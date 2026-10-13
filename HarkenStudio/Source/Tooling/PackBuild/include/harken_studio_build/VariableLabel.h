#pragma once

#include <harken_studio/ObjectId.h>

#include <cstdint>
#include <string>

namespace harkenstudiobuild {

struct VariableLabelDefinitionV1 {
    std::int32_t anchor = 0;
    std::string name;
    harkenstudio::ObjectId labelId{};
};

}
