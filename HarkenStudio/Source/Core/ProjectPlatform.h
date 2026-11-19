#pragma once

#include "AssetPipeline/AssetTypes.h"

#include <QUuid>
#include <QVector>
#include <QMap>

#include <optional>

namespace VNX {

enum class BasePlatform { Windows, macOS, Linux, Android, iOS };

using ProjectPlatformId = QUuid;
using PlatformScope = std::optional<ProjectPlatformId>;

struct ProjectPlatform
{
    ProjectPlatformId id;
    QString name;
    BasePlatform basePlatform = BasePlatform::Windows;

    QJsonObject toJson() const;
};

struct ProjectData;
struct ProjectItem;

namespace ProjectPlatformSemantics {

QString basePlatformName(BasePlatform platform);
std::optional<BasePlatform> parseBasePlatform(const QString& name);
std::optional<ProjectPlatformId> parseId(const QString& text);
QString idString(const ProjectPlatformId& id);
QStringList ids(const QVector<ProjectPlatform>& platforms);
const ProjectPlatform* find(const QVector<ProjectPlatform>& platforms,
                            const ProjectPlatformId& id);
Platform mediaPlatform(BasePlatform platform);
QVector<ProjectPlatform> defaultPlatforms();
bool validateName(const QString& name, QString* error = nullptr);
bool validatePlatforms(const QVector<ProjectPlatform>& platforms,
                       QString* error = nullptr);
bool validateIds(const QStringList& ids, const QVector<ProjectPlatform>& platforms,
                 QString* error = nullptr);
bool platformsFromJson(const QJsonValue& value, QVector<ProjectPlatform>* platforms,
                       QString* error = nullptr);
QJsonArray platformsToJson(const QVector<ProjectPlatform>& platforms);
QVector<ProjectItem*> referenceOwners(const ProjectData& project);
bool validateProject(const ProjectData& project, QString* error = nullptr);
QStringList validateDocuments(const QJsonObject& rootContent,
                              const QMap<QString, QJsonObject>& documents);

}
}
