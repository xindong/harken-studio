#pragma once

#include <harken/Harken.h>
#include "harken/core/PcmView.h"

namespace harken {

VnResult RegisterPcmWithLoopRegionV1(VnSoundId soundId, const VnPCMDesc& desc,
                                    PcmLoopRegion loop,
                                    std::uint64_t* outContentRevision);

}
