#include "Templates/ProjectTemplateCatalog.h"

#include "Platform/PlatformPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

#include <cmath>

namespace VNH {

namespace {

constexpr qint64 kMaximumTemplateBytes = 10 * 1024 * 1024;
constexpr int kLegacyFormatVersion = 1;
constexpr int kCurrentFormatVersion = 2;
constexpr int kSoundType = 1;
constexpr int kLayerType = 5;
constexpr int kBusType = 6;
constexpr int kActionType = 7;
constexpr int kVariableType = 8;
constexpr int kAttenuationType = 9;

QString text(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

void addIssue(TemplateValidationResult& result, const QString& path,
              const QString& message)
{
    result.issues.push_back({path, message});
}

bool isInteger(const QJsonValue& value)
{
    return value.isDouble() && std::isfinite(value.toDouble()) &&
           std::floor(value.toDouble()) == value.toDouble();
}

bool isInvalidName(const QString& name)
{
    if (name.isEmpty())
        return true;
    for (const QChar character : name) {
        if (character == QLatin1Char('/') || character == QLatin1Char('\\') ||
            character.unicode() < 0x20)
            return true;
    }
    return false;
}

void validateBusNode(const QJsonValue& value, const QString& path,
                     QSet<QString>& seenNames,
                     TemplateValidationResult& result)
{
    if (!value.isObject()) {
        addIssue(result, path, text(QT_TRANSLATE_NOOP("HarkenHub", "must be an object")));
        return;
    }
    const QJsonObject object = value.toObject();
    if (object.contains(QStringLiteral("type"))) {
        const QJsonValue type = object.value(QStringLiteral("type"));
        if (!isInteger(type) || type.toInt() != kBusType)
            addIssue(result, path + QStringLiteral(".type"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "must be the Bus project item type")));
    }

    const QJsonValue nameValue = object.value(QStringLiteral("name"));
    if (!nameValue.isString()) {
        addIssue(result, path + QStringLiteral(".name"),
                 text(QT_TRANSLATE_NOOP("HarkenHub", "missing or non-string")));
    } else {
        const QString name = nameValue.toString();
        if (isInvalidName(name)) {
            addIssue(result, path + QStringLiteral(".name"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "invalid name")));
        } else if (seenNames.contains(name)) {
            addIssue(result, path + QStringLiteral(".name"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "duplicate name: %1")).arg(name));
        } else {
            seenNames.insert(name);
        }
    }

    if (!object.contains(QStringLiteral("children")))
        return;
    const QJsonValue childrenValue = object.value(QStringLiteral("children"));
    if (!childrenValue.isArray()) {
        addIssue(result, path + QStringLiteral(".children"),
                 text(QT_TRANSLATE_NOOP("HarkenHub", "must be an array")));
        return;
    }
    const QJsonArray children = childrenValue.toArray();
    for (int index = 0; index < children.size(); ++index) {
        validateBusNode(children.at(index),
                        QStringLiteral("%1.children[%2]").arg(path).arg(index),
                        seenNames, result);
    }
}

void validatePoints(const QJsonValue& value, const QString& path,
                    double maximum, TemplateValidationResult& result)
{
    if (!value.isArray()) {
        addIssue(result, path, text(QT_TRANSLATE_NOOP("HarkenHub", "must be an array of [x,y] pairs")));
        return;
    }
    const QJsonArray points = value.toArray();
    if (points.size() < 2) {
        addIssue(result, path, text(QT_TRANSLATE_NOOP("HarkenHub", "must have at least 2 points")));
        return;
    }
    double previousX = 0.0;
    for (int index = 0; index < points.size(); ++index) {
        const QString pointPath =
            QStringLiteral("%1[%2]").arg(path).arg(index);
        if (!points.at(index).isArray()) {
            addIssue(result, pointPath, text(QT_TRANSLATE_NOOP("HarkenHub", "must be a [x,y] array")));
            continue;
        }
        const QJsonArray point = points.at(index).toArray();
        if (point.size() != 2 || !point.at(0).isDouble() ||
            !point.at(1).isDouble() ||
            !std::isfinite(point.at(0).toDouble()) ||
            !std::isfinite(point.at(1).toDouble())) {
            addIssue(result, pointPath, text(QT_TRANSLATE_NOOP("HarkenHub", "must be exactly two numbers")));
            continue;
        }
        const double x = point.at(0).toDouble();
        if (x < 0.0 || x > maximum)
            addIssue(result, pointPath,
                     text(QT_TRANSLATE_NOOP("HarkenHub", "x must be between 0 and %1")).arg(maximum));
        if (index > 0 && x < previousX)
            addIssue(result, pointPath,
                     text(QT_TRANSLATE_NOOP("HarkenHub", "x values must be monotonically increasing")));
        previousX = x;
    }
}

