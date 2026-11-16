#ifndef HARKEN_BUS_DEFINITION_FINGERPRINT_VALUE_H
#define HARKEN_BUS_DEFINITION_FINGERPRINT_VALUE_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace harken::busfingerprint {

inline constexpr std::uint64_t kOffsetBasis = UINT64_C(14695981039346656037);
inline constexpr std::uint64_t kPrime = UINT64_C(1099511628211);
inline constexpr std::uint64_t kKnownTargetCapabilityMaskV1 = UINT64_C(0x0000001F007F03FF);
inline constexpr std::uint64_t kRequiredBusCapabilityMaskV1 = UINT64_C(0x00770000);
inline constexpr std::uint32_t kNeutralFaderAfterSlotV1 = 8;

inline std::uint64_t Step(std::uint64_t hash, std::uint8_t byte) noexcept
{
    return (hash ^ byte) * kPrime;
}

inline std::uint64_t Finalize(std::uint64_t hash) noexcept
{
    return hash == 0 ? Step(hash, UINT8_C(0xA5)) : hash;
}

inline std::uint64_t HashBytes(const std::uint8_t* bytes,
                               std::size_t count) noexcept
{
    std::uint64_t hash = kOffsetBasis;
    for (std::size_t index = 0; index < count; ++index)
        hash = Step(hash, bytes[index]);
    return Finalize(hash);
}

template <typename Sink>
inline void AppendU32(Sink&& sink, std::uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index)
        sink(static_cast<std::uint8_t>(value >> (index * 8)));
}

template <typename Sink>
inline void AppendU64(Sink&& sink, std::uint64_t value)
{
    for (unsigned index = 0; index < 8; ++index)
        sink(static_cast<std::uint8_t>(value >> (index * 8)));
}

inline std::uint32_t CanonicalFloatBits(float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits == UINT32_C(0x80000000) ? 0 : bits;
}

template <typename Sink>
inline void AppendProfileV1(Sink&& sink, std::uint32_t vintage,
                            std::uint32_t sampleRate, std::uint32_t mixLayout,
                            std::uint32_t deviceLayout, std::uint64_t capabilityMask)
{
    AppendU32(sink, vintage);
    AppendU32(sink, sampleRate);
    AppendU32(sink, mixLayout);
    AppendU32(sink, deviceLayout);
    AppendU64(sink, capabilityMask);
}

inline std::uint64_t ProfileFingerprintV1(std::uint32_t vintage,
                                         std::uint32_t sampleRate,
                                         std::uint32_t mixLayout,
                                         std::uint32_t deviceLayout,
                                         std::uint64_t capabilityMask) noexcept
{
    std::uint64_t hash = kOffsetBasis;
    AppendProfileV1([&](std::uint8_t byte) { hash = Step(hash, byte); },
                    vintage, sampleRate, mixLayout, deviceLayout, capabilityMask);
    return Finalize(hash);
}

}

#endif
