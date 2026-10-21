#include "Core/Components/VSelection.h"
#include "ProjectPlatform.h"
#include "FieldGroupProjectAdapter.h"

#include "ProjectTypes.h"
#include "Components/VTrack.h"
#include "SoundSourceSemantics.h"

#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace VNX {

ProjectPlatformProfile ProjectPlatformProfile::fromBasePlatform(BasePlatform platform)
{
    PlatformProfile profile;
    switch (platform) {
    case BasePlatform::Windows: profile = PlatformProfileManager::createWindowsDefault(); break;
    case BasePlatform::macOS: profile = PlatformProfileManager::createMacOSDefault(); break;
    case BasePlatform::Linux: profile = PlatformProfileManager::createLinuxDefault(); break;
    case BasePlatform::Android: profile = PlatformProfileManager::createAndroidDefault(); break;
    case BasePlatform::iOS: profile = PlatformProfileManager::createiOSDefault(); break;
    }
    return {profile.performResample, profile.sizePolicies};
}

QJsonObject ProjectPlatformProfile::toJson() const
{
    QJsonArray policies;
    for (const auto& policy : sizePolicies)
        policies.append(policy.toJson());
    return {{QStringLiteral("performResample"), performResample},
            {QStringLiteral("sizePolicies"), policies}};
}

QJsonObject ProjectPlatform::toJson() const
{
    QJsonObject json{{QStringLiteral("id"), ProjectPlatformSemantics::idString(id)},
                     {QStringLiteral("name"), name},
                     {QStringLiteral("basePlatform"),
                      ProjectPlatformSemantics::basePlatformName(basePlatform)}};
    json.insert(QStringLiteral("profile"), profile.toJson());
    return json;
}

namespace ProjectPlatformSemantics {
namespace {

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool readConfig(const QJsonValue& value, ConversionParameters* config)
{
    if (!value.isObject()) return false;
    const QJsonObject json = value.toObject();
    const QJsonObject defaults = ConversionParameters{}.toJson();
    if (json.keys() != defaults.keys()) return false;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (json.value(it.key()).type() != it.value().type()) return false;
    }
    for (const auto& key : {"format", "vorbisEncodeMode", "targetSampleRate",
                            "vorbisAverageBitrate", "vorbisMinBitrate", "vorbisMaxBitrate"}) {
        const double number = json.value(QLatin1String(key)).toDouble();
        if (number < 0 || number > UINT32_MAX || std::trunc(number) != number)
            return false;
    }
    if (json.value(QStringLiteral("format")).toInteger() > 5
        || json.value(QStringLiteral("vorbisEncodeMode")).toInteger() > 1
        || json.value(QStringLiteral("targetSampleRate")).toInteger() == 0
        || json.value(QStringLiteral("vorbisQuality")).toDouble() < -0.1
        || json.value(QStringLiteral("vorbisQuality")).toDouble() > 1.0)
        return false;
    *config = ConversionParameters::fromJson(json);
    return true;
}

bool readProfile(const QJsonValue& value, ProjectPlatformProfile* profile)
{
    if (!value.isObject()) return false;
    const QJsonObject json = value.toObject();
    ProjectPlatformProfile parsed;
    parsed.sizePolicies.clear();
    if (json.keys() != QStringList{QStringLiteral("performResample"), QStringLiteral("sizePolicies")}
        || !json.value(QStringLiteral("performResample")).isBool()
        || !json.value(QStringLiteral("sizePolicies")).isArray())
        return false;
    parsed.performResample = json.value(QStringLiteral("performResample")).toBool();
    const auto readBound = [](const QJsonValue& value) {
        return value.isDouble() && value.toInteger(-1) >= 0
            && static_cast<double>(value.toInteger()) == value.toDouble();
    };
    for (const auto& value : json.value(QStringLiteral("sizePolicies")).toArray()) {
        if (!value.isObject()) return false;
        const auto row = value.toObject();
        const auto maximum = row.value(QStringLiteral("maxBytes"));
        SizePolicy policy;
        if (row.keys() != QStringList{QStringLiteral("config"), QStringLiteral("maxBytes")}
            || (!maximum.isNull() && !readBound(maximum))
            || !readConfig(row.value(QStringLiteral("config")), &policy.config))
            return false;
        if (!maximum.isNull()) policy.maxBytes = static_cast<uint64_t>(maximum.toInteger());
        parsed.sizePolicies.push_back(std::move(policy));
    }
    if (!validateSizePolicies(parsed.sizePolicies)) return false;
    *profile = std::move(parsed);
    return true;
}

class ReferenceValidator
{
public:
    explicit ReferenceValidator(const QVector<ProjectPlatform>& platforms)
    {
        for (const auto& platform : platforms) ids_.insert(platform.id);
    }

