#ifndef HARKEN_CORE_CURRENT_WORLD_CODEC_H
#define HARKEN_CORE_CURRENT_WORLD_CODEC_H

#include "harken/core/CurrentWorld.h"
#include "harken/media/CurrentWorldBodyVintage.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace harken {

enum class CurrentWorldDecodeMode : std::uint8_t {
    StrictArtifact = 0,
    ReplayRawAttempt = 1,
    ReplayCurrentAttempt = 2,
};

enum class CurrentWorldBodyDecodeResult : std::uint8_t {
    Ok = 0,
    Invalid = 1,
    Capacity = 2,
};

bool EncodeCurrentWorldShippingBody(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

bool EncodeCurrentWorldBodyV19(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

// V20 extends the V18 body with signal relations and target configurations.
// It does not enable V19 entry scheduling or change the existing writer table.
bool EncodeCurrentWorldBodyV20(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

bool EncodeCurrentWorldShippingBodyRawAttempt(
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
        std::uint32_t bodyVintage, CurrentWorldDecodeMode mode) noexcept;
    bool Decode(const std::uint8_t* bytes, std::size_t byteCount,
                std::uint32_t bodyVintage,
                CurrentWorldDecodeMode mode) noexcept {
        return DecodeWithResult(bytes, byteCount, bodyVintage, mode) ==
               CurrentWorldBodyDecodeResult::Ok;
    }
    const ContainerWorldInstallViewV1* ProductionView() const noexcept;
    const HistoricalContainerWorldViewV1* ReplayView() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace harken

#endif // HARKEN_CORE_CURRENT_WORLD_CODEC_H
