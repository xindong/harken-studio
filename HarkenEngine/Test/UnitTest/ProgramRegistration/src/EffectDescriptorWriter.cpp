#include "harken_program_registration/EffectDescriptorWriter.h"

#include <cstring>

namespace harkenprogramreg {

EffectDescriptorWriteStatus WriteEffectDescriptor(
    VnEffectType type, std::uint32_t descriptorSize,
    std::uint32_t descriptorVintage, const float* params,
    std::uint32_t paramCount, EffectDescriptorBytes* out,
    VnObjectId effectId) {
    // The vintage gate first: it says whether the size relation below even
    // describes this type's layout, so checking the relation first would be
    // reading meaning into a number whose meaning is not yet established.
    if (descriptorVintage != kSupportedEffectDescriptorVintage) {
        return EffectDescriptorWriteStatus::VintageUnsupported;
    }
    if (paramCount > kMaxEffectDescriptorParams) {
        return EffectDescriptorWriteStatus::TooLarge;
    }
    const std::uint32_t expectedSize = EffectDescriptorSize(paramCount);
    if (descriptorSize != expectedSize) {
        return EffectDescriptorWriteStatus::LayoutUnsupported;
    }
    out->type = type;
    out->size = descriptorSize;
    std::memset(out->data, 0, sizeof(out->data));
    const VnEffectDesc header{descriptorSize, 0u, effectId};
    std::memcpy(out->data, &header, sizeof(header));
    if (paramCount > 0) {
        std::memcpy(out->data + kEffectDescriptorHeaderBytes, params,
                    static_cast<std::size_t>(paramCount) * sizeof(float));
    }
    return EffectDescriptorWriteStatus::Ok;
}

}  // namespace harkenprogramreg