void validateAttenuations(const QJsonValue& value,
                          TemplateValidationResult& result)
{
    if (!value.isArray()) {
        addIssue(result, QStringLiteral("attenuation"),
                 text(QT_TRANSLATE_NOOP("HarkenHub", "must be an array")));
        return;
    }
    QSet<QString> seenNames;
    const QJsonArray entries = value.toArray();
    for (int index = 0; index < entries.size(); ++index) {
        const QString path = QStringLiteral("attenuation[%1]").arg(index);
        if (!entries.at(index).isObject()) {
            addIssue(result, path, text(QT_TRANSLATE_NOOP("HarkenHub", "must be an object")));
            continue;
        }
        const QJsonObject object = entries.at(index).toObject();
        if (object.contains(QStringLiteral("type"))) {
            const QJsonValue type = object.value(QStringLiteral("type"));
            if (!isInteger(type) || type.toInt() != kAttenuationType)
                addIssue(result, path + QStringLiteral(".type"),
                         text(QT_TRANSLATE_NOOP("HarkenHub", "must be the Attenuation project item type")));
        }
        const QString name = object.value(QStringLiteral("name")).toString();
        if (isInvalidName(name)) {
            addIssue(result, path + QStringLiteral(".name"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "missing or invalid name")));
        } else if (seenNames.contains(name)) {
            addIssue(result, path + QStringLiteral(".name"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "duplicate name: %1")).arg(name));
        } else {
            seenNames.insert(name);
        }
        const QJsonValue maximumValue = object.value(QStringLiteral("max"));
        if (!maximumValue.isDouble() ||
            !std::isfinite(maximumValue.toDouble())) {
            addIssue(result, path + QStringLiteral(".max"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "must be a number")));
            continue;
        }
        const double maximum = maximumValue.toDouble();
        if (maximum <= 0.0) {
            addIssue(result, path + QStringLiteral(".max"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "must be greater than 0")));
            continue;
        }
        const QJsonValue volumeValue = object.value(QStringLiteral("volume"));
        if (!volumeValue.isObject()) {
            addIssue(result, path + QStringLiteral(".volume"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "must be an object")));
            continue;
        }
        const QJsonObject volume = volumeValue.toObject();
        if (!volume.value(QStringLiteral("curve")).isString() ||
            volume.value(QStringLiteral("curve")).toString().isEmpty()) {
            addIssue(result, path + QStringLiteral(".volume.curve"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "missing or empty curve type")));
        }
        validatePoints(volume.value(QStringLiteral("points")),
                       path + QStringLiteral(".volume.points"), maximum,
                       result);
    }
}

void validateAuthoringArray(const QJsonObject& root, const QString& key,
                            std::optional<int> expectedType,
                            TemplateValidationResult& result)
{
    if (!root.contains(key))
        return;
    const QJsonValue value = root.value(key);
    if (!value.isArray()) {
        addIssue(result, key, text(QT_TRANSLATE_NOOP("HarkenHub", "must be an array")));
        return;
    }
    const QJsonArray entries = value.toArray();
    for (int index = 0; index < entries.size(); ++index) {
        const QString path = QStringLiteral("%1[%2]").arg(key).arg(index);
        if (!entries.at(index).isObject()) {
            addIssue(result, path, text(QT_TRANSLATE_NOOP("HarkenHub", "must be an object")));
            continue;
        }
        const QJsonValue type =
            entries.at(index).toObject().value(QStringLiteral("type"));
        if (!isInteger(type)) {
            addIssue(result, path + QStringLiteral(".type"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "must be an integer")));
            continue;
        }
        if (expectedType && type.toInt() != *expectedType) {
            addIssue(result, path + QStringLiteral(".type"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "unexpected project item type")));
        } else if (key == QStringLiteral("content") &&
                   (type.toInt() < kSoundType || type.toInt() > kLayerType)) {
            addIssue(result, path + QStringLiteral(".type"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "must be a content project item type")));
        }
    }
}

