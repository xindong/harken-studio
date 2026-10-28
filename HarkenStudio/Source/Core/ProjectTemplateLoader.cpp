#include "ProjectTemplateLoader.h"
#include "SoundSourceSemantics.h"
#include "AuthoringPolicy.h"
#include "ItemTypeCatalog.h"
#include "NameValidator.h"
#include "ProjectTypes.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QCoreApplication>
#include <QCryptographicHash>

#include <cmath>
#include <set>

namespace VNX {

namespace {

std::optional<ProjectItem::Type> templateItemType(const QJsonValue& value)
{
    if (!value.isString()) return std::nullopt;
    return ItemTypeCatalog::parseTypeName(
        value.toString(), Qt::CaseSensitive, false);
}

bool isTemplateContentType(ProjectItem::Type type)
{
    return type == ProjectItem::Type::Sound
        || type == ProjectItem::Type::Container
        || type == ProjectItem::Type::Track
        || type == ProjectItem::Type::Segment
        || type == ProjectItem::Type::Layer;
}

QJsonValue pointToJson(const QPointF& p) {
    return QJsonArray{p.x(), p.y()};
}

QJsonObject busNodeToJson(const TemplateBusNode& n) {
    QJsonObject o = n.authoring;
    o["name"] = n.name;
    if (!n.children.empty()) {
        QJsonArray arr;
        for (const auto& c : n.children)
            arr.append(busNodeToJson(c));
        o["children"] = arr;
    }
    return o;
}

}

QJsonObject ProjectTemplateData::toJson() const {
    QJsonObject root;
    root["templateFormatVersion"] = kCurrentFormatVersion;
    root["displayName"] = displayName;
    root["description"] = description;
    root["bus"] = busNodeToJson(bus);

    QJsonArray at;
    for (const auto& a : attenuations) {
        QJsonArray pts;
        for (const auto& p : a.volumePoints)
            pts.append(pointToJson(p));
        QJsonObject value = a.authoring;
        value["name"] = a.name;
        value["max"] = a.maxDistance;
        value["volume"] = QJsonObject{{"curve", a.volumeCurve}, {"points", pts}};
        at.append(value);
    }
    root["attenuation"] = at;
    if (!content.isEmpty()) root["content"] = content;
    if (!variables.isEmpty()) root["variables"] = variables;
    if (!actions.isEmpty()) root["actions"] = actions;

    return root;
}


QString ValidationResult::toUserText() const {
    if (valid && errors.empty())
        return QStringLiteral("Template is valid.");
    QStringList lines;
    if (!summary.isEmpty())
        lines << summary;
    for (const auto& e : errors)
        lines << QStringLiteral("  • %1: %2").arg(e.jsonPath, e.message);
    return lines.join(QChar('\n'));
}


namespace {

void validateBusNodeJson(const QJsonValue& v,
                         const QString& path,
                         std::vector<ValidationError>& errors);

void validatePoints(const QJsonValue& v,
                    const QString& path,
                    bool requireMonotonicX,
                    std::optional<double> xMin,
                    std::optional<double> xMax,
                    std::vector<ValidationError>& errors) {
    if (!v.isArray()) {
        errors.push_back({path, QStringLiteral("must be an array of [x,y] pairs")});
        return;
    }
    auto arr = v.toArray();
    if (arr.size() < 2) {
        errors.push_back({path, QStringLiteral("must have at least 2 points")});
        return;
    }
    double prevX = 0.0;
    for (int i = 0; i < arr.size(); ++i) {
        const auto& it = arr.at(i);
        const QString itPath = QStringLiteral("%1[%2]").arg(path).arg(i);
        if (!it.isArray()) {
            errors.push_back({itPath,
                              QStringLiteral("must be a [x,y] array")});
            continue;
        }
        auto pair = it.toArray();
        if (pair.size() != 2 || !pair.at(0).isDouble() || !pair.at(1).isDouble()) {
            errors.push_back({itPath,
                              QStringLiteral("must be exactly two numbers")});
            continue;
        }
        const double x = pair.at(0).toDouble();
        if (xMin.has_value() && x < *xMin) {
            errors.push_back({itPath,
                              QStringLiteral("x=%1 below min %2").arg(x).arg(*xMin)});
        }
        if (xMax.has_value() && x > *xMax) {
            errors.push_back({itPath,
                              QStringLiteral("x=%1 exceeds max %2").arg(x).arg(*xMax)});
        }
        if (requireMonotonicX && i > 0 && x < prevX) {
            errors.push_back({itPath,
                              QStringLiteral("x not monotonically increasing (%1 < %2)")
                                  .arg(x).arg(prevX)});
        }
        prevX = x;
    }
}

void validateBusNodeJson(const QJsonValue& v,
                         const QString& path,
                         std::vector<ValidationError>& errors) {
    if (!v.isObject()) {
        errors.push_back({path, QStringLiteral("must be an object")});
        return;
    }
    auto obj = v.toObject();

    if (obj.contains(QStringLiteral("type"))) {
        const auto type = templateItemType(obj.value(QStringLiteral("type")));
        if (!type || *type != ProjectItem::Type::Bus) {
            errors.push_back({path + QStringLiteral(".type"),
                              QStringLiteral("must be the Bus project item type")});
        }
    }

    QString name;
    if (!obj.contains("name") || !obj["name"].isString()) {
        errors.push_back({path + QStringLiteral(".name"),
                          QStringLiteral("missing or non-string")});
    } else {
        name = obj["name"].toString();
        if (!NameValidator::validateDisplayName(name)) {
            errors.push_back({path + QStringLiteral(".name"),
                              QStringLiteral("invalid name '%1'").arg(name)});
        }
    }

    if (obj.contains("children")) {
        const auto& cs = obj["children"];
        if (!cs.isArray()) {
            errors.push_back({path + QStringLiteral(".children"),
                              QStringLiteral("must be an array")});
        } else {
            const auto arr = cs.toArray();
            for (int i = 0; i < arr.size(); ++i) {
                validateBusNodeJson(arr.at(i),
                                    QStringLiteral("%1.children[%2]").arg(path).arg(i),
                                    errors);
            }
        }
    }
}

void validateAttenuationArray(const QJsonValue& v,
                              std::vector<ValidationError>& errors) {
    if (!v.isArray()) {
        errors.push_back({QStringLiteral("attenuation"),
                          QStringLiteral("must be an array")});
        return;
    }
    const auto arr = v.toArray();
    for (int i = 0; i < arr.size(); ++i) {
        const QString p = QStringLiteral("attenuation[%1]").arg(i);
        if (!arr.at(i).isObject()) {
            errors.push_back({p, QStringLiteral("must be an object")});
            continue;
        }
        const auto o = arr.at(i).toObject();
        if (o.contains(QStringLiteral("type"))) {
            const auto type = templateItemType(o.value(QStringLiteral("type")));
            if (!type || *type != ProjectItem::Type::Attenuation) {
                errors.push_back({p + QStringLiteral(".type"),
                                  QStringLiteral("must be the Attenuation project item type")});
            }
        }
        const auto name = o.value("name").toString();
        if (!NameValidator::validateDisplayName(name)) {
            errors.push_back({p + QStringLiteral(".name"),
                              QStringLiteral("missing/invalid name")});
        }
        if (!o.value("max").isDouble()) {
            errors.push_back({p + QStringLiteral(".max"),
                              QStringLiteral("must be a number")});
            continue;
        }
        const double mx = o["max"].toDouble();
        if (mx <= 0.0) {
            errors.push_back({p + QStringLiteral(".max"),
                              QStringLiteral("must be > 0 (got %1)").arg(mx)});
        }
        if (!o.value("volume").isObject()) {
            errors.push_back({p + QStringLiteral(".volume"),
                              QStringLiteral("must be an object")});
            continue;
        }
        const auto volo = o["volume"].toObject();
        if (!volo.value("curve").isString()
            || volo.value("curve").toString().isEmpty()) {
            errors.push_back({p + QStringLiteral(".volume.curve"),
                              QStringLiteral("missing or empty curve type")});
        }
        validatePoints(volo.value("points"),
                       p + QStringLiteral(".volume.points"),
                       true,
                       0.0, mx, errors);
    }
}

void validateAuthoringArray(const QJsonObject& root,
                            const QString& key,
                            std::optional<ProjectItem::Type> expectedType,
                            std::vector<ValidationError>& errors) {
    if (!root.contains(key)) return;
    const QJsonValue value = root.value(key);
    if (!value.isArray()) {
        errors.push_back({key, QStringLiteral("must be an array")});
        return;
    }
    const QJsonArray array = value.toArray();
    for (int i = 0; i < array.size(); ++i) {
        const QString path = QStringLiteral("%1[%2]").arg(key).arg(i);
        if (!array.at(i).isObject()) {
            errors.push_back({path, QStringLiteral("must be an object")});
            continue;
        }
        const auto type = templateItemType(
            array.at(i).toObject().value(QStringLiteral("type")));
        if (!type) {
            errors.push_back({path + QStringLiteral(".type"),
                              QStringLiteral("must be a project item type name")});
            continue;
        }
        if (expectedType && *type != *expectedType) {
            errors.push_back({path + QStringLiteral(".type"),
                              QStringLiteral("unexpected project item type")});
        } else if (key == QStringLiteral("content")
                   && !isTemplateContentType(*type)) {
            errors.push_back({path + QStringLiteral(".type"),
                              QStringLiteral("must be a content project item type")});
        }
    }
}

}

ValidationResult ProjectTemplateLoader::validate(const QJsonDocument& doc,
                                                 const QString& sourceLabel) {
    ValidationResult result;
    if (!doc.isObject()) {
        result.summary  = QStringLiteral("Top-level JSON must be an object");
        result.errors.push_back({sourceLabel.isEmpty() ? QStringLiteral("<root>")
                                                       : sourceLabel,
                                 QStringLiteral("not a JSON object")});
        return result;
    }
    const auto root = doc.object();
    const auto authoring = AuthoringPolicy::validateTemplate(root);
    if (!authoring)
        result.errors.push_back({QStringLiteral("content"), authoring.message});

    const auto validatePlatformReferences = [&](auto&& self, const QJsonValue& value,
                                                const QString& path) -> void {
        if (value.isArray()) {
            qsizetype index = 0;
            for (const auto& child : value.toArray())
                self(self, child, path + QStringLiteral("[%1]").arg(index++));
            return;
        }
        if (!value.isObject()) return;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.key() == QStringLiteral("extensions")) continue;
            const auto childPath = path + QChar('.') + it.key();
            if (it.key() == QStringLiteral("platformOverrides")) {
                if (!it.value().isArray() || !it.value().toArray().isEmpty())
                    result.errors.push_back({childPath,
                        QStringLiteral("Project platform overrides are not supported in templates")});
            } else if (it.key() == QStringLiteral("fieldGroups")) {
                const auto platforms = it.value().toObject().value(QStringLiteral("platforms"));
                if (!platforms.isUndefined() && (!platforms.isObject() || !platforms.toObject().isEmpty()))
                    result.errors.push_back({childPath,
                        QStringLiteral("Project platform field groups are not supported in templates")});
            } else if (it.key() == QStringLiteral("sources")) {
                const auto sources = it->isObject()
                    ? SoundSourceSemantics::fromJson(it->toObject()) : std::nullopt;
                if (!sources) {
                    result.errors.push_back({childPath, QStringLiteral("Invalid Sound sources")});
                    continue;
                }
                for (const auto* set : SoundSourceSemantics::sourceSets(*sources))
                    if (!set->platforms.isEmpty())
                        result.errors.push_back({childPath,
                            QStringLiteral("Project platform source overrides are not supported in templates")});
            } else {
                self(self, it.value(), childPath);
            }
        }
    };
    validatePlatformReferences(validatePlatformReferences, root, QStringLiteral("template"));

    const QJsonValue versionValue = root.value("templateFormatVersion");
    if (!versionValue.isUndefined()
        && (!versionValue.isDouble() || !std::isfinite(versionValue.toDouble())
            || std::floor(versionValue.toDouble()) != versionValue.toDouble())) {
        result.errors.push_back({QStringLiteral("templateFormatVersion"),
                                 QStringLiteral("must be an integer")});
    } else {
        const int version = versionValue.isUndefined()
            ? ProjectTemplateData::kLegacyFormatVersion : versionValue.toInt();
        if (version < ProjectTemplateData::kLegacyFormatVersion
            || version > ProjectTemplateData::kCurrentFormatVersion) {
            result.errors.push_back({QStringLiteral("templateFormatVersion"),
                                     QStringLiteral("unsupported version %1 (supported %2-%3)")
                                         .arg(version)
                                         .arg(ProjectTemplateData::kLegacyFormatVersion)
                                         .arg(ProjectTemplateData::kCurrentFormatVersion)});
        }
    }

    if (!root.value("displayName").isString()
        || root.value("displayName").toString().isEmpty()) {
        result.errors.push_back({QStringLiteral("displayName"),
                                 QStringLiteral("missing or empty (string required)")});
    }
    if (!root.contains("description") || !root.value("description").isString()) {
        result.errors.push_back({QStringLiteral("description"),
                                 QStringLiteral("missing (string required, may be empty)")});
    }

    if (root.contains("attenuation")) {
        validateAttenuationArray(root["attenuation"], result.errors);
    }
    validateAuthoringArray(root, QStringLiteral("content"), std::nullopt,
                           result.errors);
    validateAuthoringArray(root, QStringLiteral("variables"),
                           ProjectItem::Type::Variable, result.errors);
    validateAuthoringArray(root, QStringLiteral("actions"),
                           ProjectItem::Type::Action, result.errors);
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
        if (!supportedSections.contains(section.key())) {
            result.errors.push_back({section.key(),
                                     QStringLiteral("unsupported template section")});
        }
    }

    if (!root.contains("bus")) {
        result.errors.push_back({QStringLiteral("bus"),
                                 QStringLiteral("missing (object required)")});
    } else if (!root.value("bus").isObject()) {
        result.errors.push_back({QStringLiteral("bus"),
                                 QStringLiteral("must be an object")});
    } else {
        const auto busObj = root["bus"].toObject();
        const QString rootName = busObj.value("name").toString();
        if (rootName != QStringLiteral("Master Bus")) {
            result.errors.push_back(
                {QStringLiteral("bus.name"),
                 QStringLiteral("must be 'Master Bus' (project default master bus)")});
        }
        validateBusNodeJson(root["bus"], QStringLiteral("bus"),
                            result.errors);
    }

    result.valid = result.errors.empty();
    if (result.valid) {
        result.summary = sourceLabel.isEmpty()
            ? QStringLiteral("Template is valid.")
            : QStringLiteral("Template '%1' is valid.").arg(sourceLabel);
    } else {
        result.summary = QStringLiteral("Found %1 issue(s)%2:")
            .arg(result.errors.size())
            .arg(sourceLabel.isEmpty() ? QString()
                                       : QStringLiteral(" in %1").arg(sourceLabel));
    }
    return result;
}


