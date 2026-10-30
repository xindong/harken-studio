#include "ThemePackageParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <type_traits>
#include <utility>

namespace VNX {

namespace {

template<typename T>
using ParseResult = ThemePackageParseResult<T>;

ThemePackageDiagnostic diagnostic(ThemePackageErrorCode code,
                                  const QString& file,
                                  const QString& path,
                                  const QString& message)
{
    return ThemePackageDiagnostic{code, file, path, message};
}

template<typename T>
ParseResult<T> failed(ThemePackageErrorCode code,
                      const QString& file,
                      const QString& path,
                      const QString& message)
{
    ParseResult<T> result;
    result.diagnostics.append(diagnostic(code, file, path, message));
    return result;
}

template<typename T>
std::optional<QJsonObject> parseRootObject(const QByteArray& json,
                                           const QString& file,
                                           ParseResult<T>& result)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError)
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::InvalidJson,
            file,
            QStringLiteral("$"),
            error.errorString()));
        return std::nullopt;
    }
    if (!document.isObject())
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::RootTypeMismatch,
            file,
            QStringLiteral("$"),
            QStringLiteral("Expected a JSON object.")));
        return std::nullopt;
    }
    return document.object();
}

bool rejectUnknownFields(const QJsonObject& object,
                         const QSet<QString>& allowed,
                         const QString& file,
                         const QString& path,
                         QList<ThemePackageDiagnostic>& diagnostics)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
    {
        if (!allowed.contains(it.key()))
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::UnknownField,
                file,
                path + QLatin1Char('.') + it.key(),
                QStringLiteral("Unknown field.")));
            return false;
        }
    }
    return true;
}

bool readRequiredString(const QJsonObject& object,
                        const QString& key,
                        const QString& file,
                        const QString& path,
                        QString& output,
                        QList<ThemePackageDiagnostic>& diagnostics)
{
    const QString fieldPath = path + QLatin1Char('.') + key;
    if (!object.contains(key))
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::MissingField,
            file,
            fieldPath,
            QStringLiteral("Required field is missing.")));
        return false;
    }
    const QJsonValue value = object.value(key);
    if (!value.isString())
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::FieldTypeMismatch,
            file,
            fieldPath,
            QStringLiteral("Expected a string.")));
        return false;
    }
    output = value.toString();
    if (output.isEmpty())
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::InvalidValue,
            file,
            fieldPath,
            QStringLiteral("String must not be empty.")));
        return false;
    }
    return true;
}

bool readOptionalString(const QJsonObject& object,
                        const QString& key,
                        const QString& file,
                        const QString& path,
                        std::optional<QString>& output,
                        QList<ThemePackageDiagnostic>& diagnostics)
{
    if (!object.contains(key))
        return true;
    QString value;
    if (!readRequiredString(object, key, file, path, value, diagnostics))
        return false;
    output = value;
    return true;
}

bool readRequiredObject(const QJsonObject& object,
                        const QString& key,
                        const QString& file,
                        const QString& path,
                        QJsonObject& output,
                        QList<ThemePackageDiagnostic>& diagnostics)
{
    const QString fieldPath = path + QLatin1Char('.') + key;
    if (!object.contains(key))
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::MissingField,
            file,
            fieldPath,
            QStringLiteral("Required field is missing.")));
        return false;
    }
    const QJsonValue value = object.value(key);
    if (!value.isObject())
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::FieldTypeMismatch,
            file,
            fieldPath,
            QStringLiteral("Expected an object.")));
        return false;
    }
    output = value.toObject();
    return true;
}

bool readStringList(const QJsonObject& object,
                    const QString& key,
                    const QString& file,
                    const QString& path,
                    QStringList& output,
                    QList<ThemePackageDiagnostic>& diagnostics)
{
    if (!object.contains(key))
        return true;
    const QString fieldPath = path + QLatin1Char('.') + key;
    const QJsonValue value = object.value(key);
    if (!value.isArray())
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::FieldTypeMismatch,
            file,
            fieldPath,
            QStringLiteral("Expected an array.")));
        return false;
    }
    const QJsonArray values = value.toArray();
    for (qsizetype index = 0; index < values.size(); ++index)
    {
        const QJsonValue entry = values.at(index);
        const QString entryPath = fieldPath + QLatin1Char('[')
            + QString::number(index) + QLatin1Char(']');
        if (!entry.isString())
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::FieldTypeMismatch,
                file,
                entryPath,
                QStringLiteral("Expected a string.")));
            return false;
        }
        const QString text = entry.toString();
        if (text.isEmpty())
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::InvalidValue,
                file,
                entryPath,
                QStringLiteral("String must not be empty.")));
            return false;
        }
        output.append(text);
    }
    return true;
}

template<typename T>
bool readSemanticValues(const QJsonObject& root,
                        const QString& file,
                        const QString& section,
                        QMap<QString, T>& output,
                        QList<ThemePackageDiagnostic>& diagnostics)
{
    if (!root.contains(section))
        return true;
    const QString sectionPath = QStringLiteral("$.") + section;
    const QJsonValue sectionValue = root.value(section);
    if (!sectionValue.isObject())
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::FieldTypeMismatch,
            file,
            sectionPath,
            QStringLiteral("Expected an object.")));
        return false;
    }
    const QJsonObject values = sectionValue.toObject();
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        const QString valuePath = sectionPath + QLatin1Char('.') + it.key();
        if (it.key().isEmpty())
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::InvalidValue,
                file,
                sectionPath,
                QStringLiteral("Semantic keys must not be empty.")));
            return false;
        }
        if constexpr (std::is_same_v<T, QString>)
        {
            if (!it.value().isString())
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::FieldTypeMismatch,
                    file,
                    valuePath,
                    QStringLiteral("Expected a string.")));
                return false;
            }
            const QString value = it.value().toString();
            if (value.isEmpty())
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::InvalidValue,
                    file,
                    valuePath,
                    QStringLiteral("String must not be empty.")));
                return false;
            }
            output.insert(it.key(), value);
        }
        else
        {
            if (!it.value().isDouble() || !std::isfinite(it.value().toDouble()))
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::FieldTypeMismatch,
                    file,
                    valuePath,
                    QStringLiteral("Expected a finite number.")));
                return false;
            }
            output.insert(it.key(), it.value().toDouble());
        }
    }
    return true;
}

