#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/SourceContentCache.h"
#include "AssetPipeline/PathUtils.h"
#include "ProjectSerializer.h"
#include "SoundSourceSemantics.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Tooling/Harkenizer/ResourceCompiler/ResourceCompiler.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QFileInfo>
#include <QDir>

namespace VNX {
namespace {

std::optional<QJsonObject> projectIdentity(const ProjectBuildSnapshot& snapshot,
                                         QMap<uint32_t, QString>* objectGuids,
                                         const std::function<bool()>& canceled)
{
    const auto& project = snapshot.project;
    ProjectSerializer serializer;
    if (!serializer.prepareReferenceWrite(project)) return std::nullopt;
    QJsonArray items;
    QSet<uint32_t> sourceIds;
    QSet<const ProjectItem*> visited;
    const auto visit = [&](const auto& self, const ProjectItem* item) -> bool {
        if (!item || visited.contains(item)) return true;
        if (canceled && canceled()) return false;
        visited.insert(item);
        auto object = serializer.projectItemToJson(*item, false);
        if (object.isEmpty()) return false;
        object.remove(QStringLiteral("notes"));
        object.remove(QStringLiteral("color"));
        if (const auto* sound = item->comp<VSound>())
            for (const auto* set : SoundSourceSemantics::sourceSets(sound->sources))
                for (const auto& source : set->entries) sourceIds.insert(source.sourceId);
        object.insert(QStringLiteral("treeParent"), item->parent
            ? item->parent->guid.toString(QUuid::WithoutBraces) : QString());
        QJsonArray children;
        for (const auto& child : item->children)
            children.append(child ? child->guid.toString(QUuid::WithoutBraces) : QString());
        object.insert(QStringLiteral("children"), children);
        items.append(object);
        if (item->objectID != 0)
            objectGuids->insert(item->objectID, item->guid.toString(QUuid::WithoutBraces));
        for (const auto& child : item->children)
            if (!self(self, child.get())) return false;
        return true;
    };
    if (!visit(visit, project.root.get())) return std::nullopt;
    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem || !visit(visit, binding->rootItem.get())) return std::nullopt;
    }
    QJsonObject media;
    for (const auto id : sourceIds) {
        const auto asset = snapshot.media.assets.constFind(id);
        if (asset == snapshot.media.assets.cend()) continue;
        const auto override = snapshot.media.fileOverrides.constFind(id);
        QJsonObject source{{QStringLiteral("path"), asset->absolutePath}};
        if (override != snapshot.media.fileOverrides.cend())
            source.insert(QStringLiteral("override"), override->toJson());
        else if (asset->settings.hash() != ConversionSettings{}.hash())
            source.insert(QStringLiteral("override"), asset->settings.toJson());
        media.insert(QString::number(id), source);
    }
    QJsonObject identity{
        {QStringLiteral("projectId"), project.projectId},
        {QStringLiteral("languages"), QJsonArray::fromStringList(project.projectLanguages)},
        {QStringLiteral("defaultLanguage"), project.defaultLanguage},
        {QStringLiteral("sampleRate"), project.sampleRate},
        {QStringLiteral("channelConfig"), static_cast<int>(project.channelConfig)},
        {QStringLiteral("exportPackMetadata"), project.buildExportPackMetadata},
        {QStringLiteral("exportWaveforms"), project.buildExportWaveforms},
        {QStringLiteral("items"), items},
        {QStringLiteral("media"), media}};
    if (project.buildExportWaveforms)
        identity.insert(QStringLiteral("waveformOutputDirectory"), project.buildWaveformOutputDir);
    return identity;
}

}

std::optional<ProjectBuildIdentity> ProjectBuildSnapshot::buildIdentity(
    const QString& platform, std::optional<ProjectBuildIdentity>& sharedIdentity,
    const std::function<bool()>& canceled) const
{
    if (canceled && canceled()) return std::nullopt;
    const auto* definition = ProjectPlatformSemantics::find(project.platforms, QUuid(platform));
    if (!definition) return std::nullopt;
    if (!sharedIdentity) {
        ProjectBuildIdentity shared;
        const auto object = projectIdentity(*this, &shared.objectGuids, canceled);
        if (!object) return std::nullopt;
        shared.fingerprint = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(*object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        sharedIdentity = std::move(shared);
    }
    auto identity = *sharedIdentity;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(identity.fingerprint.toLatin1());
    hash.addData(QJsonDocument(definition->toJson()).toJson(QJsonDocument::Compact));
    identity.fingerprint = QString::fromLatin1(hash.result().toHex());
    if (canceled && canceled()) return std::nullopt;
    return identity;
}

std::shared_ptr<const ProjectBuildSnapshot> ProjectBuildSnapshot::capture(
    const ProjectData& source, QString* error)
{
    if (!ProjectPlatformSemantics::validatePlatforms(source.platforms, error)) return {};
    auto snapshot = std::make_shared<ProjectBuildSnapshot>(source);
    snapshot->derivativeCache = std::make_shared<SoundSourceDerivative::MaterializationCache>();
    snapshot->mediaCache = std::make_shared<harkenizer::resourcecompiler::MediaCache>();
    snapshot->media = AssetRegistry::instance().buildSnapshot(
        QFileInfo(source.projectPath).absolutePath(), source.platforms, false);
    snapshot->sourceContentCache = std::make_shared<SourceContentCache>(
        QDir(PathUtils::getDotCacheDir(snapshot->media.projectRoot))
            .filePath(QStringLiteral("pack/sources.json")));
    snapshot->media.sourceInputs = std::make_shared<BuildSourceInputs>(snapshot->sourceContentCache);
    for (const auto& sourceAsset : source.audioSources) {
        if (snapshot->media.assets.contains(sourceAsset.sourceID)) continue;
        AssetInfo asset;
        asset.sourceID = sourceAsset.sourceID;
        asset.relativePath = sourceAsset.relativePath;
        asset.absolutePath = QDir(PathUtils::getAudioSourcesDir(snapshot->media.projectRoot))
            .filePath(asset.relativePath);
        snapshot->media.assets.insert(asset.sourceID, asset);
    }
    QStringList sources;
    for (const auto& asset : snapshot->media.assets) sources.append(asset.absolutePath);
    snapshot->sourceContentCache->retainSources(sources);
    return snapshot;
}

}
