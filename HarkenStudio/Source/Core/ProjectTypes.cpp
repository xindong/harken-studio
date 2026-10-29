#include "ProjectTypes.h"

#include "AssetPipeline/AssetRegistry.h"
#include "BucketFolderStore.h"
#include "ItemTypeCatalog.h"
#include "NameValidator.h"
#include "SoundSourceSemantics.h"

#include "Components/VMixNode.h"
#include "Components/VTrack.h"
#include "Components/VSegment.h"

#include <QByteArray>
#include <QDir>
#include <QUuid>

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace VNX {

namespace {

const PhysicalFolder* findFolderChild(const PhysicalFolder* parent, const QString& name)
{
    if (!parent) return nullptr;
    for (const auto& child : parent->children) {
        if (child && child->name.compare(name, Qt::CaseInsensitive) == 0)
            return child.get();
    }
    return nullptr;
}

std::vector<MediaFileRef> mediaFilesForItem(const ProjectItem& item)
{
    std::vector<MediaFileRef> result;
    if (item.type != ProjectItem::Type::Sound) return result;
    const auto* sound = item.comp<VSound>();
    if (!sound) return result;
    const auto append = [&](const SoundSourceSet& set, const QString& language) {
        for (const auto& source : set.entries) {
            MediaFileRef ref;
            ref.sourceID = source.sourceId;
            ref.language = language;
            if (const AssetInfo* asset = AssetRegistry::instance().getAsset(source.sourceId)) {
                ref.relativePath = asset->relativePath;
                ref.lastModified = static_cast<uint64_t>(asset->lastModified);
            }
            result.push_back(std::move(ref));
        }
    };
    append(sound->sources.plain, {});
    for (auto it = sound->sources.localized.cbegin(); it != sound->sources.localized.cend(); ++it)
        append(it.value(), it.key());
    return result;
}

}

QString ProjectData::itemPath(const ProjectItem* item) const
{
    if (!item) return {};
    QStringList segments;
    const ProjectItem* rootItem = item;
    for (const ProjectItem* current = item; current; current = current->parent) {
        segments.prepend(current->name);
        rootItem = current;
    }
    for (const auto* binding : allFileBindings()) {
        if (binding && binding->rootItem.get() == rootItem) {
            const QString folderPath = folderRelPath(objectFolder(rootItem));
            if (!folderPath.isEmpty()) segments.prepend(folderPath);
            segments.prepend(binding->parentFolder);
            break;
        }
    }
    return segments.join(QLatin1Char('/'));
}

std::vector<ProjectItem*> ProjectData::findItemsBySelector(
    const QString& selector, std::optional<ProjectItem::Type> type) const
{
    std::vector<ProjectItem*> matches;
    if (selector.isEmpty()) return matches;
    QString key = selector.normalized(QString::NormalizationForm_C);
    if (key.startsWith(QStringLiteral("Pack://"))) key.remove(0, 7);
    const bool explicitGuid = key.startsWith(QStringLiteral("guid:"));
    const QUuid guid = QUuid::fromString(explicitGuid ? key.mid(5) : key);
    if (explicitGuid && guid.isNull()) return matches;
    const bool isPath = key.contains(QLatin1Char('/')) || key.contains(QLatin1Char('\\'));
    const QStringList segments = isPath
        ? QDir::fromNativeSeparators(key).split(QLatin1Char('/'), Qt::SkipEmptyParts) : QStringList{};
    const auto visit = [&](auto&& self, ProjectItem* item, qsizetype depth, bool parentMatches) -> void {
        if (!item) return;
        const auto sensitivity = item->type == ProjectItem::Type::Attenuation ? Qt::CaseInsensitive : Qt::CaseSensitive;
        const QString name = item->name.normalized(QString::NormalizationForm_C);
        const bool pathMatches = parentMatches && depth < segments.size()
            && name.compare(segments[depth], sensitivity) == 0;
        if ((!type || item->type == *type)
            && (!guid.isNull() ? item->guid == guid
                : (isPath ? pathMatches && depth + 1 == segments.size()
                          : name.compare(key, sensitivity) == 0)))
            matches.push_back(item);
        for (const auto& child : item->children) self(self, child.get(), depth + 1, pathMatches);
    };
    const auto visitRoot = [&](ProjectItem* root, const QString& parentPath) {
        const auto folders = parentPath.normalized(QString::NormalizationForm_C)
            .split(QLatin1Char('/'), Qt::SkipEmptyParts);
        bool parentMatches = folders.size() < segments.size();
        for (qsizetype i = 0; parentMatches && i < folders.size(); ++i)
            parentMatches = folders[i].compare(segments[i], Qt::CaseInsensitive) == 0;
        visit(visit, root, folders.size(), parentMatches);
    };
    for (const auto* binding : allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        QString parentPath = binding->parentFolder;
        const QString folderPath = folderRelPath(objectFolder(binding->rootItem.get()));
        if (!folderPath.isEmpty())
            parentPath += QLatin1Char('/') + folderPath;
        visitRoot(binding->rootItem.get(), parentPath);
    }
    return matches;
}

ProjectItem* ProjectData::findItemBySelector(
    const QString& selector, std::optional<ProjectItem::Type> type) const
{
    const auto matches = findItemsBySelector(selector, type);
    return matches.size() == 1 ? matches.front() : nullptr;
}

std::vector<const PhysicalFolder*> ProjectData::folderRootsForBucket(
    const QString& parentFolder) const
{
    std::vector<const PhysicalFolder*> result;
    for (const ProjectFolderBinding& binding : folders) {
        if (binding.parentFolder == parentFolder && binding.rootFolder) {
            result.push_back(binding.rootFolder.get());
        }
    }
    return result;
}

std::vector<PhysicalFolder*> ProjectData::folderRootsForBucket(const QString& parentFolder)
{
    std::vector<PhysicalFolder*> result;
    for (const PhysicalFolder* root :
         static_cast<const ProjectData&>(*this).folderRootsForBucket(parentFolder)) {
        result.push_back(const_cast<PhysicalFolder*>(root));
    }
    return result;
}

const PhysicalFolder* ProjectData::findFolderByPath(const QString& parentFolder,
                                                 const QString& relativePath) const
{
    const QString normalized = QDir::fromNativeSeparators(relativePath);
    const QStringList segments = normalized.split(QChar('/'), Qt::KeepEmptyParts);
    if (segments.isEmpty() || segments.contains(QString()))
        return nullptr;

    const PhysicalFolder* current = nullptr;
    for (const PhysicalFolder* root : folderRootsForBucket(parentFolder)) {
        if (root->name.compare(segments.front(), Qt::CaseInsensitive) == 0) {
            current = root;
            break;
        }
    }
    for (qsizetype i = 1; current && i < segments.size(); ++i)
        current = findFolderChild(current, segments[i]);
    return current;
}

PhysicalFolder* ProjectData::findFolderByPath(const QString& parentFolder,
                                           const QString& relativePath)
{
    return const_cast<PhysicalFolder*>(
        static_cast<const ProjectData&>(*this).findFolderByPath(parentFolder,
                                                                relativePath));
}

PhysicalFolder* ProjectData::ensureFolderPath(const QString& parentFolder,
                                           const QString& relativePath)
{
    const QString normalized = QDir::fromNativeSeparators(relativePath);
    if (parentFolder.isEmpty() || normalized.isEmpty()
        || QDir::isAbsolutePath(normalized)) {
        return nullptr;
    }

    const QStringList segments = normalized.split(QChar('/'), Qt::KeepEmptyParts);
    for (const QString& segment : segments) {
        NameValidator::Result result;
        if (segment == QStringLiteral(".") || segment == QStringLiteral("..")
            || segment.trimmed() != segment || segment.endsWith(QChar('.'))
            || !NameValidator::validateFileName(segment, &result)) {
            return nullptr;
        }
    }

    PhysicalFolder* parent = nullptr;
    QString currentPath;
    for (const QString& segment : segments) {
        currentPath = currentPath.isEmpty()
            ? segment
            : currentPath + QChar('/') + segment;
        PhysicalFolder* folder = findFolderByPath(parentFolder, currentPath);
        if (!folder) {
            auto recovered = std::make_shared<PhysicalFolder>();
            recovered->name = segment;
            recovered->parent = parent;
            folder = recovered.get();
            if (parent) {
                parent->children.push_back(std::move(recovered));
            } else {
                ProjectFolderBinding binding;
                binding.parentFolder = parentFolder;
                binding.rootFolder = std::move(recovered);
                folders.push_back(std::move(binding));
            }
        }
        parent = folder;
    }
    return parent;
}

QString ProjectData::folderRelPath(const PhysicalFolder* folder) const
{
    return folderBucket(folder).isEmpty() ? QString() : BucketFolderStore::folderRelPath(folder);
}

QString ProjectData::folderBucket(const PhysicalFolder* folder) const
{
    if (!folder) return {};
    const auto contains = [&](auto&& self, const PhysicalFolder* current) -> bool {
        if (!current) return false;
        if (current == folder) return true;
        for (const auto& child : current->children)
            if (self(self, child.get())) return true;
        return false;
    };
    for (const ProjectFolderBinding& binding : folders)
        if (contains(contains, binding.rootFolder.get())) return binding.parentFolder;
    return {};
}

const PhysicalFolder* ProjectData::folderForItem(const ProjectItem* item) const
{
    return objectFolder(item);
}

QString ProjectData::objectFilePath(const ProjectItem* item) const
{
    if (!item) return {};
    const auto it = documentBindings.constFind(item->guid);
    return it == documentBindings.cend() ? QString() : it->filePath;
}

PhysicalFolder* ProjectData::objectFolder(const ProjectItem* item) const
{
    if (!item) return nullptr;
    const auto it = documentBindings.constFind(item->guid);
    return it == documentBindings.cend() ? nullptr : it->folder;
}

void ProjectData::setObjectFileLocation(const ProjectItem* item, PhysicalFolder* folder,
                                      const QString& filePath)
{
    if (!item || item->guid.isNull()) return;
    ProjectDocumentBinding binding;
    binding.guid = item->guid;
    binding.parentFolder = ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(item->type));
    binding.folder = folder;
    binding.filePath = filePath.isEmpty() ? QString() : QDir::cleanPath(filePath);
    documentBindings.insert(item->guid, std::move(binding));
}