template<typename T>
ParseResult<T> parseSemanticDocument(const QByteArray& json,
                                     const QString& file)
{
    ParseResult<T> result;
    const std::optional<QJsonObject> root = parseRootObject(json, file, result);
    if (!root)
        return result;
    if (!rejectUnknownFields(*root,
                             {QStringLiteral("colors"), QStringLiteral("metrics")},
                             file,
                             QStringLiteral("$"),
                             result.diagnostics))
        return result;
    T dto;
    if (!readSemanticValues(*root, file, QStringLiteral("colors"),
                            dto.colors, result.diagnostics)
        || !readSemanticValues(*root, file, QStringLiteral("metrics"),
                               dto.metrics, result.diagnostics))
        return result;
    if (!root->contains(QStringLiteral("colors"))
        && !root->contains(QStringLiteral("metrics")))
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::MissingField,
            file,
            QStringLiteral("$"),
            QStringLiteral("At least one of colors or metrics is required.")));
        return result;
    }
    result.value = std::move(dto);
    return result;
}

bool readFontRoles(const QJsonObject& root,
                   const QString& file,
                   QMap<QString, ThemeTokenDto::FontRole>& output,
                   QList<ThemePackageDiagnostic>& diagnostics)
{
    if (!root.contains(QStringLiteral("fonts")))
        return true;
    const QJsonValue fontsValue = root.value(QStringLiteral("fonts"));
    if (!fontsValue.isObject())
    {
        diagnostics.append(diagnostic(
            ThemePackageErrorCode::FieldTypeMismatch, file,
            QStringLiteral("$.fonts"), QStringLiteral("Expected an object.")));
        return false;
    }
    const QJsonObject fonts = fontsValue.toObject();
    for (auto it = fonts.constBegin(); it != fonts.constEnd(); ++it)
    {
        const QString rolePath = QStringLiteral("$.fonts.") + it.key();
        if (it.key().isEmpty() || !it.value().isObject())
        {
            diagnostics.append(diagnostic(
                it.key().isEmpty() ? ThemePackageErrorCode::InvalidValue
                                   : ThemePackageErrorCode::FieldTypeMismatch,
                file, it.key().isEmpty() ? QStringLiteral("$.fonts") : rolePath,
                it.key().isEmpty() ? QStringLiteral("Font roles must not be empty.")
                                   : QStringLiteral("Expected an object.")));
            return false;
        }
        const QJsonObject roleObject = it.value().toObject();
        if (!rejectUnknownFields(
                roleObject,
                {QStringLiteral("family"), QStringLiteral("pointSize"),
                 QStringLiteral("weight")},
                file, rolePath, diagnostics))
            return false;
        if (roleObject.isEmpty())
        {
            diagnostics.append(diagnostic(
                ThemePackageErrorCode::MissingField, file, rolePath,
                QStringLiteral("At least one font role field is required.")));
            return false;
        }
        ThemeTokenDto::FontRole role;
        if (roleObject.contains(QStringLiteral("family")))
        {
            QString family;
            if (!readRequiredString(roleObject, QStringLiteral("family"), file,
                                    rolePath, family, diagnostics))
                return false;
            role.family = std::move(family);
        }
        const QStringList integerFields{
            QStringLiteral("pointSize"), QStringLiteral("weight")};
        std::optional<int>* destinations[]{&role.pointSize, &role.weight};
        for (qsizetype index = 0; index < integerFields.size(); ++index)
        {
            const QString& name = integerFields.at(index);
            if (!roleObject.contains(name))
                continue;
            const QJsonValue value = roleObject.value(name);
            if (!value.isDouble()
                || std::floor(value.toDouble()) != value.toDouble())
            {
                diagnostics.append(diagnostic(
                    ThemePackageErrorCode::FieldTypeMismatch, file,
                    rolePath + QLatin1Char('.') + name,
                    QStringLiteral("Expected an integer.")));
                return false;
            }
            *destinations[index] = value.toInt();
        }
        output.insert(it.key(), std::move(role));
    }
    return true;
}

}

