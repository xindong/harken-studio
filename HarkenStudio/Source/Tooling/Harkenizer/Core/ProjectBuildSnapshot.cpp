#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VAudio.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"
#include "Components/VSegment.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"

#include <QFileInfo>
#include <QDir>

namespace VNX {
namespace {

template<typename T>
void copyComponent(const ProjectItem& source, ProjectItem& target)
{
    if (const auto* component = source.comp<T>()) {
        auto* copy = target.compOrCreate<T>();
        *copy = *component;
        copy->setHostItem(&target);
    }
}

std::shared_ptr<ProjectItem> copyItem(
    const std::shared_ptr<ProjectItem>& source,
    QHash<const ProjectItem*, std::shared_ptr<ProjectItem>>& copies)
{
    if (!source) return {};
    if (copies.contains(source.get())) return copies.value(source.get());
    auto item = std::make_shared<ProjectItem>();
    copies.insert(source.get(), item);
    item->name = source->name;
    item->type = source->type;
    item->sourcePath = source->sourcePath;
    item->sourceID = source->sourceID;
    item->objectID = source->objectID;
    item->guid = source->guid;
    item->nameBindingRevision = source->nameBindingRevision;
    item->documentHeader = source->documentHeader;
    item->extensions = source->extensions;
    item->objectPath = source->objectPath;
    item->notes = source->notes;
    item->color = source->color;
    item->playablePropertyPresence = source->playablePropertyPresence;
    item->platformOverrides = source->platformOverrides;
    item->fieldGroups = source->fieldGroups;
    item->parentRef = source->parentRef;
    item->dirtyBit = source->dirtyBit;
    item->outputBusOverride = source->outputBusOverride;
    item->canvasPos = source->canvasPos;
    copyComponent<VMixNode>(*source, *item);
    copyComponent<VAudio>(*source, *item);
    copyComponent<VSelection>(*source, *item);
    copyComponent<VBus>(*source, *item);
    copyComponent<VAction>(*source, *item);
    copyComponent<VVariable>(*source, *item);
    copyComponent<VAttenuation>(*source, *item);
    copyComponent<VSpatial>(*source, *item);
    copyComponent<VTrack>(*source, *item);
    copyComponent<VSegment>(*source, *item);
    copyComponent<VSound>(*source, *item);
    for (const auto& child : source->children)
        item->children.push_back(copyItem(child, copies));
    return item;
}

std::shared_ptr<PhysicalFolder> copyFolder(
    const std::shared_ptr<PhysicalFolder>& source,
    PhysicalFolder* parent,
    QHash<const PhysicalFolder*, PhysicalFolder*>& copies)
{
    if (!source) return {};
    auto folder = std::make_shared<PhysicalFolder>();
    copies.insert(source.get(), folder.get());
    folder->name = source->name;
    folder->parent = parent;
    for (const auto& child : source->children)
        folder->children.push_back(copyFolder(child, folder.get(), copies));
    return folder;
}

}

std::shared_ptr<const ProjectBuildSnapshot> ProjectBuildSnapshot::capture(
    const ProjectData& source, QString* error)
{
    if (!ProjectPlatformSemantics::validatePlatforms(source.platforms, error)) return {};
    auto snapshot = std::make_shared<ProjectBuildSnapshot>();
    snapshot->derivativeCache = std::make_shared<SoundSourceDerivative::MaterializationCache>();
    snapshot->project = source;
    QHash<const ProjectItem*, std::shared_ptr<ProjectItem>> copies;
    snapshot->project.root = copyItem(source.root, copies);
    for (auto* binding : snapshot->project.allFileBindings())
        binding->rootItem = copyItem(binding->rootItem, copies);
    QHash<const PhysicalFolder*, PhysicalFolder*> folderCopies;
    for (auto& binding : snapshot->project.folders)
        binding.rootFolder = copyFolder(binding.rootFolder, nullptr, folderCopies);
    for (auto* binding : snapshot->project.allDocumentBindings())
        binding->folder = folderCopies.value(binding->folder);
    for (auto it = copies.cbegin(); it != copies.cend(); ++it)
        it.value()->parent = copies.value(it.key()->parent).get();
    snapshot->media = AssetRegistry::instance().buildSnapshot(
        QFileInfo(source.projectPath).absolutePath(), source.platforms, false);
    snapshot->media.sourceInputs = std::make_shared<BuildSourceInputs>();
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
