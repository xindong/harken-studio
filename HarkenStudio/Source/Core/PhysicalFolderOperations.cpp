#include "PhysicalFolderOperations.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <QHash>
#include <QSet>

#include <algorithm>

namespace VNX {
namespace {

struct FolderPlacement {
    std::shared_ptr<PhysicalFolder> folder;
    PhysicalFolder* parent = nullptr;
    QString bucket;
    int index = -1;
};

FolderPlacement folderPlacement(ProjectData& project, PhysicalFolder* folder)
{
    FolderPlacement placement;
    placement.parent = folder->parent;
    placement.bucket = project.folderBucket(folder);
    if (placement.parent) {
        const auto& siblings = placement.parent->children;
        for (size_t index = 0; index < siblings.size(); ++index) {
            if (siblings[index].get() != folder) continue;
            placement.folder = siblings[index];
            placement.index = static_cast<int>(index);
            break;
        }
    } else {
        for (size_t index = 0; index < project.folders.size(); ++index) {
            if (project.folders[index].rootFolder.get() != folder) continue;
            placement.folder = project.folders[index].rootFolder;
            placement.index = static_cast<int>(index);
            break;
        }
    }
    return placement;
}

void detachFolder(ProjectData& project, PhysicalFolder* folder)
{
    if (folder->parent) {
        auto& siblings = folder->parent->children;
        const auto position = std::find_if(siblings.begin(), siblings.end(),
            [folder](const auto& sibling) { return sibling.get() == folder; });
        siblings.erase(position);
    } else {
        RootFolderSlot discard;
        ProjectManager::detachRootFolderBinding(&project, folder, &discard);
    }
}

void attachFolder(ProjectData& project, const FolderPlacement& placement)
{
    auto* folder = placement.folder.get();
    folder->parent = placement.parent;
    if (folder->parent) {
        auto& siblings = folder->parent->children;
        const auto position = placement.index < 0 ? siblings.size()
            : std::min(static_cast<size_t>(placement.index), siblings.size());
        siblings.insert(siblings.begin() + position, placement.folder);
    } else {
        RootFolderSlot slot;
        slot.binding.parentFolder = placement.bucket;
        slot.binding.rootFolder = placement.folder;
        slot.index = placement.index;
        ProjectManager::attachRootFolderBinding(&project, slot);
    }
}

class CreateFolderCommand final : public TransactionalUndoCommand
{
public:
    explicit CreateFolderCommand(FolderPlacement placement)
        : TransactionalUndoCommand(QObject::tr("New Folder"), nullptr, false)
        , placement_(std::move(placement))
    {}

private:
    bool apply(bool redo) override
    {
        auto& manager = ProjectManager::instance();
        auto* project = manager.currentProject();
        if (!project) return false;
        if (redo) {
            if ((placement_.parent && project->folderBucket(placement_.parent) != placement_.bucket)
                || !manager.isFolderNameUniqueAmongSiblings(placement_.parent, placement_.bucket,
                                                            placement_.folder->name)) return false;
            attachFolder(*project, placement_);
            emit manager.folderCreated(placement_.folder.get());
        } else {
            if (project->folderBucket(placement_.folder.get()) != placement_.bucket) return false;
            detachFolder(*project, placement_.folder.get());
            emit manager.folderDeleted(placement_.folder.get());
        }
        return true;
    }

    FolderPlacement placement_;
};

class RenameFolderCommand final : public TransactionalUndoCommand
{
public:
    RenameFolderCommand(std::shared_ptr<PhysicalFolder> folder, QString name)
        : TransactionalUndoCommand(QObject::tr("Rename Folder"), nullptr, false)
        , folder_(std::move(folder)), before_(folder_->name), after_(std::move(name))
    {}

private:
    bool apply(bool redo) override
    {
        auto& manager = ProjectManager::instance();
        auto* project = manager.currentProject();
        if (!project) return false;
        const QString bucket = project->folderBucket(folder_.get());
        const QString& name = redo ? after_ : before_;
        if (bucket.isEmpty() || !manager.isFolderNameUniqueAmongSiblings(
                folder_->parent, bucket, name, folder_.get())) return false;
        const QString previous = folder_->name;
        folder_->name = name;
        emit manager.folderRenamed(folder_.get(), previous);
        return true;
    }

