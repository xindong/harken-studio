#include "harken/render/InsertEffect.h"

namespace harken {

namespace {

void PassthroughProcess(void* /*state*/, float* const* /*channels*/,
                        std::uint32_t /*channelCount*/,
                        std::uint32_t /*offset*/, std::uint32_t /*frames*/) {
    // Identity: y = x.
}

std::uint32_t PassthroughTail(const void* /*state*/) {
    return 0;
}

} // namespace

InsertEffect MakePassthroughEffect() {
    InsertEffect fx;
    fx.process = PassthroughProcess;
    fx.tailFrames = PassthroughTail;
    fx.state = nullptr;
    return fx;
}

} // namespace harken
