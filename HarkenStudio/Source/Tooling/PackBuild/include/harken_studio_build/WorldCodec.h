#ifndef HARKEN_STUDIO_BUILD_CURRENT_WORLD_CODEC_H
#define HARKEN_STUDIO_BUILD_CURRENT_WORLD_CODEC_H

#include "harken_studio_build/World.h"
#include "harken_studio_build/WorldBodyVintage.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace harkenstudiobuild {

inline constexpr std::uint32_t kCurrentWorldV3PrefixWireBytes = 200;
inline constexpr std::uint32_t kCurrentWorldV6ActionCapacityWireBytes = 24;
inline constexpr std::uint32_t kCurrentWorldV9CapacityWireBytes = 148;
inline constexpr std::uint32_t kCurrentWorldActionRefWireBytes = 32;
inline constexpr std::uint32_t kCurrentWorldActionTimeWireBytes = 16;
inline constexpr std::uint32_t kCurrentWorldActionHeaderWireBytes = 44;
inline constexpr std::uint32_t kCurrentWorldActionStepWireBytes = 144;

enum class CurrentWorldBodyDecodeResult : std::uint8_t {
    Ok = 0,
    Invalid = 1,
    Capacity = 2,
};
bool EncodeCurrentWorldShippingBody(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;
class CurrentWorldBodyImage {
public:
    CurrentWorldBodyImage() noexcept;
    ~CurrentWorldBodyImage();
    CurrentWorldBodyImage(const CurrentWorldBodyImage&) = delete;
    CurrentWorldBodyImage& operator=(const CurrentWorldBodyImage&) = delete;
    CurrentWorldBodyDecodeResult DecodeWithResult(
        const std::uint8_t* bytes, std::size_t byteCount,
        std::uint32_t bodyVintage) noexcept;
    bool Decode(const std::uint8_t* bytes, std::size_t byteCount,
                std::uint32_t bodyVintage) noexcept {
        return DecodeWithResult(bytes, byteCount, bodyVintage) ==
               CurrentWorldBodyDecodeResult::Ok;
    }
    const ContainerWorldInstallViewV1* ProductionView() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_CURRENT_WORLD_CODEC_H