    std::shared_ptr<PhysicalFolder> folder_;
    QString before_;
    QString after_;
};

struct ItemPlacement {
    ProjectItem* item = nullptr;
    PhysicalFolder* before = nullptr;
    PhysicalFolder* after = nullptr;
};

struct FolderMovePlacement {
    FolderPlacement before;
    FolderPlacement after;
};

class MoveFolderEntriesCommand final : public TransactionalUndoCommand
{
public:
    MoveFolderEntriesCommand(std::vector<ItemPlacement> items,
                             std::vector<FolderMovePlacement> folders,
                             std::unique_ptr<TransactionalUndoCommand> business)
        : TransactionalUndoCommand(business ? business->text() : QObject::tr("Move entries"), nullptr, false)
        , items_(std::move(items)), folders_(std::move(folders)), business_(std::move(business))
    {}

private:
    bool apply(bool redo) override
    {
        auto& manager = ProjectManager::instance();
        auto* project = manager.currentProject();
        if (!project) return false;
        for (const auto& placement : items_) {
            auto* destination = redo ? placement.after : placement.before;
            const QString bucket = ItemTypeCatalog::folderNameOf(
                ItemTypeCatalog::bucketOf(placement.item->type));
            if (manager.findItemByGuid(placement.item->guid) != placement.item
                || (destination && project->folderBucket(destination) != bucket)) return false;
        }
        for (const auto& move : folders_) {
            const auto& destination = redo ? move.after : move.before;
            if (project->folderBucket(destination.folder.get()) != destination.bucket
                || (destination.parent && project->folderBucket(destination.parent) != destination.bucket)
                || !manager.isFolderNameUniqueAmongSiblings(destination.parent, destination.bucket,
                    destination.folder->name, destination.folder.get())) return false;
        }
        if (business_ && !business_->tryReplay(redo)) return false;
        for (const auto& placement : items_)
            project->setObjectFileLocation(placement.item, redo ? placement.after : placement.before,
                                          project->objectFilePath(placement.item));
        std::vector<const FolderPlacement*> destinations;
        for (const auto& move : folders_) {
            detachFolder(*project, move.before.folder.get());
            destinations.push_back(redo ? &move.after : &move.before);
        }
        if (!redo)
            std::stable_sort(destinations.begin(), destinations.end(), [](const auto* left, const auto* right) {
                return left->index < right->index;
            });
        for (const auto* placement : destinations) attachFolder(*project, *placement);
        for (const auto& move : folders_) emit manager.folderMoved(move.before.folder.get());
        for (const auto& placement : items_)
            emit manager.itemMoved(placement.item, placement.item->parent, placement.item->parent);
        return true;
    }

    std::vector<ItemPlacement> items_;
    std::vector<FolderMovePlacement> folders_;
    std::unique_ptr<TransactionalUndoCommand> business_;
};

bool collectItems(ProjectItem* item, PhysicalFolder* destination,
                  QHash<ProjectItem*, PhysicalFolder*>& items)
{
    if (!item || ProjectManager::instance().findItemByGuid(item->guid) != item
        || !ItemTypeCatalog::isFolderAddressable(item->type)) return false;
    if (items.contains(item)) return items.value(item) == destination;
    items.insert(item, destination);
    for (const auto& child : item->children)
        if (!collectItems(child.get(), destination, items)) return false;
    return true;
}

bool pushCommand(std::unique_ptr<TransactionalUndoCommand> command)
{
    if (!command) return true;
    if (!command->tryReplay(true)) return false;
    UndoRedoManager::instance().push(command.release());
    return true;
}

}

bool createPhysicalFolder(PhysicalFolder* parent,
                          const std::shared_ptr<PhysicalFolder>& folder,
                          const QString& bucket)
{
    if (!folder || !ProjectManager::instance().currentProject()) return false;
    return pushCommand(std::make_unique<CreateFolderCommand>(FolderPlacement{folder, parent, bucket}));
}

bool renamePhysicalFolder(PhysicalFolder* folder, const QString& name)
{
    auto* project = ProjectManager::instance().currentProject();
    if (!project || !folder) return false;
    auto placement = folderPlacement(*project, folder);
    if (!placement.folder) return false;
    return pushCommand(std::make_unique<RenameFolderCommand>(std::move(placement.folder), name));
}

bool executePhysicalMoves(const std::vector<PhysicalItemMove>& items,
                          const std::vector<PhysicalFolderMove>& folders,
                          std::unique_ptr<TransactionalUndoCommand> business)
{
    auto& manager = ProjectManager::instance();
    auto* project = manager.currentProject();
    if (!project) return false;
    QHash<ProjectItem*, PhysicalFolder*> itemTargets;
    for (const auto& move : items)
        if (!collectItems(move.item, move.destination, itemTargets)) return false;
    std::vector<ItemPlacement> itemPlacements;
    for (auto it = itemTargets.cbegin(); it != itemTargets.cend(); ++it) {
        const QString bucket = ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(it.key()->type));
        if (it.value() && project->folderBucket(it.value()) != bucket) return false;
        if (project->objectFolder(it.key()) != it.value())
            itemPlacements.push_back({it.key(), project->objectFolder(it.key()), it.value()});
    }
    std::vector<FolderMovePlacement> folderPlacements;
    QSet<PhysicalFolder*> seen;
    for (const auto& move : folders) {
        if (!move.folder || !ProjectManager::bucketAllowsFolder(move.bucket)
            || project->folderBucket(move.folder) != move.bucket
            || (move.destination && project->folderBucket(move.destination) != move.bucket)) return false;
        if (move.folder->parent == move.destination || seen.contains(move.folder)) continue;
        for (auto* ancestor = move.destination; ancestor; ancestor = ancestor->parent)
            if (ancestor == move.folder) return false;
        if (!manager.isFolderNameUniqueAmongSiblings(move.destination, move.bucket,
                                                     move.folder->name, move.folder)) {
            emit manager.errorOccurred(QObject::tr("A folder with this name already exists."));
            return false;
        }
        for (const auto& placement : folderPlacements) {
            if (placement.after.parent == move.destination
                && placement.after.bucket == move.bucket
                && placement.after.folder->name.compare(move.folder->name, Qt::CaseInsensitive) == 0) {
                emit manager.errorOccurred(QObject::tr("A folder with this name already exists."));
                return false;
            }
        }
        auto before = folderPlacement(*project, move.folder);
        if (!before.folder) return false;
        auto after = before;
        after.parent = move.destination;
        after.index = -1;
        folderPlacements.push_back({std::move(before), std::move(after)});
        seen.insert(move.folder);
    }
    if (itemPlacements.empty() && folderPlacements.empty()) return pushCommand(std::move(business));
    return pushCommand(std::make_unique<MoveFolderEntriesCommand>(
        std::move(itemPlacements), std::move(folderPlacements), std::move(business)));
}

}