    void reference(const QJsonValue& value, const QString& path, bool shared = false)
    {
        if (shared && value.isNull()) return;
        const auto id = parseId(value.toString());
        if (!value.isString() || !id || !ids_.contains(*id))
            errors.append(QStringLiteral("Unknown or invalid platform ID at %1").arg(path));
    }

    void references(const QJsonValue& value, const QString& path)
    {
        if (!value.isArray()) {
            errors.append(QStringLiteral("Platform IDs must be an array at %1").arg(path));
            return;
        }
        QSet<ProjectPlatformId> seen;
        qsizetype index = 0;
        for (const auto& entry : value.toArray()) {
            reference(entry, path + QStringLiteral("[%1]").arg(index++));
            const auto id = parseId(entry.toString());
            if (id && seen.contains(*id))
                errors.append(QStringLiteral("Duplicate platform ID at %1").arg(path));
            if (id) seen.insert(*id);
        }
    }

    void object(const QJsonValue& value, const QString& path)
    {
        if (value.isArray()) {
            qsizetype index = 0;
            for (const auto& child : value.toArray())
                object(child, path + QStringLiteral("[%1]").arg(index++));
            return;
        }
        if (!value.isObject()) return;
        const auto json = value.toObject();
        for (auto it = json.begin(); it != json.end(); ++it) {
            const auto childPath = path + QChar('/') + it.key();
            if (it.key() == QStringLiteral("extensions")) continue;
            if (it.key() == QStringLiteral("platformOverrides")) {
                if (!it.value().isArray()) {
                    errors.append(QStringLiteral("Platform scopes must be an array at %1").arg(childPath));
                    continue;
                }
                QSet<QString> keys;
                qsizetype index = 0;
                for (const auto& value : it.value().toArray()) {
                    const auto row = value.toObject();
                    const auto rowPath = childPath + QStringLiteral("[%1]").arg(index++);
                    if (row.contains(QStringLiteral("platform")))
                        errors.append(QStringLiteral("Legacy platform format is unsupported at %1").arg(rowPath));
                    const auto platform = row.value(QStringLiteral("platformId"));
                    reference(platform, rowPath);
                    const auto key = platform.toString();
                    if (keys.contains(key))
                        errors.append(QStringLiteral("Duplicate platform scope at %1").arg(rowPath));
                    keys.insert(key);
                }
            } else if (it.key() == QStringLiteral("sources")) {
                QString error;
                const auto sources = it->isObject()
                    ? SoundSourceSemantics::fromJson(it->toObject(), &error) : std::nullopt;
                if (!sources) {
                    errors.append(QStringLiteral("Invalid Sound sources at %1: %2").arg(childPath, error));
                    continue;
                }
                for (const auto* set : SoundSourceSemantics::sourceSets(*sources))
                    for (auto p = set->platforms.cbegin(); p != set->platforms.cend(); ++p)
                        reference(p.key(), childPath + QChar('/') + p.key());
            } else if (it.key() == QStringLiteral("selections")) {
                QString error;
                const auto selections = SelectionData::fromJson(it.value().toObject(), &error);
                if (!selections) {
                    errors.append(error + QStringLiteral(" at ") + childPath);
                    continue;
                }
                for (auto p = selections->platforms.cbegin(); p != selections->platforms.cend(); ++p)
                    reference(p.key(), childPath + QChar('/') + p.key());
            } else if (it.key() == QStringLiteral("fieldGroups")) {
                const auto groups = it.value().toObject();
                const auto platforms = groups.value(QStringLiteral("platforms"));
                if (!platforms.isUndefined() && !platforms.isObject())
                    errors.append(QStringLiteral("Field group platforms must be an object at %1").arg(childPath));
                const auto rows = platforms.toObject();
                for (auto row = rows.begin(); row != rows.end(); ++row)
                    reference(row.key(), childPath + QChar('/') + row.key());
            } else {
                object(it.value(), childPath);
            }
        }
    }

