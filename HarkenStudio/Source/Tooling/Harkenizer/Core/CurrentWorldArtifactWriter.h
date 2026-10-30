#pragma once

#include "PlatformContext.h"

#include <QString>

#include <cstdint>
#include <unordered_map>

namespace harkenstudiobuild {
struct ContainerWorldInstallViewV1;
}

namespace VNX {

struct ProjectData;

bool WriteCurrentWorldArtifactV1(
    const QString& targetPath,
    const harkenstudiobuild::ContainerWorldInstallViewV1& world,
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
    const std::unordered_map<harkenstudio::ObjectId, std::uint64_t>& soundRevisions,
    std::uint64_t* semanticFingerprint,
    QString* error = nullptr);

}