TemplateValidationResult validateDocument(const QJsonDocument& document,
                                          const QString& sourceLabel)
{
    TemplateValidationResult result;
    if (!document.isObject()) {
        result.summary = text(QT_TRANSLATE_NOOP("HarkenHub", "The top-level JSON must be an object."));
        addIssue(result, sourceLabel.isEmpty() ? QStringLiteral("<root>")
                                              : sourceLabel,
                 text(QT_TRANSLATE_NOOP("HarkenHub", "not a JSON object")));
        return result;
    }
    const QJsonObject root = document.object();
    const QJsonValue version =
        root.value(QStringLiteral("templateFormatVersion"));
    if (!version.isUndefined() && !isInteger(version)) {
        addIssue(result, QStringLiteral("templateFormatVersion"),
                 text(QT_TRANSLATE_NOOP("HarkenHub", "must be an integer")));
    } else {
        const int number =
            version.isUndefined() ? kLegacyFormatVersion : version.toInt();
        if (number < kLegacyFormatVersion || number > kCurrentFormatVersion)
            addIssue(result, QStringLiteral("templateFormatVersion"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "unsupported template format version: %1")).arg(number));
    }
    const QJsonValue displayName = root.value(QStringLiteral("displayName"));
    if (!displayName.isString() || displayName.toString().trimmed().isEmpty())
        addIssue(result, QStringLiteral("displayName"),
                 text(QT_TRANSLATE_NOOP("HarkenHub", "missing or empty string")));
    const QJsonValue description = root.value(QStringLiteral("description"));
    if (!description.isString())
        addIssue(result, QStringLiteral("description"),
                 text(QT_TRANSLATE_NOOP("HarkenHub", "missing string")));

    if (root.contains(QStringLiteral("attenuation")))
        validateAttenuations(root.value(QStringLiteral("attenuation")), result);
    validateAuthoringArray(root, QStringLiteral("content"), std::nullopt,
                           result);
    validateAuthoringArray(root, QStringLiteral("variables"), kVariableType,
                           result);
    validateAuthoringArray(root, QStringLiteral("actions"), kActionType,
                           result);
    static const QSet<QString> supportedSections = {
        QStringLiteral("templateFormatVersion"),
        QStringLiteral("displayName"),
        QStringLiteral("description"),
        QStringLiteral("bus"),
        QStringLiteral("attenuation"),
        QStringLiteral("content"),
        QStringLiteral("variables"),
        QStringLiteral("actions")};
    for (auto section = root.constBegin(); section != root.constEnd();
         ++section) {
        if (!supportedSections.contains(section.key()))
            addIssue(result, section.key(),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "unsupported template section")));
    }

    const QJsonValue bus = root.value(QStringLiteral("bus"));
    if (!bus.isObject()) {
        addIssue(result, QStringLiteral("bus"),
                 text(QT_TRANSLATE_NOOP("HarkenHub", "missing object")));
    } else {
        if (bus.toObject().value(QStringLiteral("name")).toString() !=
            QStringLiteral("Master Bus")) {
            addIssue(result, QStringLiteral("bus.name"),
                     text(QT_TRANSLATE_NOOP("HarkenHub", "must be 'Master Bus'")));
        }
        QSet<QString> seenNames;
        validateBusNode(bus, QStringLiteral("bus"), seenNames, result);
    }

    result.valid = result.issues.empty();
    result.summary = result.valid
                         ? text(QT_TRANSLATE_NOOP("HarkenHub", "The template is valid."))
                         : text(QT_TRANSLATE_NOOP("HarkenHub", "Found %1 issue(s) in %2:"))
                               .arg(result.issues.size())
                               .arg(sourceLabel);
    return result;
}

QByteArray readTemplate(const QString& path, TemplateValidationResult* result)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (result) {
            result->summary = text(QT_TRANSLATE_NOOP("HarkenHub", "The template could not be read."));
            addIssue(*result, path, file.errorString());
        }
        return {};
    }
    if (file.size() <= 0 || file.size() > kMaximumTemplateBytes) {
        if (result) {
            result->summary = text(QT_TRANSLATE_NOOP("HarkenHub", "The template file size is invalid."));
            addIssue(*result, path,
                     text(QT_TRANSLATE_NOOP("HarkenHub", "The file must be between 1 byte and 10 MB.")));
        }
        return {};
    }
    return file.readAll();
}

TemplateValidationResult readAndValidateTemplate(
    const QString& path, QJsonDocument* document)
{
    TemplateValidationResult result;
    const QByteArray bytes = readTemplate(path, &result);
    if (bytes.isEmpty())
        return result;
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.summary = text(QT_TRANSLATE_NOOP(
            "HarkenHub", "The template contains invalid JSON."));
        addIssue(result, QFileInfo(path).fileName(),
                 text(QT_TRANSLATE_NOOP("HarkenHub", "%1 at offset %2"))
                     .arg(parseError.errorString())
                     .arg(parseError.offset));
        return result;
    }
    result = validateDocument(parsed, QFileInfo(path).fileName());
    if (result.valid && document)
        *document = parsed;
    return result;
}

QString uniqueId(const QString& directory, const QString& stem)
{
    QString safeStem = stem.trimmed();
    safeStem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                     QStringLiteral("_"));
    if (safeStem.isEmpty())
        safeStem = QStringLiteral("template");
    QDir dir(directory);
    if (!dir.exists(safeStem + QStringLiteral(".json")))
        return safeStem;
    for (int index = 2; index < 1000; ++index) {
        const QString candidate =
            QStringLiteral("%1_%2").arg(safeStem).arg(index);
        if (!dir.exists(candidate + QStringLiteral(".json")))
            return candidate;
    }
    return safeStem + QStringLiteral("_imported");
}

}