    QStringList errors;

private:
    QSet<ProjectPlatformId> ids_;
};

}

QString basePlatformName(BasePlatform platform)
{
    switch (platform) {
    case BasePlatform::Windows: return QStringLiteral("Windows");
    case BasePlatform::macOS: return QStringLiteral("macOS");
    case BasePlatform::Linux: return QStringLiteral("Linux");
    case BasePlatform::Android: return QStringLiteral("Android");
    case BasePlatform::iOS: return QStringLiteral("iOS");
    }
    return {};
}

std::optional<BasePlatform> parseBasePlatform(const QString& name)
{
    for (const auto platform : {BasePlatform::Windows, BasePlatform::macOS,
                                BasePlatform::Linux, BasePlatform::Android, BasePlatform::iOS}) {
        if (name == basePlatformName(platform)) return platform;
    }
    return std::nullopt;
}

std::optional<ProjectPlatformId> parseId(const QString& text)
{
    const auto id = QUuid::fromString(text);
    if (id.isNull() || idString(id) != text) return std::nullopt;
    return id;
}

QString idString(const ProjectPlatformId& id)
{
    return id.isNull() ? QString() : id.toString(QUuid::WithoutBraces);
}

QStringList ids(const QVector<ProjectPlatform>& platforms)
{
    QStringList result;
    for (const auto& platform : platforms) result.append(idString(platform.id));
    return result;
}

const ProjectPlatform* find(const QVector<ProjectPlatform>& platforms, const ProjectPlatformId& id)
{
    for (const auto& platform : platforms) {
        if (platform.id == id) return &platform;
    }
    return nullptr;
}

Platform mediaPlatform(BasePlatform platform)
{
    switch (platform) {
    case BasePlatform::Windows: return Platform::Windows;
    case BasePlatform::macOS: return Platform::macOS;
    case BasePlatform::Linux: return Platform::Linux;
    case BasePlatform::Android: return Platform::Android;
    case BasePlatform::iOS: return Platform::iOS;
    }
    qFatal("Invalid base platform");
}

QVector<ProjectPlatform> defaultPlatforms()
{
    return {{QUuid::createUuid(), QStringLiteral("Windows"), BasePlatform::Windows},
            {QUuid::createUuid(), QStringLiteral("Android"), BasePlatform::Android},
            {QUuid::createUuid(), QStringLiteral("macOS"), BasePlatform::macOS},
            {QUuid::createUuid(), QStringLiteral("iOS"), BasePlatform::iOS}};
}

bool validateName(const QString& name, QString* error)
{
    static const QRegularExpression forbidden(QStringLiteral("[\\x{0000}-\\x{001f}\\x{007f}-\\x{009f}<>:\"/\\\\|?*]"));
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|COM[1-9¹²³]|LPT[1-9¹²³])(?:\\.|$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (name.isEmpty() || name.toUtf8().size() > 120 || name != name.trimmed()
        || QString::fromUtf8(name.toUtf8()) != name
        || name.endsWith(QChar('.')) || forbidden.match(name).hasMatch()
        || reserved.match(name).hasMatch())
        return fail(error, QStringLiteral("Platform name must be a valid directory name of at most 120 UTF-8 bytes"));
    return true;
}

