#include "ConversionMigration.h"

#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VConversion.h"
#include "NameValidator.h"
#include "ProjectDocument.h"
#include "ProjectDataPolicy.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <iterator>

namespace VNX {
namespace {

quint32 closestRate(quint32 rate)
{
    if (rate == 0) return 0;
    quint32 selected = kConversionSampleRates[1];
    for (const auto allowed : kConversionSampleRates)
        if (allowed != 0 && allowed <= rate) selected = allowed;
    return selected;
}

QJsonObject conversionGroup(const QString& id)
{
    if (id.isEmpty()) return {{QStringLiteral("inherit"), true}};
    return {{QStringLiteral("inherit"), false},
            {QStringLiteral("values"), QJsonObject{{QStringLiteral("settingsRef"), id}}}};
}

QJsonArray singleTier(quint32 rate)
{
    return {QJsonObject{{QStringLiteral("maxBytes"), QJsonValue::Null},
                        {QStringLiteral("sampleRate"), static_cast<qint64>(rate)}}};
}

quint32 tierRate(const QJsonArray& tiers, quint64 bytes, bool legacy)
{
    for (const auto& value : tiers) {
        const auto tier = value.toObject();
        const auto limit = tier.value(QStringLiteral("maxBytes"));
        if (!limit.isNull() && bytes > static_cast<quint64>(limit.toInteger())) continue;
        return legacy ? static_cast<quint32>(tier.value(QStringLiteral("config")).toObject()
                            .value(QStringLiteral("targetSampleRate")).toInteger(48000))
                      : static_cast<quint32>(tier.value(QStringLiteral("sampleRate")).toInteger());
    }
    return 0;
}

struct Source
{
    QString entryId;
    QString sourceId;
    QString language;
    QJsonObject set;
    QJsonObject asset;
    quint32 sampleRate = 0;
    quint64 fileSize = 0;
};

quint32 oldRate(const Source& source, const QJsonObject& platform,
                const QJsonObject& overrides)
{
    const auto profile = platform.value(QStringLiteral("profile")).toObject();
    if (!profile.value(QStringLiteral("performResample")).toBool()) return 0;
    auto settings = overrides.value(source.sourceId).toObject();
    const auto assetSettings = source.asset.value(QStringLiteral("settings")).toObject();
    if (!overrides.contains(source.sourceId)
        && assetSettings.value(QStringLiteral("performResample")).toBool())
        settings = assetSettings;
    quint32 rate = 0;
    if (overrides.contains(source.sourceId)
        || assetSettings.value(QStringLiteral("performResample")).toBool()) {
        if (settings.value(QStringLiteral("performResample")).toBool())
            rate = static_cast<quint32>(settings.value(QStringLiteral("targetSampleRate")).toInteger(48000));
    } else {
        rate = tierRate(profile.value(QStringLiteral("sizePolicies")).toArray(), source.fileSize, true);
    }
    return rate == source.sampleRate ? 0 : rate;
}

quint32 newRate(const Source& source, const QJsonObject& config)
{
    const quint32 rate = tierRate(config.value(QStringLiteral("sampleRateTiers")).toArray(),
                                  source.fileSize, false);
    return source.sampleRate != 0 && rate >= source.sampleRate ? 0 : rate;
}

QJsonObject resultJson(const Source& source, quint32 rate)
{
    return {{QStringLiteral("resample"), rate != 0},
            {QStringLiteral("sampleRate"), static_cast<qint64>(rate != 0 ? rate : source.sampleRate)}};
}

}

bool prepareConversionMigration(const QString& sourceDirectory,
                                const QString& projectFile,
                                QMap<QString, QJsonObject>& documents,
                                QHash<QString, QByteArray>& inputFingerprints,
                                QStringList* warnings,
                                QString* error)
{
    auto rootDocument = documents.value(projectFile);
    auto root = rootDocument.value(QStringLiteral("content")).toObject();
    const auto projectId = rootDocument.value(QStringLiteral("harkenDocument")).toObject()
        .value(QStringLiteral("projectId")).toString();
    const auto platforms = root.value(QStringLiteral("deployPlatforms")).toArray();
    const QString registryPath = QDir(sourceDirectory).relativeFilePath(
        QDir(PathUtils::getAssetRegistryCacheDir(sourceDirectory)).filePath(QStringLiteral("assets.json")));
    QJsonObject registry;
    QFile registryFile(QDir(sourceDirectory).filePath(registryPath));
    if (registryFile.exists()) {
        if (!registryFile.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Cannot read the legacy asset registry");
            return false;
        }
        QJsonParseError parseError;
        const auto json = QJsonDocument::fromJson(registryFile.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
            if (error) *error = QStringLiteral("The legacy asset registry is invalid");
            return false;
        }
        registry = json.object();
    }
    const auto assets = registry.value(QStringLiteral("assets")).toObject();
    const auto overrides = registry.value(QStringLiteral("fileOverrides")).toObject();
    QJsonObject sourcePaths = root.value(QStringLiteral("sourceAssets")).toObject();
    QJsonArray defaults;
    for (const auto& value : platforms) {
        const auto platform = value.toObject();
        const auto profile = platform.value(QStringLiteral("profile")).toObject();
        QJsonArray tiers;
        if (profile.value(QStringLiteral("performResample")).toBool()) {
            for (const auto& policyValue : profile.value(QStringLiteral("sizePolicies")).toArray()) {
                const auto policy = policyValue.toObject();
                const auto rate = static_cast<quint32>(policy.value(QStringLiteral("config")).toObject()
                    .value(QStringLiteral("targetSampleRate")).toInteger(48000));
                tiers.append(QJsonObject{{QStringLiteral("maxBytes"), policy.value(QStringLiteral("maxBytes"))},
                    {QStringLiteral("sampleRate"), static_cast<qint64>(closestRate(rate))}});
            }
        } else {
            tiers = singleTier(0);
        }
        defaults.append(QJsonObject{{QStringLiteral("platformId"), platform.value(QStringLiteral("id"))},
            {QStringLiteral("format"), QStringLiteral("PCM")},
            {QStringLiteral("vorbisQuality"), static_cast<double>(0.4f)},
            {QStringLiteral("sampleRateTiers"), tiers}});
    }

    QSet<QString> names;
    for (auto document = documents.cbegin(); document != documents.cend(); ++document)
        if (document.key().startsWith(QStringLiteral("Conversions/")))
            names.insert(document.value().value(QStringLiteral("content")).toObject()
                             .value(QStringLiteral("name")).toString().toCaseFolded());
    QMap<QByteArray, QString> presets;
    const auto addPreset = [&](const QString& base, const QJsonArray& configs) {
        const auto key = QJsonDocument(configs).toJson(QJsonDocument::Compact);
        const auto found = presets.constFind(key);
        if (found != presets.cend()) return found.value();
        const QString name = NameValidator::makeUniqueName(base, [&](const QString& candidate) {
            return !names.contains(candidate.toCaseFolded());
        });
        names.insert(name.toCaseFolded());
        const QUuid guid = QUuid::createUuid();
        const QString id = guid.toString(QUuid::WithoutBraces);
        const QString path = QStringLiteral("Conversions/") + NameValidator::objectFileName(name, guid);
        const QJsonObject content{{QStringLiteral("type"), QStringLiteral("Conversion")},
            {QStringLiteral("guid"), id}, {QStringLiteral("name"), name},
            {QStringLiteral("parentRef"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("root")}}},
            {QStringLiteral("components"), QJsonObject{{QStringLiteral("conversion"),
                QJsonObject{{QStringLiteral("platforms"), configs}}}}}};
        documents.insert(path, ProjectDocument::create(QStringLiteral("project.conversion"), id, projectId, content));
        presets.insert(key, id);
        return id;
    };
    const QString defaultId = addPreset(QStringLiteral("Default"), defaults);
    QJsonArray differences;
    QMap<QString, QJsonObject> metadataBySource;
    AssetImporter importer;
    bool metadataValid = true;
    std::function<void(QJsonObject&, bool)> migrateNode;
    migrateNode = [&](QJsonObject& node, bool rootNode) {
        QString presetId;
        const auto sound = node.value(QStringLiteral("components")).toObject().value(QStringLiteral("sound")).toObject();
        if (node.value(QStringLiteral("type")) == QStringLiteral("Sound")) {
            QVector<Source> sources;
            const auto sourceSets = sound.value(QStringLiteral("sources")).toObject();
            const auto appendSet = [&](const QJsonObject& set, const QString& language) {
                for (const auto& value : set.value(QStringLiteral("entries")).toArray()) {
                    const auto entry = value.toObject();
                    Source source;
                    source.entryId = entry.value(QStringLiteral("entryId")).toString();
                    source.sourceId = QString::number(entry.value(QStringLiteral("sourceId")).toInteger());
                    source.language = language;
                    source.set = set;
                    source.asset = assets.value(source.sourceId).toObject();
                    auto metadata = metadataBySource.value(source.sourceId,
                        source.asset.value(QStringLiteral("metadata")).toObject());
                    if (!metadataBySource.contains(source.sourceId)) {
                        const auto path = sourcePaths.value(source.sourceId).toString(
                            source.asset.value(QStringLiteral("relativePath")).toString());
                        if (!path.isEmpty() && !ProjectDataPolicy::isSafeRelativePath(path)) {
                            metadataValid = false;
                            if (error) *error = QStringLiteral("Invalid source path during conversion migration: %1").arg(path);
                            return;
                        }
                        const auto absolute = QDir(PathUtils::getAudioSourcesDir(sourceDirectory)).filePath(path);
                        const QString relative = QDir::fromNativeSeparators(QDir(sourceDirectory).relativeFilePath(absolute));
                        if (!path.isEmpty() && QFileInfo(absolute).isFile()) {
                            QFile sourceFile(absolute);
                            QCryptographicHash fingerprint(QCryptographicHash::Sha256);
                            if (!sourceFile.open(QIODevice::ReadOnly) || !fingerprint.addData(&sourceFile)) {
                                metadataValid = false;
                                if (error) *error = QStringLiteral("Cannot read source during conversion migration: %1").arg(path);
                                return;
                            }
                            inputFingerprints.insert(relative, fingerprint.result());
                            metadata = importer.readMetadata(absolute).toJson();
                            if (metadata.value(QStringLiteral("sampleRate")).toInteger() == 0) {
                                metadataValid = false;
                                if (error) *error = QStringLiteral("Cannot read source metadata during conversion migration: %1").arg(path);
                                return;
                            }
                        } else if (!path.isEmpty()) {
                            inputFingerprints.insert(relative, {});
                        }
                        const bool requiresMetadata = std::any_of(platforms.begin(), platforms.end(), [](const QJsonValue& platform) {
                            return platform.toObject().value(QStringLiteral("profile")).toObject()
                                .value(QStringLiteral("performResample")).toBool();
                        });
                        if (requiresMetadata && (metadata.value(QStringLiteral("sampleRate")).toInteger() <= 0
                            || metadata.value(QStringLiteral("fileSize")).toInteger() <= 0)) {
                            metadataValid = false;
                            if (error) *error = QStringLiteral("Cannot resolve legacy conversion for source %1 without readable audio or cached metadata").arg(source.sourceId);
                            return;
                        }
                    }
                    metadataBySource.insert(source.sourceId, metadata);
                    source.sampleRate = static_cast<quint32>(metadata.value(QStringLiteral("sampleRate")).toInteger());
                    source.fileSize = static_cast<quint64>(metadata.value(QStringLiteral("fileSize")).toInteger());
                    sources.append(source);
                }
            };
            appendSet(sourceSets.value(QStringLiteral("plain")).toObject(), {});
            const auto localized = sourceSets.value(QStringLiteral("localized")).toObject();
            for (auto language = localized.begin(); language != localized.end(); ++language)
                appendSet(language.value().toObject(), language.key());
            if (!metadataValid) return;
            QJsonArray configs = defaults;
            bool changed = false;
            for (qsizetype index = 0; index < platforms.size() && !sources.isEmpty(); ++index) {
                const auto platform = platforms[index].toObject();
                const QString platformId = platform.value(QStringLiteral("id")).toString();
                auto config = configs[index].toObject();
                bool matches = true;
                for (const auto& source : sources)
                    matches = matches && oldRate(source, platform, overrides) == newRate(source, config);
                if (matches) continue;
                changed = true;
                const Source* selected = &sources.first();
                for (const auto& source : sources) {
                    const auto platformOverrides = source.set.value(QStringLiteral("platforms")).toObject();
                    const auto active = platformOverrides.contains(platformId)
                        ? platformOverrides.value(platformId).toObject().value(QStringLiteral("active"))
                        : source.set.value(QStringLiteral("active"));
                    const bool enabled = sound.value(QStringLiteral("distinguishLanguage")).toBool()
                        ? source.language == root.value(QStringLiteral("defaultLanguage")).toString()
                        : source.language.isEmpty();
                    if (enabled && active.toString() == source.entryId) { selected = &source; break; }
                }
                const auto mappedRate = [&](const Source& source) {
                    const quint32 previous = oldRate(source, platform, overrides);
                    return source.sampleRate != 0 && previous > source.sampleRate
                        ? 0u : closestRate(previous);
                };
                QMap<quint64, QVector<const Source*>> bySize;
                for (const auto& source : sources) bySize[source.fileSize].append(&source);
                QJsonArray tiers;
                bool conflict = false;
                for (auto group = bySize.cbegin(); group != bySize.cend(); ++group) {
                    quint32 target = 0;
                    for (const auto* source : group.value())
                        if (mappedRate(*source) != 0) { target = mappedRate(*source); break; }
                    const auto candidate = QJsonObject{{QStringLiteral("sampleRateTiers"), singleTier(target)}};
                    for (const auto* source : group.value()) {
                        const quint32 wanted = mappedRate(*source);
                        const quint32 effective = source->sampleRate != 0 && wanted >= source->sampleRate ? 0 : wanted;
                        if (newRate(*source, candidate) != effective) conflict = true;
                    }
                    const bool last = std::next(group) == bySize.cend();
                    const QJsonValue maximum = last ? QJsonValue(QJsonValue::Null)
                        : QJsonValue(static_cast<qint64>(group.key()));
                    if (!tiers.isEmpty() && tiers.last().toObject().value(QStringLiteral("sampleRate")).toInteger() == target) {
                        auto previous = tiers.last().toObject();
                        previous.insert(QStringLiteral("maxBytes"), maximum);
                        tiers[tiers.size() - 1] = previous;
                    } else {
                        tiers.append(QJsonObject{{QStringLiteral("maxBytes"), maximum},
                            {QStringLiteral("sampleRate"), static_cast<qint64>(target)}});
                    }
                }
                if (conflict) tiers = singleTier(mappedRate(*selected));
                config.insert(QStringLiteral("sampleRateTiers"), tiers);
                configs[index] = config;
                for (const auto& source : sources) {
                    const quint32 previous = oldRate(source, platform, overrides);
                    const quint32 current = newRate(source, config);
                    if (previous == current) continue;
                    QJsonArray reasons;
                    if (conflict)
                        reasons.append(QStringLiteral("Sources of the same size require incompatible conversions; selected the platform active source for the default language or first source"));
                    if (source.sampleRate != 0 && previous > source.sampleRate)
                        reasons.append(QStringLiteral("Upsampling is replaced with Original"));
                    else if (closestRate(previous) != previous)
                        reasons.append(QStringLiteral("The sample rate is outside the supported list; selected the closest permitted rate"));
                    differences.append(QJsonObject{
                        {QStringLiteral("sound"), QJsonObject{{QStringLiteral("id"), node.value(QStringLiteral("guid"))},
                            {QStringLiteral("name"), node.value(QStringLiteral("name"))}}},
                        {QStringLiteral("source"), QJsonObject{{QStringLiteral("entryId"), source.entryId},
                            {QStringLiteral("sourceId"), source.sourceId}, {QStringLiteral("language"), source.language}}},
                        {QStringLiteral("platform"), QJsonObject{{QStringLiteral("id"), platformId},
                            {QStringLiteral("name"), platform.value(QStringLiteral("name"))}}},
                        {QStringLiteral("oldResult"), resultJson(source, previous)},
                        {QStringLiteral("newResult"), resultJson(source, current)},
                        {QStringLiteral("selectedSource"), conflict ? QJsonValue(selected->entryId) : QJsonValue(QJsonValue::Null)},
                        {QStringLiteral("reasons"), reasons}});
                }
            }
            if (changed) presetId = addPreset(QStringLiteral("Conversion"), configs);
        }
        if (node.value(QStringLiteral("fieldGroups")).isObject()) {
            auto groups = node.value(QStringLiteral("fieldGroups")).toObject();
            auto shared = groups.value(QStringLiteral("shared")).toObject();
            shared.insert(QStringLiteral("conversion"), conversionGroup(
                presetId.isEmpty() && rootNode ? defaultId : presetId));
            groups.insert(QStringLiteral("shared"), shared);
            node.insert(QStringLiteral("fieldGroups"), groups);
        }
        if (node.value(QStringLiteral("children")).isArray()) {
            auto children = node.value(QStringLiteral("children")).toArray();
            for (qsizetype index = 0; index < children.size(); ++index) {
                auto child = children[index].toObject();
                migrateNode(child, false);
                children[index] = child;
            }
            node.insert(QStringLiteral("children"), children);
        }
    };