QString TemplateValidationResult::toUserText() const
{
    QStringList lines;
    if (!summary.isEmpty())
        lines.append(summary);
    for (const TemplateValidationIssue& issue : issues)
        lines.append(QStringLiteral("- %1: %2").arg(issue.path, issue.message));
    return lines.join(QLatin1Char('\n'));
}

QString ProjectTemplateCatalog::userTemplateDir()
{
    return QDir(userDataDir()).filePath(QStringLiteral("Templates"));
}

TemplateValidationResult ProjectTemplateCatalog::validateFile(
    const QString& path)
{
    return readAndValidateTemplate(path, nullptr);
}

std::optional<ProjectTemplateEntry> ProjectTemplateCatalog::loadFile(
    const QString& path, bool userTemplate, TemplateValidationResult* result)
{
    QJsonDocument document;
    TemplateValidationResult validation =
        readAndValidateTemplate(path, &document);
    if (result)
        *result = validation;
    if (!validation.valid)
        return std::nullopt;

    const QJsonObject object = document.object();
    ProjectTemplateEntry entry;
    entry.id = QFileInfo(path).completeBaseName();
    entry.displayName =
        object.value(QStringLiteral("displayName")).toString().trimmed();
    entry.description =
        object.value(QStringLiteral("description")).toString();
    entry.filePath = QFileInfo(path).absoluteFilePath();
    entry.content = object;
    entry.userTemplate = userTemplate;
    return entry;
}

std::vector<ProjectTemplateEntry> ProjectTemplateCatalog::listAvailable(
    const QString& builtInTemplateDir)
{
    std::vector<ProjectTemplateEntry> entries;
    const auto appendDirectory = [&entries](const QString& path,
                                             bool userTemplate) {
        QDir dir(path);
        const QFileInfoList files = dir.entryInfoList(
            {QStringLiteral("*.json")}, QDir::Files | QDir::Readable,
            QDir::Name);
        for (const QFileInfo& file : files) {
            const auto entry = loadFile(file.absoluteFilePath(), userTemplate);
            if (entry)
                entries.push_back(*entry);
        }
    };
    appendDirectory(builtInTemplateDir, false);
    appendDirectory(userTemplateDir(), true);
    return entries;
}

std::optional<ProjectTemplateEntry> ProjectTemplateCatalog::importFromFile(
    const QString& sourcePath, TemplateValidationResult* result)
{
    TemplateValidationResult validation;
    const auto source = loadFile(sourcePath, true, &validation);
    if (!source) {
        if (result)
            *result = validation;
        return std::nullopt;
    }

    const QByteArray sourceHash = QCryptographicHash::hash(
        QJsonDocument(source->content).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
    QDir userDir(userTemplateDir());
    if (userDir.exists()) {
        const QFileInfoList existingFiles = userDir.entryInfoList(
            {QStringLiteral("*.json")}, QDir::Files | QDir::Readable,
            QDir::Name);
        for (const QFileInfo& file : existingFiles) {
            const auto existing = loadFile(file.absoluteFilePath(), true);
            if (!existing)
                continue;
            const QByteArray existingHash = QCryptographicHash::hash(
                QJsonDocument(existing->content).toJson(QJsonDocument::Compact),
                QCryptographicHash::Sha256);
            if (existingHash == sourceHash) {
                if (result)
                    *result = validation;
                return existing;
            }
        }
    }
    if (!QDir().mkpath(userTemplateDir())) {
        validation.valid = false;
        validation.summary = text(QT_TRANSLATE_NOOP("HarkenHub", "The template could not be added."));
        validation.issues.clear();
        addIssue(validation, userTemplateDir(),
                 text(QT_TRANSLATE_NOOP("HarkenHub", "The template directory could not be created.")));
        if (result)
            *result = validation;
        return std::nullopt;
    }

    const QString id = uniqueId(userTemplateDir(),
                                QFileInfo(sourcePath).completeBaseName());
    const QString destination =
        QDir(userTemplateDir()).filePath(id + QStringLiteral(".json"));
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(QJsonDocument(source->content).toJson(
            QJsonDocument::Indented)) < 0 ||
        !output.commit()) {
        validation.valid = false;
        validation.summary = text(QT_TRANSLATE_NOOP("HarkenHub", "The template could not be added."));
        validation.issues.clear();
        addIssue(validation, destination, output.errorString());
        if (result)
            *result = validation;
        return std::nullopt;
    }
    if (result)
        *result = validation;
    return loadFile(destination, true);
}

}