bool validatePlatforms(const QVector<ProjectPlatform>& platforms, QString* error)
{
    if (platforms.isEmpty()) return fail(error, QStringLiteral("Project must contain at least one platform instance"));
    QSet<ProjectPlatformId> seenIds;
    QSet<QString> names;
    for (const auto& platform : platforms) {
        if (platform.id.isNull() || seenIds.contains(platform.id))
            return fail(error, QStringLiteral("Platform IDs must be non-null and unique"));
        seenIds.insert(platform.id);
        if (!validateName(platform.name, error)) return false;
        const auto name = platform.name.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (names.contains(name)) return fail(error, QStringLiteral("Platform directory names must be unique"));
        names.insert(name);
        if (!parseBasePlatform(basePlatformName(platform.basePlatform)))
            return fail(error, QStringLiteral("Invalid base platform"));
        if (!validateSizePolicies(platform.profile.sizePolicies, error)) return false;
        ProjectPlatformProfile profile;
        if (!readProfile(platform.profile.toJson(), &profile))
            return fail(error, QStringLiteral("Invalid platform conversion profile"));
    }
    return true;
}

bool platformsFromJson(const QJsonValue& value, QVector<ProjectPlatform>* platforms, QString* error)
{
    if (!value.isArray())
        return fail(error, QStringLiteral("Legacy platform format is unsupported; metadata.platforms must contain platform instances. No migration is available."));
    QVector<ProjectPlatform> parsed;
    for (const auto& value : value.toArray()) {
        const auto json = value.toObject();
        const auto id = parseId(json.value(QStringLiteral("id")).toString());
        const auto base = parseBasePlatform(json.value(QStringLiteral("basePlatform")).toString());
        if (!value.isObject() || !id || !base || !json.value(QStringLiteral("name")).isString())
            return fail(error, QStringLiteral("Invalid project platform definition"));
        for (const auto& key : json.keys()) {
            if (!QStringList{"id", "name", "basePlatform", "profile"}.contains(key))
                return fail(error, QStringLiteral("Unknown project platform field: %1").arg(key));
        }
        ProjectPlatform platform{*id, json.value(QStringLiteral("name")).toString(), *base};
        if (!readProfile(json.value(QStringLiteral("profile")), &platform.profile))
            return fail(error, QStringLiteral("A platform resampling flag and strictly increasing size limits ending with Infinite are required; legacy profiles are unsupported."));
        parsed.append(std::move(platform));
    }
    if (!validatePlatforms(parsed, error)) return false;
    *platforms = std::move(parsed);
    return true;
}

bool validateIds(const QStringList& ids, const QVector<ProjectPlatform>& platforms, QString* error)
{
    ReferenceValidator validator(platforms);
    validator.references(QJsonArray::fromStringList(ids), QStringLiteral("platform_ids"));
    return validator.errors.isEmpty() || fail(error, validator.errors.first());
}

QJsonArray platformsToJson(const QVector<ProjectPlatform>& platforms)
{
    QJsonArray result;
    for (const auto& platform : platforms) result.append(platform.toJson());
    return result;
}

