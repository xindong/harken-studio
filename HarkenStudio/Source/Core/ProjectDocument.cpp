#include "ProjectDocument.h"

#include "ProjectSchemaGenerated.h"
#include "ItemTypeCatalog.h"
#include "PlayablePropertyAuthoringV1.h"
#include "FieldGroupJson.h"
#include "harken_buildinfo.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <cmath>
#include <initializer_list>
#include <utility>

namespace VNX {

namespace {

bool rejectAuthoringField(QString* error, const QString& path)
{
    if (error) *error = QStringLiteral("Retired field group authoring field: %1").arg(path);
    return false;
}

bool validateDefinitionComponents(const QJsonObject& components, QString* error)
{
    for (const auto& field : harkenplayableproperties::studio::kJsonFieldsV1) {
        if (!FieldGroupForProperty(
                static_cast<harkenplayableproperties::PlayablePropertyIdV1>(field.id)))
            continue;
        const QJsonObject component = components.value(QLatin1String(field.component)).toObject();
        for (const auto* key : {field.storageField, field.jsonKey, field.fieldName})
            if (component.contains(QLatin1String(key)))
                return rejectAuthoringField(error, QStringLiteral("components.%1.%2")
                    .arg(QLatin1String(field.component), QLatin1String(key)));
    }
    const auto rejectFields = [&](const QString& componentName,
                                  std::initializer_list<const char*> fields) {
        const auto component = components.value(componentName).toObject();
        for (const auto* field : fields)
            if (component.contains(QLatin1String(field)))
                return rejectAuthoringField(error, QStringLiteral("components.%1.%2")
                    .arg(componentName, QLatin1String(field)));
        return true;
    };
    return rejectFields(QStringLiteral("mixnode"), {"outputBusId", "outputBusRef"})
        && rejectFields(QStringLiteral("audio"), {"outputBusOverride", "overrideBits"})
        && rejectFields(QStringLiteral("spatial"), {"attenuation", "attenuationGuid"});
}

bool validateOccurrence(const QJsonObject& occurrence, QString* error)
{
    std::string detail;
    if (ValidateOccurrenceFieldGroupExclusionJson(
            QJsonDocument(occurrence).toJson(QJsonDocument::Compact).toStdString(), detail))
        return true;
    if (error) *error = QString::fromStdString(detail);
    return false;
}

bool validateAuthoringNode(const QJsonObject& content, QString* error)
{
    if (content.contains(QStringLiteral("sectionGroups")))
        return rejectAuthoringField(error, QStringLiteral("sectionGroups"));
    const auto type = ItemTypeCatalog::parseTypeName(
        content.value(QStringLiteral("type")).toString(), Qt::CaseSensitive, false);
    const bool definition = type
        && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(*type);
    if (definition) {
        if (!content.value(QStringLiteral("fieldGroups")).isObject()) {
            if (error) *error = QStringLiteral("Definition requires fieldGroups authoring");
            return false;
        }
        DefinitionFieldGroups groups;
        std::string detail;
        if (!ReadFieldGroupsJson(QJsonDocument(content.value(QStringLiteral("fieldGroups"))
                .toObject()).toJson(QJsonDocument::Compact).toStdString(), groups, detail)) {
            if (error) *error = QString::fromStdString(detail);
            return false;
        }
        if (content.contains(QStringLiteral("outputBusOverride")))
            return rejectAuthoringField(error, QStringLiteral("outputBusOverride"));
        QJsonObject properties = content;
        for (const auto* key : {"fieldGroups", "components", "children", "childOccurrences"})
            properties.remove(QLatin1String(key));
        if (!validateOccurrence(properties, error)
            || !validateDefinitionComponents(content.value(QStringLiteral("components")).toObject(), error))
            return false;
    } else if (content.contains(QStringLiteral("fieldGroups"))) {
        if (error) *error = QStringLiteral("This object type cannot author fieldGroups");
        return false;
    }
    const auto validateRows = [&](const QJsonValue& rows, const QString& path) {
        if (rows.isUndefined()) return true;
        if (!rows.isArray()) {
            if (error) *error = path + QStringLiteral(" must be an array");
            return false;
        }
        for (const auto& row : rows.toArray()) {
            if (!row.isObject()) {
                if (error) *error = path + QStringLiteral(" contains a non-object occurrence");
                return false;
            }
            if (!validateOccurrence(row.toObject(), error)) return false;
        }
        return true;
    };
    const auto components = content.value(QStringLiteral("components")).toObject();
    if (content.contains(QStringLiteral("childOccurrences")))
        return rejectAuthoringField(error, QStringLiteral("childOccurrences"));
    if (!validateRows(components.value(QStringLiteral("tracktimeline")).toObject()
            .value(QStringLiteral("clips")), QStringLiteral("tracktimeline.clips")))
        return false;
    for (const auto& child : content.value(QStringLiteral("children")).toArray())
        if (!child.isObject() || !validateAuthoringNode(child.toObject(), error)) return false;
    return true;
}

}

QJsonObject ProjectDocument::createHeader(const QString& type,
                                    const QString& id,
                                    const QString& projectId,
                                    const QJsonObject& originalHeader)
{
    QJsonObject header = originalHeader;
    header.insert(QStringLiteral("type"), type);
    header.insert(QStringLiteral("id"), id);
    header.insert(QStringLiteral("projectId"), projectId);
    header.insert(QStringLiteral("schemaVersion"),
                  ProjectSchema::kProjectSchemaVersion);
    header.insert(QStringLiteral("propertyModel"),
                  QString::fromLatin1(ProjectSchema::kProjectPropertyModel));
    header.insert(QStringLiteral("savedStudioVersion"), QString::fromUtf8(
        Harken::BuildInfo::Version.data(),
        static_cast<int>(Harken::BuildInfo::Version.size())));
    return header;
}

QJsonObject ProjectDocument::create(const QString& type,
                                    const QString& id,
                                    const QString& projectId,
                                    const QJsonObject& content,
                                    const QJsonObject& originalHeader)
{
    if (!validateFieldGroupAuthoring(content)) return {};
    return {
        {QStringLiteral("harkenDocument"),
         createHeader(type, id, projectId, originalHeader)},
        {QStringLiteral("content"), content}
    };
}

bool ProjectDocument::parse(const QJsonObject& document,
                            const QString& expectedType,
                            ProjectDocumentData* data,
                            QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!document.value(QStringLiteral("harkenDocument")).isObject()
        || !document.value(QStringLiteral("content")).isObject()) {
        return fail(QStringLiteral("Project document envelope is missing"));
    }
    if (document.size() != 2)
        return fail(QStringLiteral("Project document has fields outside its envelope"));

