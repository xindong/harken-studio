#ifndef HARKEN_STUDIO_BUILD_EFFECT_PARAM_LOCATOR_H
#define HARKEN_STUDIO_BUILD_EFFECT_PARAM_LOCATOR_H

#include <cstdint>

namespace harkenstudiobuild {

inline constexpr std::uint32_t kEffectParamLocatorIndexShift = 0;
inline constexpr std::uint32_t kEffectParamLocatorSlotShift = 6;
inline constexpr std::uint32_t kEffectParamLocatorTypeShift = 9;

inline constexpr std::uint32_t kEffectParamLocatorIndexMask = 0x3Fu;    // 6 bits
inline constexpr std::uint32_t kEffectParamLocatorSlotMask = 0x7u;      // 3 bits
inline constexpr std::uint32_t kEffectParamLocatorTypeMask = 0x7FFFFFu; // 23 bits
inline constexpr std::uint32_t kEffectParamLocatorMaxIndex =
    kEffectParamLocatorIndexMask; // 63
inline constexpr std::uint32_t kEffectParamLocatorMaxSlot =
    kEffectParamLocatorSlotMask; // 7
inline constexpr std::uint32_t kEffectParamLocatorMaxType =
    kEffectParamLocatorTypeMask; // 8388607

struct EffectParamLocator {
    std::uint32_t effectType;
    std::uint32_t slot;
    std::uint32_t paramIndex;
};
inline constexpr bool EffectParamLocatorInRange(std::uint32_t effectType,
                                                std::uint32_t slot,
                                                std::uint32_t paramIndex) {
    return effectType <= kEffectParamLocatorMaxType &&
           slot <= kEffectParamLocatorMaxSlot &&
           paramIndex <= kEffectParamLocatorMaxIndex;
}
inline constexpr std::uint32_t PackEffectParamLocator(std::uint32_t effectType,
                                                      std::uint32_t slot,
                                                      std::uint32_t paramIndex) {
    return ((effectType & kEffectParamLocatorTypeMask)
            << kEffectParamLocatorTypeShift) |
           ((slot & kEffectParamLocatorSlotMask)
            << kEffectParamLocatorSlotShift) |
           ((paramIndex & kEffectParamLocatorIndexMask)
            << kEffectParamLocatorIndexShift);
}
inline constexpr EffectParamLocator
UnpackEffectParamLocator(std::uint32_t targetId) {
    EffectParamLocator loc{};
    loc.paramIndex = (targetId >> kEffectParamLocatorIndexShift) &
                     kEffectParamLocatorIndexMask;
    loc.slot =
        (targetId >> kEffectParamLocatorSlotShift) & kEffectParamLocatorSlotMask;
    loc.effectType =
        (targetId >> kEffectParamLocatorTypeShift) & kEffectParamLocatorTypeMask;
    return loc;
}

} // namespace harken

#endif // HARKEN_STUDIO_BUILD_EFFECT_PARAM_LOCATOR_H