std::vector<ProjectDocumentBinding*> ProjectData::allDocumentBindings()
{
    std::vector<ProjectDocumentBinding*> result;
    result.reserve(documentBindings.size());
    for (auto it = documentBindings.begin(); it != documentBindings.end(); ++it)
        result.push_back(&it.value());
    return result;
}

std::vector<const ProjectDocumentBinding*> ProjectData::allDocumentBindings() const
{
    std::vector<const ProjectDocumentBinding*> result;
    result.reserve(documentBindings.size());
    for (auto it = documentBindings.cbegin(); it != documentBindings.cend(); ++it)
        result.push_back(&it.value());
    return result;
}

std::vector<const ProjectDocumentBinding*> ProjectData::documentBindingsUnderFolder(
    const PhysicalFolder* folder) const
{
    std::vector<const ProjectDocumentBinding*> result;
    const QString bucket = folderBucket(folder);
    if (bucket.isEmpty()) return result;
    for (const ProjectDocumentBinding* binding : allDocumentBindings()) {
        if (binding->parentFolder != bucket || folderBucket(binding->folder) != bucket) continue;
        for (const PhysicalFolder* owner = binding->folder; owner; owner = owner->parent) {
            if (owner == folder) {
                result.push_back(binding);
                break;
            }
        }
    }
    return result;
}

