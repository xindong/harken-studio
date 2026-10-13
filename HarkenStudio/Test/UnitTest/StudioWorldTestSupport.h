#pragma once

#include "Engine.h"
#include "harken_studio_build/WorldArtifact.h"
#include "harken_studio_build/WorldCodec.h"
#include "harken/core/CurrentWorldCodec.h"

namespace harkentest {

inline bool DecodeStudioWorld(
    const harkenstudiobuild::ContainerWorldInstallViewV1& world,
    harken::CurrentWorldBodyImage& image) {
    std::vector<std::uint8_t> bytes;
    return harkenstudiobuild::EncodeCurrentWorldShippingBody(world, bytes)
        && image.Decode(bytes.data(), bytes.size(),
            harkenstudiobuild::kCurrentWorldShippingBodyVintage,
            harken::CurrentWorldDecodeMode::StrictArtifact);
}

inline harken::WorldInstallStatus ValidateCompilerWorld(
    const harkenstudiobuild::ContainerWorldInstallViewV1& world) {
    harken::CurrentWorldBodyImage image;
    if (!DecodeStudioWorld(world, image)) return harken::WorldInstallStatus::InvalidWorld;
    return harken::ValidateCurrentWorldV1(*image.ProductionView());
}

inline harken::WorldInstallStatus ValidateCompilerWorld(
    const harken::ContainerWorldInstallViewV1& world) {
    return harken::ValidateCurrentWorldV1(world);
}

inline harken::WorldInstallResult InstallCompilerWorld(
    harken::Engine& engine, const harkenstudiobuild::ContainerWorldInstallViewV1& world) {
    harken::CurrentWorldBodyImage image;
    if (!DecodeStudioWorld(world, image)) {
        harken::WorldInstallResult result;
        result.status = harken::WorldInstallStatus::InvalidWorld;
        return result;
    }
    return harken::InstallContainerWorldV1(engine, *image.ProductionView());
}

}