    const QJsonObject header =
        document.value(QStringLiteral("harkenDocument")).toObject();
    ProjectDocumentData parsed;
    parsed.type = header.value(QStringLiteral("type")).toString();
    parsed.id = header.value(QStringLiteral("id")).toString();
    parsed.projectId = header.value(QStringLiteral("projectId")).toString();
    const QJsonValue version = header.value(QStringLiteral("schemaVersion"));
    if (parsed.type.isEmpty() || parsed.type != expectedType)
        return fail(QStringLiteral("Project document type does not match its file location"));
    if (QUuid::fromString(parsed.id).isNull())
        return fail(QStringLiteral("Project document identity is invalid"));
    if (QUuid::fromString(parsed.projectId).isNull())
        return fail(QStringLiteral("Project identity is invalid"));
    if (!version.isDouble() || !std::isfinite(version.toDouble())
        || std::floor(version.toDouble()) != version.toDouble()
        || version.toInt() <= 0) {
        return fail(QStringLiteral("Project Schema version is invalid"));
    }
    parsed.schemaVersion = version.toInt();
    if (parsed.schemaVersion == ProjectSchema::kProjectSchemaVersion
        && header.value(QStringLiteral("propertyModel"))
            != QJsonValue(QString::fromLatin1(ProjectSchema::kProjectPropertyModel)))
        return fail(QStringLiteral("Unsupported project property model; recreate this project in Studio"));
    parsed.savedStudioVersion =
        header.value(QStringLiteral("savedStudioVersion")).toString();
    if (parsed.schemaVersion >= ProjectSchema::kProjectSchemaVersions[0]
        && parsed.savedStudioVersion.isEmpty()) {
        return fail(QStringLiteral("Project document header is malformed"));
    }
    parsed.header = header;
    parsed.content = document.value(QStringLiteral("content")).toObject();
    if (parsed.schemaVersion == ProjectSchema::kProjectSchemaVersion
        && !validateFieldGroupAuthoring(parsed.content, error)) return false;
    if (data) *data = std::move(parsed);
    return true;
}

bool ProjectDocument::validateFieldGroupAuthoring(const QJsonObject& content,
                                                   QString* error)
{
    return validateAuthoringNode(content, error);
}

}
