#pragma once

#include "Components/VConversion.h"
#include "FieldGroupEditing.h"

class QObject;

namespace VNX {

class ProjectManager;

namespace ConversionEditing {

bool setPlatforms(ProjectManager& manager, const QUuid& conversion,
    const std::vector<ConversionPlatformConfig>& replacement,
    const std::optional<std::vector<ConversionPlatformConfig>>& expectedBefore = {},
    const QObject* originator = nullptr, QString* error = nullptr);

bool setObject(ProjectManager& manager, const QUuid& object, bool inherit,
    const std::optional<QUuid>& settings = {}, const QObject* originator = nullptr,
    QString* error = nullptr);

bool setObjects(ProjectManager& manager, const QVector<QUuid>& objects,
    std::optional<bool> inherit, const std::optional<QUuid>& settings = {},
    const QVector<FieldGroupDefinitionSnapshot>& expectedBefore = {},
    const QObject* originator = nullptr, QString* error = nullptr);

}
}
