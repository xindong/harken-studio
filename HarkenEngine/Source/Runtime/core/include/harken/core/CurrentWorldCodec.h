// CurrentWorld body is the .pack playback-program chapter under
// formatVersion 2, not a third product schema. bodyVintage is an
// internal layout id (same role as directory vintage). The current
// write table is kCurrentWorldShippingBodyVintage, also named
// kCanonicalDefinitionPartitionBodyVintage. Shipping pack readers
// (Vn_LoadPack / ParsePackCanonicalImage) and the DirectWorld preview
// envelope decode only that table. Decode of 2-8 remains for
// Tooling/Host trace replay and historical fixtures; it is not a
// shipping program contract. Do not register this integer in schema
// catalog.json. Extra program fields occupy existing slots, or mint a
// new internal layout id that shipping readers refuse; they do not mint
// a public "world format".
#ifndef HARKEN_CORE_CURRENT_WORLD_CODEC_H
#define HARKEN_CORE_CURRENT_WORLD_CODEC_H

#include "harken/core/CurrentWorld.h"
#include "harken/media/CurrentWorldBodyVintage.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace harken {

inline constexpr std::uint32_t kCurrentWorldV3PrefixWireBytes = 200;
inline constexpr std::uint32_t kCurrentWorldV6ActionCapacityWireBytes = 24;
inline constexpr std::uint32_t kCurrentWorldV9CapacityWireBytes = 148;
inline constexpr std::uint32_t kCurrentWorldActionRefWireBytes = 32;
inline constexpr std::uint32_t kCurrentWorldActionTimeWireBytes = 16;
inline constexpr std::uint32_t kCurrentWorldActionHeaderWireBytes = 44;
inline constexpr std::uint32_t kCurrentWorldActionStepWireBytes = 144;

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

// Production writers use this surface so the shipping layout has one
// call boundary even while versioned encoders remain for replay fixtures.
bool EncodeCurrentWorldShippingBody(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

// V5/V6/V7/V8 remain for historical fixtures and old journals. New captures
// and shipping writers emit kCurrentWorldShippingBodyVintage only.
bool EncodeCurrentWorldBodyV6(
    const HistoricalContainerWorldViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

bool EncodeCurrentWorldBodyV7(
    const HistoricalContainerWorldViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

bool EncodeCurrentWorldBodyV8(
    const HistoricalContainerWorldViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

bool EncodeCurrentWorldBodyV9(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

bool EncodeCurrentWorldShippingBodyRawAttempt(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

bool EncodeCurrentWorldBodyV5(
    const HistoricalContainerWorldViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

class CurrentWorldBodyImage {
public:
    CurrentWorldBodyImage() noexcept;
    ~CurrentWorldBodyImage();
    CurrentWorldBodyImage(const CurrentWorldBodyImage&) = delete;
    CurrentWorldBodyImage& operator=(const CurrentWorldBodyImage&) = delete;

    // StrictArtifact accepts kCurrentWorldShippingBodyVintage only.
    // ReplayRawAttempt retains bounded historical decoding for tooling.
    // bodyVintage 2-8 is Tooling/Host trace replay and unit fixtures.
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