    const auto paths = documents.keys();
    for (const auto& path : paths) {
        if (path == projectFile || path.startsWith(QStringLiteral("Conversions/"))) continue;
        auto document = documents.value(path);
        auto content = document.value(QStringLiteral("content")).toObject();
        const auto parentKind = content.value(QStringLiteral("parentRef")).toObject()
            .value(QStringLiteral("kind")).toString();
        migrateNode(content, parentKind == QStringLiteral("root"));
        if (!metadataValid) return false;
        document.insert(QStringLiteral("content"), content);
        documents.insert(path, document);
    }
    QJsonArray currentPlatforms;
    for (const auto& value : platforms) {
        auto platform = value.toObject();
        platform.remove(QStringLiteral("profile"));
        currentPlatforms.append(platform);
    }
    root.insert(QStringLiteral("deployPlatforms"), currentPlatforms);
    auto audio = root.value(QStringLiteral("audio")).toObject();
    audio.insert(QStringLiteral("defaultConversion"), QJsonObject{
        {QStringLiteral("id"), defaultId}, {QStringLiteral("documentId"), defaultId}});
    root.insert(QStringLiteral("audio"), audio);
    rootDocument.insert(QStringLiteral("content"), root);
    documents.insert(projectFile, rootDocument);
    if (registryFile.exists()) {
        QJsonObject retainedAssets;
        for (auto asset = assets.begin(); asset != assets.end(); ++asset) {
            auto retained = asset.value().toObject();
            retained.remove(QStringLiteral("settings"));
            retained.remove(QStringLiteral("platformCaches"));
            retainedAssets.insert(asset.key(), retained);
        }
        registry.insert(QStringLiteral("assets"), retainedAssets);
        registry.remove(QStringLiteral("fileOverrides"));
        documents.insert(registryPath, registry);
    }
    if (!differences.isEmpty()) {
        const QString reportName = NameValidator::makeUniqueName(QStringLiteral("conversion-migration-9-to-10"),
            [&](const QString& name) { return !QFileInfo::exists(QDir(sourceDirectory).filePath(name + QStringLiteral(".json"))); });
        const QString reportPath = reportName + QStringLiteral(".json");
        documents.insert(reportPath, QJsonObject{{QStringLiteral("fromSchema"), 9},
            {QStringLiteral("toSchema"), 10}, {QStringLiteral("differences"), differences}});
        if (warnings) warnings->append(QStringLiteral("Audio conversion settings changed during project upgrade. Review %1 (%2 differences).")
            .arg(QDir(sourceDirectory).filePath(reportPath)).arg(differences.size()));
    }
    return true;
}

}
