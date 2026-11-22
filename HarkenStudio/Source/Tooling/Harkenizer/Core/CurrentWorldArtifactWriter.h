#pragma once

#include "PlatformContext.h"

#include <QString>

#include <cstdint>
#include <unordered_map>

namespace harken::studio::build {
struct ContainerWorldInstallViewV1;
}

namespace harken::studio {

struct ProjectData;

bool WriteCurrentWorldArtifactV1(
    const QString& targetPath,
    const harken::studio::build::ContainerWorldInstallViewV1& world,
    QString* error = nullptr);
bool WriteCurrentWorldArtifactV1(
    ProjectData& project,
    PlatformScope scope,
    const QString& targetPath,
    QString* error = nullptr);
bool WriteCurrentWorldArtifactV1(
    ProjectData& project,
    PlatformScope scope,
    const QString& targetPath,
    const std::unordered_map<harken::studio::types::ObjectId, std::uint64_t>& soundRevisions,
    std::uint64_t* semanticFingerprint,
    QString* error = nullptr);

}