QStringList validateDocuments(const QJsonObject& rootContent, const QMap<QString, QJsonObject>& documents)
{
    QVector<ProjectPlatform> platforms;
    QString error;
    const auto metadata = rootContent.value(QStringLiteral("metadata")).toObject();
    if (metadata.contains(QStringLiteral("targetPlatforms")))
        return {QStringLiteral("Legacy targetPlatforms is unsupported; no migration is available")};
    if (!platformsFromJson(metadata.value(QStringLiteral("platforms")), &platforms, &error))
        return {error};
    ReferenceValidator validator(platforms);
    for (const auto& key : {"packExportCurrentPlatform", "packExportSelectedPlatforms", "packExportPlatformOutputRoots"}) {
        if (rootContent.contains(QLatin1String(key)))
            validator.errors.append(QStringLiteral("Legacy platform field is unsupported: %1").arg(QLatin1String(key)));
    }
    for (const auto& key : {"deploymentProfiles"}) {
        qsizetype index = 0;
        for (const auto& value : rootContent.value(QLatin1String(key)).toArray()) {
            const auto row = value.toObject();
            const auto path = QStringLiteral("%1[%2]").arg(QLatin1String(key)).arg(index++);
            if (row.contains(QStringLiteral("platforms")))
                validator.errors.append(QStringLiteral("Legacy platform field is unsupported at %1").arg(path));
            validator.references(row.value(QStringLiteral("platformIds")), path);
        }
    }
    for (auto it = documents.begin(); it != documents.end(); ++it)
        validator.object(it.value(), it.key());
    return validator.errors;
}

QVector<ProjectItem*> referenceOwners(const ProjectData& project)
{
    QVector<ProjectItem*> owners;
    QSet<ProjectItem*> visited;
    const auto visit = [&](ProjectItem* item, const auto& self) -> void {
        if (!item || visited.contains(item)) return;
        visited.insert(item);
        owners.append(item);
        for (const auto& child : item->children) self(child.get(), self);
    };
    visit(project.root.get(), visit);
    for (const auto* binding : project.allFileBindings()) visit(binding->rootItem.get(), visit);
    return owners;
}

bool validateProject(const ProjectData& project, QString* error)
{
    if (!validatePlatforms(project.platforms, error)) return false;
    QJsonObject root{{QStringLiteral("metadata"), QJsonObject{{QStringLiteral("platforms"), platformsToJson(project.platforms)}}}};
    QJsonArray deployments;
    for (const auto& row : project.deploymentProfiles)
        deployments.append(QJsonObject{{QStringLiteral("platformIds"), QJsonArray::fromStringList(row.platformIds)}});
    root.insert(QStringLiteral("deploymentProfiles"), deployments);
    QMap<QString, QJsonObject> documents;
    for (const auto* item : referenceOwners(project)) {
        QJsonObject json{{QStringLiteral("platformOverrides"), PlayablePropertyAuthoringV1::platformOverridesToJson(item->platformOverrides)}};
        QJsonObject fieldGroupPlatforms;
        const auto addGroups = [&](const auto& group) {
            for (const auto& [platform, state] : group.platforms)
                fieldGroupPlatforms.insert(idString(fromFieldGroupGuid(platform)), QJsonObject{});
        };
        addGroups(item->fieldGroups.mainOutput);
        addGroups(item->fieldGroups.send);
        addGroups(item->fieldGroups.spatialRating);
        json.insert(QStringLiteral("fieldGroups"), QJsonObject{{QStringLiteral("platforms"), fieldGroupPlatforms}});
        QJsonArray occurrences;
        if (const auto* selection = item->comp<VSelection>())
            json.insert(QStringLiteral("selections"), selection->selections.toJson());
        if (const auto* timeline = item->comp<VTrack>()) {
            for (const auto& clip : timeline->clips.items)
                occurrences.append(QJsonObject{{QStringLiteral("platformOverrides"), PlayablePropertyAuthoringV1::platformOverridesToJson(clip.platformOverrides)}});
        }
        json.insert(QStringLiteral("occurrences"), occurrences);
        if (const auto* sound = item->comp<VSound>()) {
            json.insert(QStringLiteral("sound"), sound->toJson());
        }
        documents.insert(item->guid.toString() + QChar('/') + item->name, json);
    }
    if (error) error->clear();
    const auto errors = validateDocuments(root, documents);
    return errors.isEmpty() || fail(error, errors.first());
}

}
}