namespace {

QPointF jsonPair(const QJsonValue& v) {
    auto a = v.toArray();
    return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
}

TemplateBusNode parseBusNode(const QJsonObject& o) {
    TemplateBusNode n;
    n.name = o.value("name").toString();
    n.authoring = o;
    n.authoring.remove(QStringLiteral("children"));
    if (o.contains("children")) {
        for (const auto& c : o["children"].toArray()) {
            n.children.push_back(parseBusNode(c.toObject()));
        }
    }
    return n;
}

ProjectTemplateData parseTemplate(const QJsonDocument& doc, const QString& id) {
    ProjectTemplateData d;
    d.id = id;
    const auto root = doc.object();
    d.templateFormatVersion = root.value("templateFormatVersion").toInt(
        ProjectTemplateData::kLegacyFormatVersion);
    d.displayName = root.value("displayName").toString();
    d.description = root.value("description").toString();
    d.bus = parseBusNode(root.value("bus").toObject());
    d.content = root.value("content").toArray();
    d.variables = root.value("variables").toArray();
    d.actions = root.value("actions").toArray();

    for (const auto& v : root.value("attenuation").toArray()) {
        const auto o = v.toObject();
        TemplateAttenuationDef a;
        if (o.contains(QStringLiteral("type"))) a.authoring = o;
        a.name        = o.value("name").toString();
        a.maxDistance = o.value("max").toDouble();
        const auto vol = o.value("volume").toObject();
        a.volumeCurve  = vol.value("curve").toString();
        for (const auto& p : vol.value("points").toArray())
            a.volumePoints.append(jsonPair(p));
        d.attenuations.push_back(std::move(a));
    }

    return d;
}

}

