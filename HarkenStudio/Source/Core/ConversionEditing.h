#pragma once

#include "Components/VConversion.h"
#include "FieldGroupEditing.h"
#include "ProjectPlatform.h"

class QObject;

namespace harken::studio {

class ProjectManager;

namespace ConversionEditing {

bool setConfig(ProjectManager& manager, const QUuid& conversion,
    const ConversionConfig& replacement,
    const std::optional<ConversionConfig>& expectedBefore = {},
    const QObject* originator = nullptr, QString* error = nullptr);

bool setObject(ProjectManager& manager, const QUuid& object, bool inherit,
    const std::optional<QUuid>& settings = {}, const PlatformScope& platform = {},
    const QObject* originator = nullptr, QString* error = nullptr);

bool setObjects(ProjectManager& manager, const QVector<QUuid>& objects,
    std::optional<bool> inherit, const std::optional<QUuid>& settings = {},
    const PlatformScope& platform = {},
    const QVector<FieldGroupDefinitionSnapshot>& expectedBefore = {},
    const QObject* originator = nullptr, QString* error = nullptr);

}
}