std::vector<ProjectDocumentBinding*> ProjectData::documentBindingsUnderFolder(
    const PhysicalFolder* folder)
{
    std::vector<ProjectDocumentBinding*> result;
    for (const ProjectDocumentBinding* binding :
         static_cast<const ProjectData&>(*this).documentBindingsUnderFolder(folder))
        result.push_back(const_cast<ProjectDocumentBinding*>(binding));
    return result;
}

QString ProjectData::itemFolderPath(const ProjectItem* item) const
{
    return folderRelPath(folderForItem(item));
}

std::vector<const ProjectFileBinding*> ProjectData::fileBindingsUnderFolder(
    const PhysicalFolder* folder) const
{
    std::vector<const ProjectFileBinding*> result;
    const QString bucket = folderBucket(folder);
    if (bucket.isEmpty()) return result;
    for (const ProjectFileBinding* binding : allFileBindings()) {
        if (!binding || !binding->rootItem || binding->parentFolder != bucket
            || folderBucket(objectFolder(binding->rootItem.get())) != bucket) continue;
        for (const PhysicalFolder* owner = objectFolder(binding->rootItem.get());
             owner; owner = owner->parent) {
            if (owner == folder) {
                result.push_back(binding);
                break;
            }
        }
    }
    return result;
}

std::vector<ProjectFileBinding*> ProjectData::fileBindingsUnderFolder(
    const PhysicalFolder* folder)
{
    std::vector<ProjectFileBinding*> result;
    for (const ProjectFileBinding* binding :
         static_cast<const ProjectData&>(*this).fileBindingsUnderFolder(folder)) {
        result.push_back(const_cast<ProjectFileBinding*>(binding));
    }
    return result;
}

