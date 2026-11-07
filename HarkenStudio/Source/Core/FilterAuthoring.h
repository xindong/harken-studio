#pragma once

#include "FilterSemantics.h"
#include "PlayablePropertyAuthoringV1.h"

namespace VNX {
namespace FilterAuthoring {

bool isField(const QString& field);
FilterState effectiveState(const ProjectItem& item, PlatformScope scope = std::nullopt);
std::optional<PlayablePropertyStateV1> readProperty(
    const ProjectItem& item, const QString& field, PlatformScope scope = std::nullopt,
    bool overlayOnly = false);
bool restoreProperty(ProjectItem& item, const QString& field,
                     const PlayablePropertyStateV1& state, PlatformScope scope);
bool setOverride(ProjectItem& item, const QString& field,
                 const QJsonValue& value, PlatformScope scope);
bool clearOverride(ProjectItem& item, const QString& field, PlatformScope scope);

}
}
