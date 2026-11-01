#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/FileContentCache.h"
#include "AssetPipeline/PathUtils.h"
#include "ProjectSerializer.h"
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
    QSet<const ProjectItem*> visited;
    const auto visit = [&](const auto& self, const ProjectItem* item) -> bool {
        if (!item || visited.contains(item)) return true;
        if (canceled && canceled()) return false;
        visited.insert(item);
        auto object = serializer.projectItemToJson(*item, false);
        if (object.isEmpty()) return false;
        object.insert(QStringLiteral("documentHeader"), item->documentHeader);
        object.insert(QStringLiteral("physicalPath"), project.itemPath(item));
        object.insert(QStringLiteral("documentPath"), project.objectFilePath(item));
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
    QJsonArray bindings;
    for (const auto* binding : project.allFileBindings()) {
        if (!binding || !binding->rootItem || !visit(visit, binding->rootItem.get())) return std::nullopt;
        bindings.append(QJsonObject{
            {QStringLiteral("root"), binding->rootItem->guid.toString(QUuid::WithoutBraces)},
            {QStringLiteral("bucket"), binding->parentFolder}});
    }
    QJsonObject media;
    for (auto it = snapshot.media.assets.cbegin(); it != snapshot.media.assets.cend(); ++it) {
        const auto& asset = it.value();
        media.insert(QString::number(it.key()), QJsonObject{
            {QStringLiteral("path"), asset.absolutePath},
            {QStringLiteral("relativePath"), asset.relativePath},
            {QStringLiteral("settings"), asset.settings.toJson()}});
    }
    QJsonObject overrides;
    for (auto it = snapshot.media.fileOverrides.cbegin(); it != snapshot.media.fileOverrides.cend(); ++it)
        overrides.insert(QString::number(it.key()), it.value().toJson());
    return QJsonObject{
        {QStringLiteral("projectId"), project.projectId},
        {QStringLiteral("name"), project.name},
        {QStringLiteral("header"), project.documentHeader},
        {QStringLiteral("projectPath"), project.projectPath},
        {QStringLiteral("languages"), QJsonArray::fromStringList(project.projectLanguages)},
        {QStringLiteral("defaultLanguage"), project.defaultLanguage},
        {QStringLiteral("platforms"), ProjectPlatformSemantics::platformsToJson(project.platforms)},
        {QStringLiteral("sampleRate"), project.sampleRate},
        {QStringLiteral("channelConfig"), static_cast<int>(project.channelConfig)},
        {QStringLiteral("exportProjectData"), project.buildExportProjectData},
        {QStringLiteral("exportPackMetadata"), project.buildExportPackMetadata},
        {QStringLiteral("exportWaveforms"), project.buildExportWaveforms},
        {QStringLiteral("waveformOutputDirectory"), project.buildWaveformOutputDir},
        {QStringLiteral("bindings"), bindings},
        {QStringLiteral("items"), items},
        {QStringLiteral("media"), media},
        {QStringLiteral("mediaOverrides"), overrides}};
}

}

std::optional<ProjectBuildIdentity> ProjectBuildSnapshot::buildIdentity(
    const std::function<bool()>& canceled) const
{
    if (canceled && canceled()) return std::nullopt;
    ProjectBuildIdentity identity;
    const auto object = projectIdentity(*this, &identity.objectGuids, canceled);
    if (!object) return std::nullopt;
    identity.fingerprint = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(*object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
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
    snapshot->fileContentCache = std::make_shared<FileContentCache>(
        QDir(PathUtils::getDotCacheDir(snapshot->media.projectRoot))
            .filePath(QStringLiteral("pack-build/file-content.json")));
    snapshot->media.sourceInputs = std::make_shared<BuildSourceInputs>(snapshot->fileContentCache);
    for (const auto& sourceAsset : source.audioSources) {
        if (snapshot->media.assets.contains(sourceAsset.sourceID)) continue;
        AssetInfo asset;
        asset.sourceID = sourceAsset.sourceID;
        asset.relativePath = sourceAsset.relativePath;
        asset.absolutePath = QDir(PathUtils::getAudioSourcesDir(snapshot->media.projectRoot))
            .filePath(asset.relativePath);
        snapshot->media.assets.insert(asset.sourceID, asset);
    }
    return snapshot;
}

}
