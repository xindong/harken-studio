#include <harken/Harken.h>
#pragma once

#include "Monitor/MonitorDecoder.h"

#include <string>
#include <vector>

namespace VNX {
namespace Monitor {
namespace TestSupport {

struct LoadedFixture
{
    VnResult status = VN_NOT_INITIALIZED;
    std::vector<uint8_t> packed;
    uint32_t expectedRecords = 0;
    std::string expected;
};

LoadedFixture loadRuntimeLegalEvent();
LoadedFixture loadLifecycleLegalEvent();
LoadedFixture loadEditorFixture(const std::string& fileName);

} // namespace TestSupport
} // namespace Monitor
} // namespace VNX
