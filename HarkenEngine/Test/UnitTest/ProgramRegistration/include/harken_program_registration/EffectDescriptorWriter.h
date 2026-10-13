// Building a concrete effect descriptor from an index-ordered parameter array.
//
// The effect-installation seam takes a concrete per-type descriptor struct, while
// a compiled chain carries parameter values addressed by the effect type's
// private parameter index. Nothing published converts the second into the first:
// the engine owns one direction only (a descriptor to an index-ordered array),
// and that mapping is declared to be its single source. This is the other
// direction, written once here rather than per host.
//
// Pure and host-agnostic: it reads a parameter array and writes bytes, touching
// no engine state, no voice and no bus. A bus-side installer wanting the same
// descriptor calls the same function.
#ifndef HARKEN_PROGRAM_REGISTRATION_EFFECT_DESCRIPTOR_WRITER_H
#define HARKEN_PROGRAM_REGISTRATION_EFFECT_DESCRIPTOR_WRITER_H

#include <harken/Harken.h>

#include <cstdint>

namespace harkenprogramreg {

// The descriptor layout vintage this writer knows how to produce: the header
// followed by parameters as floats in private index order. A type reporting any
// other vintage carries a layout this writer has never seen and is refused.
inline constexpr std::uint32_t kSupportedEffectDescriptorVintage = 2;

// The parameter capacity the descriptor buffer below is sized for. Deliberately
// far above any built-in effect's parameter count and stated as a count rather
// than a list of the current descriptor types, so adding an effect does not
// silently outgrow the buffer: a type needing more is refused by the capacity
// check, never truncated.
inline constexpr std::uint32_t kMaxEffectDescriptorParams = 32;

inline constexpr std::uint32_t kEffectDescriptorHeaderBytes = sizeof(VnEffectDesc);

constexpr std::uint32_t EffectDescriptorSize(std::uint32_t parameterCount) {
    const std::uint32_t size = kEffectDescriptorHeaderBytes + 4u * parameterCount;
    constexpr auto alignment = static_cast<std::uint32_t>(alignof(VnEffectDesc));
    return (size + alignment - 1u) & ~(alignment - 1u);
}

static_assert(kEffectDescriptorHeaderBytes == sizeof(VnEffectDesc),
              "the descriptor header size must match the declared header");

inline constexpr std::uint32_t kMaxEffectDescriptorBytes =
    EffectDescriptorSize(kMaxEffectDescriptorParams);

// A built descriptor: the effect type it describes, the byte length the engine
// will check against its own struct size, and the bytes themselves. Aligned to
// the descriptor header's requirement so the buffer can be read as a
// VnEffectDesc, which is how the installation seam takes it.
struct EffectDescriptorBytes {
    VnEffectType type = 0;
    std::uint32_t size = 0;
    alignas(alignof(VnEffectDesc)) unsigned char data[kMaxEffectDescriptorBytes] = {};

    const VnEffectDesc* AsDesc() const {
        return reinterpret_cast<const VnEffectDesc*>(data);
    }
};

// Why a descriptor could not be built. Each is a property of the type's captured
// capability row or of the buffer, never of a parameter's value.
enum class EffectDescriptorWriteStatus : std::uint32_t {
    Ok = 0,
    // descriptorSize is not the header plus one float per parameter, so the
    // type's descriptor is not the layout this writer produces.
    LayoutUnsupported = 1,
    // The type reports a descriptor vintage this writer does not know.
    VintageUnsupported = 2,
    // The descriptor is larger than the buffer this writer is sized for.
    TooLarge = 3,
};

EffectDescriptorWriteStatus WriteEffectDescriptor(
    VnEffectType type, std::uint32_t descriptorSize,
    std::uint32_t descriptorVintage, const float* params,
    std::uint32_t paramCount, EffectDescriptorBytes* out,
    VnObjectId effectId = {});

}  // namespace harkenprogramreg

#endif  // HARKEN_PROGRAM_REGISTRATION_EFFECT_DESCRIPTOR_WRITER_H