QString themePackageErrorCodeName(ThemePackageErrorCode code)
{
    switch (code)
    {
        case ThemePackageErrorCode::InvalidJson:
            return QStringLiteral("theme.json.invalid");
        case ThemePackageErrorCode::DuplicateJsonKey:
            return QStringLiteral("theme.json.duplicate_key");
        case ThemePackageErrorCode::RootTypeMismatch:
            return QStringLiteral("theme.schema.root_type");
        case ThemePackageErrorCode::MissingField:
            return QStringLiteral("theme.schema.missing_field");
        case ThemePackageErrorCode::FieldTypeMismatch:
            return QStringLiteral("theme.schema.field_type");
        case ThemePackageErrorCode::UnsupportedSchemaVersion:
            return QStringLiteral("theme.schema.unsupported_version");
        case ThemePackageErrorCode::UnknownField:
            return QStringLiteral("theme.schema.unknown_field");
        case ThemePackageErrorCode::InvalidValue:
            return QStringLiteral("theme.schema.invalid_value");
        case ThemePackageErrorCode::FileNotFound:
            return QStringLiteral("theme.file.not_found");
        case ThemePackageErrorCode::PathEscape:
            return QStringLiteral("theme.file.path_escape");
        case ThemePackageErrorCode::ResourceLimit:
            return QStringLiteral("theme.resource.limit");
        case ThemePackageErrorCode::UnsupportedCompatibility:
            return QStringLiteral("theme.compatibility.unsupported");
        case ThemePackageErrorCode::DuplicateThemeId:
            return QStringLiteral("theme.inheritance.duplicate_id");
        case ThemePackageErrorCode::MissingBaseTheme:
            return QStringLiteral("theme.inheritance.missing_base");
        case ThemePackageErrorCode::InheritanceCycle:
            return QStringLiteral("theme.inheritance.cycle");
        case ThemePackageErrorCode::InheritanceDepthExceeded:
            return QStringLiteral("theme.inheritance.depth");
        case ThemePackageErrorCode::UnknownSemanticKey:
            return QStringLiteral("theme.semantic.unknown_key");
        case ThemePackageErrorCode::InvalidColor:
            return QStringLiteral("theme.semantic.invalid_color");
        case ThemePackageErrorCode::MetricOutOfRange:
            return QStringLiteral("theme.semantic.metric_range");
        case ThemePackageErrorCode::RuntimeThreadViolation:
            return QStringLiteral("theme.runtime.thread_violation");
        case ThemePackageErrorCode::PersistenceUnavailable:
            return QStringLiteral("theme.runtime.persistence_unavailable");
        case ThemePackageErrorCode::AssetNotFound:
            return QStringLiteral("theme.asset.not_found");
        case ThemePackageErrorCode::AssetTypeMismatch:
            return QStringLiteral("theme.asset.type_mismatch");
        case ThemePackageErrorCode::AssetDecodeFailed:
            return QStringLiteral("theme.asset.decode_failed");
        case ThemePackageErrorCode::AssetDimensionsExceeded:
            return QStringLiteral("theme.asset.dimensions_exceeded");
        case ThemePackageErrorCode::AssetThreadViolation:
            return QStringLiteral("theme.asset.thread_violation");
        case ThemePackageErrorCode::UnknownRecipeHost:
            return QStringLiteral("theme.recipe.unknown_host");
        case ThemePackageErrorCode::UnknownRecipeSlot:
            return QStringLiteral("theme.recipe.unknown_slot");
        case ThemePackageErrorCode::DuplicateRecipe:
            return QStringLiteral("theme.recipe.duplicate");
        case ThemePackageErrorCode::DuplicateRecipeSlot:
            return QStringLiteral("theme.recipe.duplicate_slot");
        case ThemePackageErrorCode::RecipeSizeConflict:
            return QStringLiteral("theme.recipe.size_conflict");
        case ThemePackageErrorCode::UnsupportedRecipeVariant:
            return QStringLiteral("theme.recipe.unsupported_variant");
        case ThemePackageErrorCode::UnsupportedRecipePresenter:
            return QStringLiteral("theme.recipe.unsupported_presenter");
    }
    return QStringLiteral("theme.schema.invalid_value");
}

