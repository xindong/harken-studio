#pragma once

#include <harken/Harken.h>
#include "harken/core/PcmView.h"

namespace harken {

VnResult RegisterPcmWithLoopRegionV1(VnSoundId soundId, const VnPCMDesc& desc,
                                    PcmLoopRegion loop,
                                    std::uint64_t* outContentRevision);

#if HARKEN_ENGINE_DEV
VnResult RegisterResidentVorbis(VnSoundId soundId, const VnPackByteSpan& media,
                                std::uint64_t& outContentRevision);
#endif

}