std::vector<MediaFileRef> collectMediaFiles(const ProjectItem* root)
{
    std::vector<MediaFileRef> result;
    if (!root) return result;
    std::function<void(const ProjectItem*)> collect = [&](const ProjectItem* item) {
        if (item->type == ProjectItem::Type::Sound) {
            auto refs = mediaFilesForItem(*item);
            result.insert(result.end(), refs.begin(), refs.end());
        }
        for (const auto& child : item->children)
            collect(child.get());
    };
    collect(root);
    return result;
}

std::vector<uint32_t> getOwnedMedia(const ProjectItem* root)
{
    std::vector<uint32_t> result;
    for (const MediaFileRef& ref : collectMediaFiles(root))
        if (ref.sourceID != 0) result.push_back(ref.sourceID);
    return result;
}

bool ownsMedia(const ProjectItem* root, uint32_t sourceID)
{
    if (sourceID == 0) return false;
    const auto ids = getOwnedMedia(root);
    return std::find(ids.begin(), ids.end(), sourceID) != ids.end();
}

const MediaFileRef* getMediaRef(const ProjectItem* root, uint32_t sourceID)
{
    static thread_local std::vector<MediaFileRef> cachedMediaFiles;
    if (!root) return nullptr;
    cachedMediaFiles = collectMediaFiles(root);
    for (const MediaFileRef& ref : cachedMediaFiles)
        if (ref.sourceID == sourceID) return &ref;
    return nullptr;
}

bool validateProjectObjectIdentities(const ProjectData& project, QString* error)
{
    QSet<QUuid> identities;
    QSet<const ProjectItem*> visited;
    auto claim = [&](const QUuid& guid) {
        if (runtimeObjectId(guid) == harkenstudio::kMasterBusObjectId) {
            if (error) *error = QStringLiteral("The Runtime output root UUID is reserved");
            return false;
        }
        if (guid.isNull() || identities.contains(guid)) {
            if (error) *error = QStringLiteral("Project contains a missing or duplicate object UUID: %1")
                                    .arg(guid.toString(QUuid::WithoutBraces));
            return false;
        }
        identities.insert(guid);
        return true;
    };
    std::function<bool(const ProjectItem*)> visit = [&](const ProjectItem* item) {
        if (!item || visited.contains(item)) return true;
        visited.insert(item);
        if (!claim(item->guid)) return false;
        if (const auto* mix = item->comp<VMixNode>()) {
            for (const auto& binding : mix->bindings.entries()) {
                if (!binding.typed) {
                    if (error) *error = QStringLiteral("Project contains an invalid binding in %1").arg(item->name);
                    return false;
                }
                if (!claim(binding.guid)) return false;
            }
            for (const auto& effect : mix->effects.entries()) {
                if (!effect.typed) {
                    if (error) *error = QStringLiteral("Project contains an invalid effect in %1").arg(item->name);
                    return false;
                }
                if (!claim(effect.guid)) return false;
            }
        }
        if (const auto* timeline = item->comp<VTrack>())
            for (const auto& clip : timeline->clips.items)
                if (!claim(clip.clipGuid)) return false;
        if (const auto* timeline = item->comp<VSegment>())
            for (const auto& marker : timeline->markers.items)
                if (!claim(marker.markerGuid)) return false;
        for (const auto& child : item->children)
            if (!visit(child.get())) return false;
        return true;
    };
    if (!visit(project.root.get())) return false;
    for (const auto* binding : project.allFileBindings())
        if (binding && !visit(binding->rootItem.get())) return false;
    for (const auto& row : project.actionPublishedNameHistory) {
        if (row.objectGuid.isNull()) {
            if (error) *error = QStringLiteral("Action history contains a missing GUID");
            return false;
        }
    }
    return true;
}

ProjectItem* findItemByObjectID(ProjectItem* root, uint32_t objectID)
{
    if (!root) return nullptr;
    if (root->objectID == objectID) return root;

    for (const auto& child : root->children)
    {
        if (ProjectItem* found = findItemByObjectID(child.get(), objectID))
        {
            return found;
        }
    }
    return nullptr;
}

ProjectItem* findItemByAuthoringHandle(ProjectItem* root, uint32_t authoringHandle)
{
    return findItemByObjectID(root, authoringHandle);
}

ProjectItem* findItemByAuthoringHandleInProject(ProjectData* project, uint32_t authoringHandle)
{
    if (!project || authoringHandle == 0) return nullptr;

    for (const auto* binding : project->allFileBindings())
    {
        if (!binding || !binding->rootItem) continue;
        if (ProjectItem* found = findItemByAuthoringHandle(binding->rootItem.get(), authoringHandle))
        {
            return found;
        }
    }
    return nullptr;
}


}