ThemePackageParseResult<ThemeManifestDto> parseThemeManifest(
    const QByteArray& json,
    const QString& file)
{
    ParseResult<ThemeManifestDto> result;
    const std::optional<QJsonObject> root = parseRootObject(json, file, result);
    if (!root)
        return result;
    ThemeManifestDto dto;
    if (!root->contains(QStringLiteral("schemaVersion")))
        return failed<ThemeManifestDto>(
            ThemePackageErrorCode::MissingField, file,
            QStringLiteral("$.schemaVersion"),
            QStringLiteral("Required field is missing."));
    const QJsonValue schemaVersion = root->value(QStringLiteral("schemaVersion"));
    if (!schemaVersion.isDouble()
        || std::floor(schemaVersion.toDouble()) != schemaVersion.toDouble())
        return failed<ThemeManifestDto>(
            ThemePackageErrorCode::FieldTypeMismatch, file,
            QStringLiteral("$.schemaVersion"),
            QStringLiteral("Expected an integer."));
    dto.schemaVersion = schemaVersion.toInt();
    if (dto.schemaVersion < ThemePackageContract::minimumSchemaVersion
        || dto.schemaVersion > ThemePackageContract::maximumSchemaVersion)
        return failed<ThemeManifestDto>(
            ThemePackageErrorCode::UnsupportedSchemaVersion, file,
            QStringLiteral("$.schemaVersion"),
            QStringLiteral("Unsupported theme package schema version."));

    const QSet<QString> rootFields{
        QStringLiteral("schemaVersion"),
        QStringLiteral("id"),
        QStringLiteral("displayName"),
        QStringLiteral("version"),
        QStringLiteral("baseThemeId"),
        QStringLiteral("compatibility"),
        QStringLiteral("entries"),
        QStringLiteral("previewAsset")};
    if (!rejectUnknownFields(*root, rootFields, file, QStringLiteral("$"),
                             result.diagnostics))
        return result;

    if (!readRequiredString(*root, QStringLiteral("id"), file,
                            QStringLiteral("$"), dto.id, result.diagnostics)
        || !readRequiredString(*root, QStringLiteral("displayName"), file,
                               QStringLiteral("$"), dto.displayName,
                               result.diagnostics)
        || !readRequiredString(*root, QStringLiteral("version"), file,
                               QStringLiteral("$"), dto.version,
                               result.diagnostics)
        || !readOptionalString(*root, QStringLiteral("baseThemeId"), file,
                               QStringLiteral("$"), dto.baseThemeId,
                               result.diagnostics)
        || !readOptionalString(*root, QStringLiteral("previewAsset"), file,
                               QStringLiteral("$"), dto.previewAssetId,
                               result.diagnostics))
        return result;

    QJsonObject compatibility;
    if (!readRequiredObject(*root, QStringLiteral("compatibility"), file,
                            QStringLiteral("$"), compatibility,
                            result.diagnostics)
        || !rejectUnknownFields(
            compatibility,
            {QStringLiteral("harkenVersion"), QStringLiteral("capabilities")},
            file, QStringLiteral("$.compatibility"), result.diagnostics)
        || !readOptionalString(
            compatibility, QStringLiteral("harkenVersion"), file,
            QStringLiteral("$.compatibility"),
            dto.compatibility.harkenVersionRange, result.diagnostics)
        || !readStringList(
            compatibility, QStringLiteral("capabilities"), file,
            QStringLiteral("$.compatibility"), dto.compatibility.capabilities,
            result.diagnostics))
        return result;
    if (!dto.compatibility.harkenVersionRange
        && dto.compatibility.capabilities.isEmpty())
        return failed<ThemeManifestDto>(
            ThemePackageErrorCode::MissingField, file,
            QStringLiteral("$.compatibility"),
            QStringLiteral("A Harken version range or capability is required."));

    QJsonObject entries;
    if (!readRequiredObject(*root, QStringLiteral("entries"), file,
                            QStringLiteral("$"), entries, result.diagnostics)
        || !rejectUnknownFields(
            entries,
            {QStringLiteral("tokens"), QStringLiteral("visuals"),
             QStringLiteral("assetMap"), QStringLiteral("presenters"),
             QStringLiteral("recipes"), QStringLiteral("colors")},
            file, QStringLiteral("$.entries"), result.diagnostics)
        || !readRequiredString(
            entries, QStringLiteral("tokens"), file,
            QStringLiteral("$.entries"), dto.entries.tokens,
            result.diagnostics)
        || !readRequiredString(
            entries, QStringLiteral("assetMap"), file,
            QStringLiteral("$.entries"), dto.entries.assetMap,
            result.diagnostics)
        || !readOptionalString(
            entries, QStringLiteral("presenters"), file,
            QStringLiteral("$.entries"), dto.entries.presenters,
            result.diagnostics)
        || !readOptionalString(
            entries, QStringLiteral("recipes"), file,
            QStringLiteral("$.entries"), dto.entries.recipes,
            result.diagnostics)
        || !readOptionalString(
            entries, QStringLiteral("colors"), file,
            QStringLiteral("$.entries"), dto.entries.colors,
            result.diagnostics))
        return result;

    QJsonObject visuals;
    if (!readRequiredObject(entries, QStringLiteral("visuals"), file,
                            QStringLiteral("$.entries"), visuals,
                            result.diagnostics)
        || !rejectUnknownFields(
            visuals,
            {QStringLiteral("curve"), QStringLiteral("meter"),
             QStringLiteral("waveform"), QStringLiteral("nodeGraph"),
             QStringLiteral("blueprint"), QStringLiteral("timeline"),
             QStringLiteral("music"), QStringLiteral("log"),
             QStringLiteral("monitor"),
             QStringLiteral("ai"), QStringLiteral("scene3d"),
             QStringLiteral("spatial"), QStringLiteral("feedback")},
            file, QStringLiteral("$.entries.visuals"), result.diagnostics)
        || !readRequiredString(
            visuals, QStringLiteral("curve"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.curve,
            result.diagnostics)
        || !readRequiredString(
            visuals, QStringLiteral("meter"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.meter,
            result.diagnostics)
        || !readRequiredString(
            visuals, QStringLiteral("waveform"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.waveform,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("nodeGraph"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.nodeGraph,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("blueprint"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.blueprint,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("timeline"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.timeline,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("music"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.music,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("log"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.log,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("monitor"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.monitor,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("ai"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.ai,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("scene3d"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.scene3d,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("spatial"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.spatial,
            result.diagnostics)
        || !readOptionalString(
            visuals, QStringLiteral("feedback"), file,
            QStringLiteral("$.entries.visuals"), dto.entries.visuals.feedback,
            result.diagnostics))
        return result;

    result.value = std::move(dto);
    return result;
}

ThemePackageParseResult<ThemeColorDefinitionDto> parseThemeColorDefinition(
    const QByteArray& json, const QString& file)
{
    ParseResult<ThemeColorDefinitionDto> result;
    const auto root = parseRootObject(json, file, result);
    if (!root)
        return result;
    ThemeColorDefinitionDto dto;
    if (!rejectUnknownFields(*root,
            {QStringLiteral("roles"), QStringLiteral("palettes"),
             QStringLiteral("defaultPalette")}, file, QStringLiteral("$"),
            result.diagnostics)
        || !readRequiredString(*root, QStringLiteral("defaultPalette"), file,
                               QStringLiteral("$"), dto.defaultPalette,
                               result.diagnostics))
        return result;
    const auto fail = [&](ThemePackageErrorCode code, const QString& path,
                          const QString& message) {
        result.diagnostics.append(diagnostic(code, file, path, message));
        return result;
    };
    if (!root->value(QStringLiteral("roles")).isArray()
        || !root->value(QStringLiteral("palettes")).isArray())
        return fail(ThemePackageErrorCode::FieldTypeMismatch, QStringLiteral("$"),
                    QStringLiteral("Roles and palettes must be arrays."));

    QSet<QString> roleKeys;
    QSet<QString> targets;
    const QJsonArray roles = root->value(QStringLiteral("roles")).toArray();
    for (qsizetype index = 0; index < roles.size(); ++index)
    {
        const QString path = QStringLiteral("$.roles[%1]").arg(index);
        if (!roles.at(index).isObject())
            return fail(ThemePackageErrorCode::FieldTypeMismatch, path,
                        QStringLiteral("Expected a role object."));
        const QJsonObject object = roles.at(index).toObject();
        ThemeColorRole role;
        if (!rejectUnknownFields(object,
                {QStringLiteral("key"), QStringLiteral("group"),
                 QStringLiteral("label"), QStringLiteral("description"),
                 QStringLiteral("bindings")}, file, path, result.diagnostics)
            || !readRequiredString(object, QStringLiteral("key"), file, path,
                                   role.key, result.diagnostics)
            || !readRequiredString(object, QStringLiteral("group"), file, path,
                                   role.group, result.diagnostics)
            || !readRequiredString(object, QStringLiteral("label"), file, path,
                                   role.label, result.diagnostics)
            || !readRequiredString(object, QStringLiteral("description"), file, path,
                                   role.description, result.diagnostics))
            return result;
        if (roleKeys.contains(role.key))
            return fail(ThemePackageErrorCode::InvalidValue, path + QStringLiteral(".key"),
                        QStringLiteral("Color role keys must be unique."));
        roleKeys.insert(role.key);
        if (!object.value(QStringLiteral("bindings")).isArray()
            || object.value(QStringLiteral("bindings")).toArray().isEmpty())
            return fail(ThemePackageErrorCode::InvalidValue,
                        path + QStringLiteral(".bindings"),
                        QStringLiteral("Each color role requires at least one binding."));
        const QJsonArray bindings = object.value(QStringLiteral("bindings")).toArray();
        for (qsizetype bindingIndex = 0; bindingIndex < bindings.size(); ++bindingIndex)
        {
            const QString bindingPath = path + QStringLiteral(".bindings[%1]").arg(bindingIndex);
            if (!bindings.at(bindingIndex).isObject())
                return fail(ThemePackageErrorCode::FieldTypeMismatch, bindingPath,
                            QStringLiteral("Expected a color binding object."));
            const QJsonObject bindingObject = bindings.at(bindingIndex).toObject();
            ThemeColorBinding binding;
            if (!rejectUnknownFields(bindingObject,
                    {QStringLiteral("target"), QStringLiteral("alpha")}, file,
                    bindingPath, result.diagnostics)
                || !readRequiredString(bindingObject, QStringLiteral("target"),
                                       file, bindingPath, binding.target,
                                       result.diagnostics))
                return result;
            if (targets.contains(binding.target))
                return fail(ThemePackageErrorCode::InvalidValue,
                            bindingPath + QStringLiteral(".target"),
                            QStringLiteral("A render color target can be bound only once."));
            targets.insert(binding.target);
            if (bindingObject.contains(QStringLiteral("alpha")))
            {
                const QJsonValue alpha = bindingObject.value(QStringLiteral("alpha"));
                const double value = alpha.toDouble(-1.0);
                if (!alpha.isDouble() || !std::isfinite(value) || value < 0.0
                    || value > 255.0 || std::floor(value) != value)
                    return fail(ThemePackageErrorCode::InvalidValue,
                                bindingPath + QStringLiteral(".alpha"),
                                QStringLiteral("Binding alpha must be an integer from 0 to 255."));
                binding.alpha = static_cast<int>(value);
            }
            role.bindings.append(std::move(binding));
        }
        dto.roles.append(std::move(role));
    }

    static const QRegularExpression colorPattern(QStringLiteral("^#(?:[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"));
    static const QRegularExpression idPattern(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"));
    QSet<QString> paletteIds;
    const QJsonArray palettes = root->value(QStringLiteral("palettes")).toArray();
    for (qsizetype index = 0; index < palettes.size(); ++index)
    {
        const QString path = QStringLiteral("$.palettes[%1]").arg(index);
        if (!palettes.at(index).isObject())
            return fail(ThemePackageErrorCode::FieldTypeMismatch, path,
                        QStringLiteral("Expected a palette object."));
        const QJsonObject object = palettes.at(index).toObject();
        ThemePaletteDto palette;
        QString scheme;
        QJsonObject colors;
        if (!rejectUnknownFields(object,
                {QStringLiteral("id"), QStringLiteral("displayName"),
                 QStringLiteral("colorScheme"), QStringLiteral("colors")},
                file, path, result.diagnostics)
            || !readRequiredString(object, QStringLiteral("id"), file, path,
                                   palette.id, result.diagnostics)
            || !readRequiredString(object, QStringLiteral("displayName"), file, path,
                                   palette.displayName, result.diagnostics)
            || !readRequiredString(object, QStringLiteral("colorScheme"), file, path,
                                   scheme, result.diagnostics)
            || !readRequiredObject(object, QStringLiteral("colors"), file, path,
                                   colors, result.diagnostics))
            return result;
        if (!idPattern.match(palette.id).hasMatch() || paletteIds.contains(palette.id))
            return fail(ThemePackageErrorCode::InvalidValue, path + QStringLiteral(".id"),
                        QStringLiteral("Palette IDs must be stable and unique within the theme."));
        paletteIds.insert(palette.id);
        if (scheme != QStringLiteral("dark") && scheme != QStringLiteral("light"))
            return fail(ThemePackageErrorCode::InvalidValue, path + QStringLiteral(".colorScheme"),
                        QStringLiteral("Color scheme must be dark or light."));
        palette.colorScheme = scheme == QStringLiteral("light")
            ? ThemeColorScheme::Light : ThemeColorScheme::Dark;
        for (auto color = colors.constBegin(); color != colors.constEnd(); ++color)
        {
            const QString colorPath = path + QStringLiteral(".colors.") + color.key();
            if (!roleKeys.contains(color.key()))
                return fail(ThemePackageErrorCode::UnknownSemanticKey, colorPath,
                            QStringLiteral("Palette color does not reference a declared role."));
            if (!color->isString() || !colorPattern.match(color->toString()).hasMatch())
                return fail(ThemePackageErrorCode::InvalidColor, colorPath,
                            QStringLiteral("Palette colors must use #RRGGBB or #AARRGGBB."));
            palette.colors.insert(color.key(), QColor(color->toString()));
        }
        if (palette.colors.size() != roleKeys.size())
            return fail(ThemePackageErrorCode::MissingField, path + QStringLiteral(".colors"),
                        QStringLiteral("Each palette must provide every declared color role."));
        dto.palettes.append(std::move(palette));
    }
    if (!paletteIds.contains(dto.defaultPalette))
        return fail(ThemePackageErrorCode::InvalidValue, QStringLiteral("$.defaultPalette"),
                    QStringLiteral("The default palette must reference a declared palette."));
    result.value = std::move(dto);
    return result;
}

ThemePackageParseResult<ThemeTokenDto> parseThemeTokens(
    const QByteArray& json,
    const QString& file)
{
    ParseResult<ThemeTokenDto> result;
    const std::optional<QJsonObject> root = parseRootObject(json, file, result);
    if (!root)
        return result;
    if (!rejectUnknownFields(
            *root,
            {QStringLiteral("colors"), QStringLiteral("metrics"),
             QStringLiteral("fonts")},
            file, QStringLiteral("$"), result.diagnostics))
        return result;
    ThemeTokenDto dto;
    if (!readSemanticValues(*root, file, QStringLiteral("colors"),
                            dto.colors, result.diagnostics)
        || !readSemanticValues(*root, file, QStringLiteral("metrics"),
                               dto.metrics, result.diagnostics)
        || !readFontRoles(*root, file, dto.fonts, result.diagnostics))
        return result;
    if (!root->contains(QStringLiteral("colors"))
        && !root->contains(QStringLiteral("metrics"))
        && !root->contains(QStringLiteral("fonts")))
    {
        result.diagnostics.append(diagnostic(
            ThemePackageErrorCode::MissingField, file, QStringLiteral("$"),
            QStringLiteral("At least one token section is required.")));
        return result;
    }
    result.value = std::move(dto);
    return result;
}

ThemePackageParseResult<ThemeVisualDto> parseThemeVisual(
    const QByteArray& json,
    const QString& file)
{
    return parseSemanticDocument<ThemeVisualDto>(json, file);
}

ThemePackageParseResult<ThemePresenterMapDto> parseThemePresenters(
    const QByteArray& json,
    const QString& file)
{
    ParseResult<ThemePresenterMapDto> result;
    const std::optional<QJsonObject> root = parseRootObject(json, file, result);
    if (!root)
        return result;
    if (!rejectUnknownFields(*root, {QStringLiteral("contexts")}, file,
                             QStringLiteral("$"), result.diagnostics))
        return result;
    QJsonObject contexts;
    if (!readRequiredObject(*root, QStringLiteral("contexts"), file,
                            QStringLiteral("$"), contexts,
                            result.diagnostics))
        return result;
    if (contexts.isEmpty())
        return failed<ThemePresenterMapDto>(
            ThemePackageErrorCode::InvalidValue, file,
            QStringLiteral("$.contexts"),
            QStringLiteral("At least one presenter context is required."));

    ThemePresenterMapDto dto;
    for (auto it = contexts.constBegin(); it != contexts.constEnd(); ++it)
    {
        const QString path = QStringLiteral("$.contexts.") + it.key();
        if (it.key().isEmpty() || !it.value().isString()
            || it.value().toString().isEmpty())
        {
            result.diagnostics.append(diagnostic(
                it.key().isEmpty() ? ThemePackageErrorCode::InvalidValue
                                   : ThemePackageErrorCode::FieldTypeMismatch,
                file, path, QStringLiteral("Expected a non-empty string.")));
            return result;
        }
        dto.contexts.insert(it.key(), it.value().toString());
    }
    result.value = std::move(dto);
    return result;
}

ThemePackageParseResult<ThemeRecipeMapDto> parseThemeRecipes(
    const QByteArray& json,
    const QString& file)
{
    ParseResult<ThemeRecipeMapDto> result;
    const std::optional<QJsonObject> root = parseRootObject(json, file, result);
    if (!root)
        return result;
    if (!rejectUnknownFields(*root, {QStringLiteral("recipes")}, file,
                             QStringLiteral("$"), result.diagnostics))
        return result;
    if (!root->contains(QStringLiteral("recipes")))
        return failed<ThemeRecipeMapDto>(
            ThemePackageErrorCode::MissingField, file,
            QStringLiteral("$.recipes"),
            QStringLiteral("Required field is missing."));
    const QJsonValue recipesValue = root->value(QStringLiteral("recipes"));
    if (!recipesValue.isArray())
        return failed<ThemeRecipeMapDto>(
            ThemePackageErrorCode::FieldTypeMismatch, file,
            QStringLiteral("$.recipes"), QStringLiteral("Expected an array."));
    const QJsonArray recipes = recipesValue.toArray();
    if (recipes.isEmpty())
        return failed<ThemeRecipeMapDto>(
            ThemePackageErrorCode::InvalidValue, file,
            QStringLiteral("$.recipes"),
            QStringLiteral("At least one component recipe is required."));

    ThemeRecipeMapDto dto;
    for (qsizetype recipeIndex = 0; recipeIndex < recipes.size(); ++recipeIndex)
    {
        const QString recipePath = QStringLiteral("$.recipes[")
            + QString::number(recipeIndex) + QLatin1Char(']');
        if (!recipes.at(recipeIndex).isObject())
            return failed<ThemeRecipeMapDto>(
                ThemePackageErrorCode::FieldTypeMismatch, file, recipePath,
                QStringLiteral("Expected an object."));
        const QJsonObject recipeObject = recipes.at(recipeIndex).toObject();
        if (!rejectUnknownFields(
                recipeObject,
                {QStringLiteral("host"), QStringLiteral("breakpoint"),
                 QStringLiteral("variant"), QStringLiteral("slots")},
                file, recipePath, result.diagnostics))
            return result;

        ThemeComponentRecipeDto recipe;
        if (!readRequiredString(recipeObject, QStringLiteral("host"), file,
                                recipePath, recipe.host, result.diagnostics)
            || !readRequiredString(recipeObject, QStringLiteral("variant"), file,
                                   recipePath, recipe.variant,
                                   result.diagnostics))
            return result;
        if (recipeObject.contains(QStringLiteral("breakpoint")))
        {
            if (!readRequiredString(recipeObject, QStringLiteral("breakpoint"),
                                    file, recipePath, recipe.breakpoint,
                                    result.diagnostics))
                return result;
        }
        if (!recipeObject.contains(QStringLiteral("slots")))
            return failed<ThemeRecipeMapDto>(
                ThemePackageErrorCode::MissingField, file,
                recipePath + QStringLiteral(".slots"),
                QStringLiteral("Required field is missing."));
        const QJsonValue slotsValue = recipeObject.value(QStringLiteral("slots"));
        if (!slotsValue.isArray())
            return failed<ThemeRecipeMapDto>(
                ThemePackageErrorCode::FieldTypeMismatch, file,
                recipePath + QStringLiteral(".slots"),
                QStringLiteral("Expected an array."));
        const QJsonArray slotItems = slotsValue.toArray();
        if (slotItems.isEmpty())
            return failed<ThemeRecipeMapDto>(
                ThemePackageErrorCode::InvalidValue, file,
                recipePath + QStringLiteral(".slots"),
                QStringLiteral("At least one slot is required."));

        for (qsizetype slotIndex = 0; slotIndex < slotItems.size(); ++slotIndex)
        {
            const QString slotPath = recipePath + QStringLiteral(".slots[")
                + QString::number(slotIndex) + QLatin1Char(']');
            if (!slotItems.at(slotIndex).isObject())
                return failed<ThemeRecipeMapDto>(
                    ThemePackageErrorCode::FieldTypeMismatch, file, slotPath,
                    QStringLiteral("Expected an object."));
            const QJsonObject slotObject = slotItems.at(slotIndex).toObject();
            if (!rejectUnknownFields(
                    slotObject,
                    {QStringLiteral("id"), QStringLiteral("visible"),
                     QStringLiteral("minimum"), QStringLiteral("preferred"),
                     QStringLiteral("maximum"), QStringLiteral("alignment"),
                     QStringLiteral("presenter"), QStringLiteral("asset")},
                    file, slotPath, result.diagnostics))
                return result;

            ThemeRecipeSlotDto slot;
            if (!readRequiredString(slotObject, QStringLiteral("id"), file,
                                    slotPath, slot.id, result.diagnostics))
                return result;
            if (slotObject.contains(QStringLiteral("visible")))
            {
                const QJsonValue visible = slotObject.value(
                    QStringLiteral("visible"));
                if (!visible.isBool())
                    return failed<ThemeRecipeMapDto>(
                        ThemePackageErrorCode::FieldTypeMismatch, file,
                        slotPath + QStringLiteral(".visible"),
                        QStringLiteral("Expected a boolean."));
                slot.visible = visible.toBool();
            }

            const QStringList sizeNames{
                QStringLiteral("minimum"), QStringLiteral("preferred"),
                QStringLiteral("maximum")};
            std::optional<int>* sizeValues[]{
                &slot.minimum, &slot.preferred, &slot.maximum};
            for (qsizetype sizeIndex = 0; sizeIndex < sizeNames.size(); ++sizeIndex)
            {
                const QString& name = sizeNames.at(sizeIndex);
                if (!slotObject.contains(name))
                    continue;
                const QJsonValue value = slotObject.value(name);
                const double numeric = value.toDouble();
                if (!value.isDouble() || !std::isfinite(numeric)
                    || std::floor(numeric) != numeric
                    || numeric < std::numeric_limits<int>::min()
                    || numeric > std::numeric_limits<int>::max())
                    return failed<ThemeRecipeMapDto>(
                        ThemePackageErrorCode::FieldTypeMismatch, file,
                        slotPath + QLatin1Char('.') + name,
                        QStringLiteral("Expected an integer."));
                *sizeValues[sizeIndex] = static_cast<int>(numeric);
            }
            if (slotObject.contains(QStringLiteral("alignment")))
            {
                if (!readRequiredString(slotObject, QStringLiteral("alignment"),
                                        file, slotPath, slot.alignment,
                                        result.diagnostics))
                    return result;
            }
            if (!readOptionalString(slotObject, QStringLiteral("presenter"), file,
                                    slotPath, slot.presenter,
                                    result.diagnostics)
                || !readOptionalString(slotObject, QStringLiteral("asset"), file,
                                       slotPath, slot.asset,
                                       result.diagnostics))
                return result;
            recipe.items.append(std::move(slot));
        }
        dto.recipes.append(std::move(recipe));
    }
    result.value = std::move(dto);
    return result;
}

ThemePackageParseResult<ThemeAssetMapDto> parseThemeAssetMap(
    const QByteArray& json,
    const QString& file)
{
    ParseResult<ThemeAssetMapDto> result;
    const std::optional<QJsonObject> root = parseRootObject(json, file, result);
    if (!root)
        return result;
    if (!rejectUnknownFields(*root, {QStringLiteral("assets")}, file,
                             QStringLiteral("$"), result.diagnostics))
        return result;
    QJsonObject assets;
    if (!readRequiredObject(*root, QStringLiteral("assets"), file,
                            QStringLiteral("$"), assets, result.diagnostics))
        return result;

    ThemeAssetMapDto dto;
    for (auto assetIt = assets.constBegin(); assetIt != assets.constEnd(); ++assetIt)
    {
        const QString assetPath = QStringLiteral("$.assets.") + assetIt.key();
        if (assetIt.key().isEmpty())
            return failed<ThemeAssetMapDto>(
                ThemePackageErrorCode::InvalidValue, file,
                QStringLiteral("$.assets"),
                QStringLiteral("Logical asset IDs must not be empty."));
        if (!assetIt.value().isObject())
            return failed<ThemeAssetMapDto>(
                ThemePackageErrorCode::FieldTypeMismatch, file, assetPath,
                QStringLiteral("Expected an object."));
        const QJsonObject assetObject = assetIt.value().toObject();
        if (!rejectUnknownFields(
            assetObject,
            {QStringLiteral("type"), QStringLiteral("sources"),
             QStringLiteral("insets")},
            file, assetPath, result.diagnostics))
            return result;

        QString type;
        if (!readRequiredString(assetObject, QStringLiteral("type"), file,
                                assetPath, type, result.diagnostics))
            return result;
        ThemeAssetDto asset;
        if (type == QStringLiteral("image"))
            asset.kind = ThemeAssetKind::Image;
        else if (type == QStringLiteral("ninePatch"))
            asset.kind = ThemeAssetKind::NinePatch;
        else if (type == QStringLiteral("frameSequence"))
            asset.kind = ThemeAssetKind::FrameSequence;
        else if (type == QStringLiteral("font"))
            asset.kind = ThemeAssetKind::Font;
        else
            return failed<ThemeAssetMapDto>(
                ThemePackageErrorCode::InvalidValue, file,
                assetPath + QStringLiteral(".type"),
                QStringLiteral("Unsupported asset type."));

        if (assetObject.contains(QStringLiteral("insets")))
        {
            if (asset.kind != ThemeAssetKind::NinePatch
                || !assetObject.value(QStringLiteral("insets")).isObject())
                return failed<ThemeAssetMapDto>(
                    ThemePackageErrorCode::InvalidValue, file,
                    assetPath + QStringLiteral(".insets"),
                    QStringLiteral("Insets are only valid for nine-patch assets."));
            const QJsonObject insets = assetObject.value(
                QStringLiteral("insets")).toObject();
            if (!rejectUnknownFields(
                    insets,
                    {QStringLiteral("left"), QStringLiteral("top"),
                     QStringLiteral("right"), QStringLiteral("bottom")},
                    file, assetPath + QStringLiteral(".insets"),
                    result.diagnostics))
                return result;
            ThemeAssetInsetsDto parsedInsets;
            const QStringList names = {
                QStringLiteral("left"), QStringLiteral("top"),
                QStringLiteral("right"), QStringLiteral("bottom")};
            int* values[] = {&parsedInsets.left, &parsedInsets.top,
                             &parsedInsets.right, &parsedInsets.bottom};
            for (qsizetype index = 0; index < names.size(); ++index)
            {
                const QJsonValue value = insets.value(names.at(index));
                if (!value.isDouble() || value.toDouble() < 0.0
                    || std::floor(value.toDouble()) != value.toDouble())
                    return failed<ThemeAssetMapDto>(
                        ThemePackageErrorCode::InvalidValue, file,
                        assetPath + QStringLiteral(".insets.") + names.at(index),
                        QStringLiteral("Insets must be non-negative integers."));
                *values[index] = value.toInt();
            }
            asset.insets = parsedInsets;
        }
        else if (asset.kind == ThemeAssetKind::NinePatch)
            return failed<ThemeAssetMapDto>(
                ThemePackageErrorCode::MissingField, file,
                assetPath + QStringLiteral(".insets"),
                QStringLiteral("Nine-patch assets require insets."));

        if (!assetObject.contains(QStringLiteral("sources")))
            return failed<ThemeAssetMapDto>(
                ThemePackageErrorCode::MissingField, file,
                assetPath + QStringLiteral(".sources"),
                QStringLiteral("Required field is missing."));
        const QJsonValue sourcesValue = assetObject.value(QStringLiteral("sources"));
        if (!sourcesValue.isArray())
            return failed<ThemeAssetMapDto>(
                ThemePackageErrorCode::FieldTypeMismatch, file,
                assetPath + QStringLiteral(".sources"),
                QStringLiteral("Expected an array."));
        const QJsonArray sources = sourcesValue.toArray();
        if (sources.isEmpty())
            return failed<ThemeAssetMapDto>(
                ThemePackageErrorCode::InvalidValue, file,
                assetPath + QStringLiteral(".sources"),
                QStringLiteral("At least one asset source is required."));

        for (qsizetype index = 0; index < sources.size(); ++index)
        {
            const QString sourcePath = assetPath + QStringLiteral(".sources[")
                + QString::number(index) + QLatin1Char(']');
            if (!sources.at(index).isObject())
                return failed<ThemeAssetMapDto>(
                    ThemePackageErrorCode::FieldTypeMismatch, file, sourcePath,
                    QStringLiteral("Expected an object."));
            const QJsonObject sourceObject = sources.at(index).toObject();
            if (!rejectUnknownFields(
                sourceObject,
                {QStringLiteral("path"), QStringLiteral("scale")},
                file, sourcePath, result.diagnostics))
                return result;
            ThemeAssetSourceDto source;
            if (!readRequiredString(sourceObject, QStringLiteral("path"), file,
                                    sourcePath, source.path,
                                    result.diagnostics))
                return result;
            if (sourceObject.contains(QStringLiteral("scale")))
            {
                const QJsonValue scale = sourceObject.value(QStringLiteral("scale"));
                if (!scale.isDouble() || !std::isfinite(scale.toDouble())
                    || scale.toDouble() <= 0.0)
                    return failed<ThemeAssetMapDto>(
                        ThemePackageErrorCode::InvalidValue, file,
                        sourcePath + QStringLiteral(".scale"),
                        QStringLiteral("Scale must be a positive finite number."));
                source.scale = scale.toDouble();
            }
            asset.sources.append(std::move(source));
        }
        dto.assets.insert(assetIt.key(), std::move(asset));
    }
    result.value = std::move(dto);
    return result;
}

}
