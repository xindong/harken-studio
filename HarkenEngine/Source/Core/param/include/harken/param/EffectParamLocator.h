// Packed target locator for the effect-parameter binding scopes
// (VN_TARGET_SCOPE_VOICE_EFFECT_PARAM / VN_TARGET_SCOPE_BUS_EFFECT_PARAM). The
// public targetId of those scopes is this locator: it names which private
// parameter of which fx slot of which expected effect type a binding drives.
//
// Frozen bit layout (ABI): bits 0..5 the effect type's private parameter index
// (0..63), bits 6..8 the fx slot (0..7), bits 9..31 the expected effect type
// (the installed effect must match at resolution, else the binding is refused).
// The field widths bound what a locator can even express; a value that decodes
// to a slot beyond the object's fx-slot count, a parameter index beyond the
// installed type's parameter count, or a type that does not match the installed
// effect is a resolution-time refusal, not a decode error.
#ifndef HARKEN_PARAM_EFFECT_PARAM_LOCATOR_H
#define HARKEN_PARAM_EFFECT_PARAM_LOCATOR_H

#include <cstdint>

namespace harken {

inline constexpr std::uint32_t kEffectParamLocatorIndexShift = 0;
inline constexpr std::uint32_t kEffectParamLocatorSlotShift = 6;
inline constexpr std::uint32_t kEffectParamLocatorTypeShift = 9;

inline constexpr std::uint32_t kEffectParamLocatorIndexMask = 0x3Fu;    // 6 bits
inline constexpr std::uint32_t kEffectParamLocatorSlotMask = 0x7u;      // 3 bits
inline constexpr std::uint32_t kEffectParamLocatorTypeMask = 0x7FFFFFu; // 23 bits

// Inclusive maxima each field can represent in its bit range.
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

// True when every field fits its frozen bit range. A locator built from fields
// that do not fit cannot be represented, so the packer's callers range-check
// first and refuse out-of-range fields (never silently truncate).
inline constexpr bool EffectParamLocatorInRange(std::uint32_t effectType,
                                                std::uint32_t slot,
                                                std::uint32_t paramIndex) {
    return effectType <= kEffectParamLocatorMaxType &&
           slot <= kEffectParamLocatorMaxSlot &&
           paramIndex <= kEffectParamLocatorMaxIndex;
}

// Packs the three fields into a targetId. Fields are masked to their bit ranges,
// so callers MUST EffectParamLocatorInRange() first; packing an out-of-range
// field would alias a different locator.
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

// Decodes a targetId into its fields. Every uint32 decodes to in-range fields
// (the masks guarantee it); semantic validity (slot/index/type vs the installed
// effect) is checked separately at resolution.
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

#endif // HARKEN_PARAM_EFFECT_PARAM_LOCATOR_H