std::optional<ProjectTemplateData> ProjectTemplateLoader::parse(
    const QJsonObject& object, ValidationResult* result, const QString& id)
{
    const QJsonDocument document(object);
    const ValidationResult validation = validate(document, id);
    if (result) *result = validation;
    if (!validation.valid) return std::nullopt;
    return parseTemplate(document, id);
}


namespace {

QJsonDocument readJsonFile(const QString& path, QString* parseErrOut) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (parseErrOut)
            *parseErrOut = QStringLiteral("cannot open: %1").arg(f.errorString());
        return {};
    }
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        if (parseErrOut)
            *parseErrOut = QStringLiteral("%1 at offset %2")
                               .arg(err.errorString()).arg(err.offset);
        return {};
    }
    return doc;
}

QString uniqueIdInDir(const QString& dir, const QString& baseStem) {
    QDir d(dir);
    if (!d.exists(baseStem + QStringLiteral(".json")))
        return baseStem;
    for (int i = 2; i < 1000; ++i) {
        const QString candidate = QStringLiteral("%1_%2").arg(baseStem).arg(i);
        if (!d.exists(candidate + QStringLiteral(".json")))
            return candidate;
    }
    return baseStem + QStringLiteral("_uniq");
}

void removeRetiredBuiltInTemplates(const QString& directory)
{
    const QString path =
        QDir(directory).filePath(QStringLiteral("mmorpg.json"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;

    const QByteArray contents = file.readAll();
    QByteArray gitObject = QByteArrayLiteral("blob ");
    gitObject.append(QByteArray::number(contents.size()));
    gitObject.append('\0');
    gitObject.append(contents);
    const QByteArray objectId =
        QCryptographicHash::hash(gitObject, QCryptographicHash::Sha1).toHex();
    static const QSet<QByteArray> retiredObjectIds{
        QByteArrayLiteral("3f6359f8acbd478069623184e145af45382f6aef"),
        QByteArrayLiteral("7aacbc6d4f59d84a0f53b38fbda461ad536b899c"),
        QByteArrayLiteral("7b62c47f12f45701ffe241234dddf7a104e520e2")};
    if (!retiredObjectIds.contains(objectId))
        return;

    file.close();
    if (!QFile::remove(path))
        qWarning() << "[ProjectTemplate] failed to remove retired template"
                   << path;
}

}


QString ProjectTemplateLoader::runtimeDir() {
    const auto root = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    return QDir(root).filePath(QStringLiteral("Templates"));
}

void ProjectTemplateLoader::ensureUserTemplatesSeeded() {
    const QString target = runtimeDir();
    removeRetiredBuiltInTemplates(target);
    if (QFileInfo::exists(target)) {
        return;
    }
    if (!QDir().mkpath(target)) {
        qWarning() << "[ProjectTemplate] failed to mkpath" << target;
        return;
    }

    const QString srcDir = QCoreApplication::applicationDirPath();
    QDir s(srcDir);
    const auto entries = s.entryInfoList(
        QStringList() << QStringLiteral("*.json"),
        QDir::Files | QDir::Readable);

    int seeded = 0;
    for (const auto& fi : entries) {
        QString parseErr;
        const auto doc = readJsonFile(fi.absoluteFilePath(), &parseErr);
        if (doc.isNull() || doc.isEmpty()) {
            qInfo() << "[ProjectTemplate] seed skip (parse)" << fi.fileName()
                    << parseErr;
            continue;
        }
        const auto vr = validate(doc, fi.fileName());
        if (!vr.valid) {
            qInfo() << "[ProjectTemplate] seed skip (validate)" << fi.fileName()
                    << vr.summary;
            continue;
        }
        const QString dst = QDir(target).filePath(fi.fileName());
        if (!QFile::copy(fi.absoluteFilePath(), dst)) {
            qWarning() << "[ProjectTemplate] seed copy failed"
                       << fi.absoluteFilePath() << "→" << dst;
            continue;
        }
        ++seeded;
    }
    qInfo() << "[ProjectTemplate] seed complete:" << seeded << "file(s) into"
            << target;
}

std::vector<TemplateLocation> ProjectTemplateLoader::listAvailable() {
    std::vector<TemplateLocation> out;
    const QString dir = runtimeDir();
    QDir d(dir);
    if (!d.exists()) return out;
    const auto entries = d.entryInfoList(
        QStringList() << QStringLiteral("*.json"),
        QDir::Files | QDir::Readable);
    for (const auto& fi : entries) {
        QString parseErr;
        const auto doc = readJsonFile(fi.absoluteFilePath(), &parseErr);
        if (doc.isNull() || !doc.isObject()) {
            qWarning() << "[ProjectTemplate] listAvailable skip"
                       << fi.fileName() << parseErr;
            continue;
        }
        const auto root = doc.object();
        const auto display = root.value("displayName").toString();
        if (display.isEmpty()) {
            qWarning() << "[ProjectTemplate] listAvailable skip (no displayName)"
                       << fi.fileName();
            continue;
        }
        TemplateLocation loc;
        loc.id          = fi.completeBaseName();
        loc.displayName = display;
        loc.description = root.value("description").toString();
        loc.filePath    = fi.absoluteFilePath();
        out.push_back(std::move(loc));
    }
    return out;
}

std::optional<ProjectTemplateData> ProjectTemplateLoader::load(const QString& id) {
    const QString path = QDir(runtimeDir()).filePath(id + QStringLiteral(".json"));
    if (!QFileInfo::exists(path)) {
        qWarning() << "[ProjectTemplate] load: file missing" << path;
        return std::nullopt;
    }
    QString parseErr;
    const auto doc = readJsonFile(path, &parseErr);
    if (doc.isNull()) {
        qWarning() << "[ProjectTemplate] load: parse failed" << path << parseErr;
        return std::nullopt;
    }
    const auto vr = validate(doc, id);
    if (!vr.valid) {
        qWarning() << "[ProjectTemplate] load: validate failed" << id
                   << vr.toUserText();
        return std::nullopt;
    }
    return parseTemplate(doc, id);
}

std::optional<ProjectTemplateData> ProjectTemplateLoader::loadFile(
    const QString& path, ValidationResult* result) {
    QString parseErr;
    const auto doc = readJsonFile(path, &parseErr);
    if (doc.isNull()) {
        if (result) {
            result->valid = false;
            result->summary = QStringLiteral("Template could not be read.");
            result->errors = {{path, parseErr}};
        }
        return std::nullopt;
    }
    const ValidationResult validation = validate(doc, path);
    if (result)
        *result = validation;
    if (!validation.valid)
        return std::nullopt;
    return parseTemplate(doc, QFileInfo(path).completeBaseName());
}

std::optional<QString> ProjectTemplateLoader::importFromFile(
    const QString& srcPath, ValidationResult& outResult) {

    QString parseErr;
    const auto doc = readJsonFile(srcPath, &parseErr);
    if (doc.isNull()) {
        outResult.valid   = false;
        outResult.summary = QStringLiteral("JSON parse error in '%1'").arg(srcPath);
        outResult.errors.clear();
        outResult.errors.push_back({QFileInfo(srcPath).fileName(), parseErr});
        return std::nullopt;
    }
    outResult = validate(doc, QFileInfo(srcPath).fileName());
    if (!outResult.valid)
        return std::nullopt;

    const QString destDir = runtimeDir();
    if (!QDir().mkpath(destDir)) {
        outResult.valid   = false;
        outResult.summary = QStringLiteral("Cannot create runtime templates dir");
        outResult.errors.clear();
        outResult.errors.push_back({destDir,
                                    QStringLiteral("mkpath failed")});
        return std::nullopt;
    }
    const QString baseStem = QFileInfo(srcPath).completeBaseName();
    const QString id       = uniqueIdInDir(destDir, baseStem);
    const QString destPath = QDir(destDir).filePath(id + QStringLiteral(".json"));
    if (!QFile::copy(srcPath, destPath)) {
        outResult.valid   = false;
        outResult.summary = QStringLiteral("Failed to copy template into '%1'")
                                .arg(destDir);
        outResult.errors.clear();
        outResult.errors.push_back({destPath,
                                    QStringLiteral("QFile::copy failed")});
        return std::nullopt;
    }
    return id;
}

}
