#pragma once

#include "Core/ProjectSchemaGenerated.h"
#include "ProjectPlatformTestUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace HarkenTest {

inline const QString kProjectId =
    QStringLiteral("11111111-2222-3333-4444-555555555555");

inline QJsonObject projectDocument(const QString& type,
                                   const QString& id,
                                   const QString& projectId,
                                   const QJsonObject& content,
                                   int schemaVersion =
                                       VNX::ProjectSchema::kProjectSchemaVersion)
{
    QJsonObject header{
        {QStringLiteral("type"), type},
        {QStringLiteral("id"), id},
        {QStringLiteral("projectId"), projectId},
        {QStringLiteral("schemaVersion"), schemaVersion},
    };
    QJsonObject authoredContent = content;
    if (schemaVersion >= VNX::ProjectSchema::kProjectSchemaVersion) {
        header.insert(QStringLiteral("propertyModel"),
                      QLatin1String(VNX::ProjectSchema::kProjectPropertyModel));
        const QString itemType = content.value(QStringLiteral("type")).toString();
        const bool hasFieldGroups = itemType == QStringLiteral("Sound")
            || itemType == QStringLiteral("Container") || itemType == QStringLiteral("Layer")
            || itemType == QStringLiteral("Track") || itemType == QStringLiteral("Segment");
        if (hasFieldGroups && !authoredContent.contains(QStringLiteral("fieldGroups"))) {
            const QJsonObject inherit{{QStringLiteral("inherit"), true}};
            authoredContent.insert(QStringLiteral("fieldGroups"), QJsonObject{
                {QStringLiteral("shared"), QJsonObject{
                    {QStringLiteral("mainOutput"), inherit},
                    {QStringLiteral("send"), inherit},
                    {QStringLiteral("spatialRating"), inherit},
                }},
            });
        }
        if ((hasFieldGroups || itemType == QStringLiteral("Bus"))
            && !authoredContent.contains(QStringLiteral("playableProperties"))) {
            authoredContent.insert(QStringLiteral("playableProperties"), QJsonObject{
                {QStringLiteral("presence"), QStringLiteral("0000000000000000")},
            });
        }
    }
    if (schemaVersion >= 2
        && (schemaVersion < VNX::ProjectSchema::kProjectSchemaVersion
            || type == QStringLiteral("project.root")))
        header.insert(QStringLiteral("savedStudioVersion"), QStringLiteral("1.0.0"));
    if (schemaVersion >= VNX::ProjectSchema::kProjectSchemaVersion
        && type == QStringLiteral("project.root"))
        header.insert(QStringLiteral("packSchemaVersion"), HARKEN_PACK_SCHEMA_VERSION);
    return {
        {QStringLiteral("harkenDocument"), header},
        {QStringLiteral("content"), authoredContent},
    };
}

inline QJsonObject projectRoot(const QJsonObject& content,
                               int schemaVersion =
                                   VNX::ProjectSchema::kProjectSchemaVersion,
                               const QString& projectId = kProjectId)
{
    QJsonObject configured = content;
    if (schemaVersion == VNX::ProjectSchema::kProjectSchemaVersion
        && content.value(QStringLiteral("metadata")).isObject()) {
        auto metadata = content.value(QStringLiteral("metadata")).toObject();
        if (!metadata.contains(QStringLiteral("platforms"))
            && !metadata.contains(QStringLiteral("targetPlatforms"))) {
            const auto project = projectWithPlatforms();
            metadata.insert(QStringLiteral("platforms"),
                            VNX::ProjectPlatformSemantics::platformsToJson(project.platforms));
            configured.insert(QStringLiteral("metadata"), metadata);
        }
    }
    return projectDocument(QStringLiteral("project.root"), projectId, projectId,
                           configured, schemaVersion);
}

inline QJsonObject projectContent(const QJsonObject& document)
{
    return document.value(QStringLiteral("content")).toObject();
}

inline QJsonObject projectHeader(const QJsonObject& document)
{
    return document.value(QStringLiteral("harkenDocument")).toObject();
}

inline bool writeRootBusForProject(const QString& projectPath, int schemaVersion)
{
    QFile projectFile(projectPath);
    if (!projectFile.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument projectDocumentJson =
        QJsonDocument::fromJson(projectFile.readAll());
    projectFile.close();
    if (!projectDocumentJson.isObject())
        return false;

    const QString projectId = projectHeader(projectDocumentJson.object())
                                  .value(QStringLiteral("projectId"))
                                  .toString();
    if (projectId.isEmpty())
        return false;

    const QString rootBusId =
        QStringLiteral("99999999-9999-4999-8999-999999999999");
    const QJsonObject bus = projectDocument(
        QStringLiteral("project.bus"), rootBusId, projectId,
        QJsonObject{
            {QStringLiteral("type"), QStringLiteral("Bus")},
            {QStringLiteral("guid"), rootBusId},
            {QStringLiteral("name"), QStringLiteral("Master Audio Bus")},
            {QStringLiteral("parentRef"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("root")},
            }},
            {QStringLiteral("componentData"), QJsonObject{}},
        },
        schemaVersion);
    QDir projectDirectory(QFileInfo(projectPath).absolutePath());
    if (!projectDirectory.mkpath(QStringLiteral("Busses")))
        return false;
    QFile busFile(projectDirectory.filePath(QStringLiteral("Busses/Root.vp")));
    if (!busFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray payload = QJsonDocument(bus).toJson(QJsonDocument::Indented);
    return busFile.write(payload) == payload.size();
}

inline bool writeRootBusForProject(const QString& projectPath)
{
    return writeRootBusForProject(projectPath,
                                  VNX::ProjectSchema::kProjectSchemaVersion);
}

}
