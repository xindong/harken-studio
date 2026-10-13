#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace harkenstudiobuild::busfingerprint {

inline constexpr std::uint64_t kOffsetBasis = UINT64_C(14695981039346656037);
inline constexpr std::uint64_t kPrime = UINT64_C(1099511628211);
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

inline std::uint64_t HashBytes(const std::uint8_t* bytes, std::size_t count) noexcept
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
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return bits == UINT32_C(0x80000000) ? 0 : bits;
}

inline std::uint64_t ProfileFingerprintV1(std::uint32_t vintage,
                                         std::uint32_t sampleRate,
                                         std::uint32_t mixLayout,
                                         std::uint32_t deviceLayout,
                                         std::uint64_t capabilityMask) noexcept
{
    std::uint64_t hash = kOffsetBasis;
    const auto byte = [&](std::uint8_t value) { hash = Step(hash, value); };
    AppendU32(byte, vintage);
    AppendU32(byte, sampleRate);
    AppendU32(byte, mixLayout);
    AppendU32(byte, deviceLayout);
    AppendU64(byte, capabilityMask);
    return Finalize(hash);
}

}
