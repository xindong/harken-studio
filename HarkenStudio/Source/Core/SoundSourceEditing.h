#pragma once

#include "AssetPipeline/AssetTypes.h"
#include "ProjectTypes.h"

namespace VNX::SoundSourceEditing {

QString previewLanguage(const ProjectItem& item, const QString& contextLanguage);
QString exportLanguage(const ProjectItem& item, const QString& variantLanguage);

bool syncActiveProjection(ProjectItem& item, ProjectPlatformId platform, const QString& language);
const SoundSourceDef* activeSource(const ProjectItem& item, ProjectPlatformId platform, const QString& language);
uint32_t activeSourceId(const ProjectItem& item, ProjectPlatformId platform, const QString& language);
QString activeRelativePath(const ProjectItem& item, ProjectPlatformId platform, const QString& language);
std::optional<double> activeTrimmedDuration(const ProjectItem& item, ProjectPlatformId platform, const QString& language);

}
