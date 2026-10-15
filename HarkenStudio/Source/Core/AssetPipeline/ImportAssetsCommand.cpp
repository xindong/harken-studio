
#include "ImportAssetsCommand.h"
#include "AssetImporter.h"
#include "AssetRegistry.h"
#include "AudioImportGate.h"
#include "PathUtils.h"
#include "SourceFileSystem.h"
#include "../AuthoringPolicy.h"
#include "../NameValidator.h"
#include "../ProjectTypes.h"
#include "../ProjectManager.h"
#include "../FieldGroupProjectAdapter.h"
#include "../UndoCommands.h"
#include "../../Widgets/LogPanel/LogPanel.h"

#include <QFile>
#include <QFileInfo>
#include <QDebug>

#include <memory>
#include <algorithm>
#include <vector>

namespace VNX {


static ProjectItem* findItemByObjectIDRecursive(ProjectItem* root, uint32_t objectID)
{
    if (!root || objectID == 0) return nullptr;
    if (root->objectID == objectID) return root;

    for (const auto& child : root->children)
    {
        if (ProjectItem* found = findItemByObjectIDRecursive(child.get(), objectID))
        {
            return found;
        }
    }
    return nullptr;
}

static ImportAssetsCommand* pushCollected(std::unique_ptr<ImportAssetsCommand> command)
{
    if (command->successCount() == 0) {
        static std::vector<std::unique_ptr<ImportAssetsCommand>> failed;
        ImportAssetsCommand* raw = command.get();
        failed.push_back(std::move(command));
        return raw;
    }
    ImportAssetsCommand* raw = command.release();
    UndoRedoManager::instance().push(raw);
    return raw;
}

ImportAssetsCommand::ImportAssetsCommand(const QStringList& externalPaths,
                                         ProjectFileBinding* targetBinding,
                                         ProjectItem* targetParent,
                                         QUndoCommand* parent)
    : TransactionalUndoCommand({}, parent, false)
    , externalPaths_(externalPaths)
    , targetBinding_(targetBinding)
    , targetParentObjectID_(targetParent ? targetParent->objectID : 0)
{
    if (externalPaths.size() == 1) {
        setText(QObject::tr("Import %1").arg(QFileInfo(externalPaths.first()).fileName()));
    } else {
        setText(QObject::tr("Import %1 files").arg(externalPaths.size()));
    }
}

ImportAssetsCommand::ImportAssetsCommand(const QStringList& externalPaths,
                                         QUndoCommand* parent)
    : TransactionalUndoCommand({}, parent, false)
    , externalPaths_(externalPaths)
    , rootMode_(true)
{
    if (externalPaths.size() == 1) {
        setText(QObject::tr("Import %1").arg(QFileInfo(externalPaths.first()).fileName()));
    } else {
        setText(QObject::tr("Import %1 files").arg(externalPaths.size()));
    }
}

ImportAssetsCommand::~ImportAssetsCommand() = default;

ImportAssetsCommand* ImportAssetsCommand::pushImport(const QStringList& externalPaths,
                                                     ProjectFileBinding* targetBinding,
                                                     ProjectItem* targetParent)
{
    auto command = std::make_unique<ImportAssetsCommand>(
        externalPaths, targetBinding, targetParent);
    command->collectParentEntries();
    return pushCollected(std::move(command));
}

ImportAssetsCommand* ImportAssetsCommand::pushImportAtRoot(const QStringList& externalPaths,
                                                        const QUuid& outputBusGuid)
{
    auto command = std::make_unique<ImportAssetsCommand>(externalPaths);
    command->rootOutputBusGuid_ = outputBusGuid;
    command->collectRootEntries();
    return pushCollected(std::move(command));
}

ImportAssetsCommand* ImportAssetsCommand::pushImportSources(const QStringList& externalPaths,
                                                          const QString& sourceFolder,
                                                          const QMap<QString, QString>& replacements)
{
    auto command = std::make_unique<ImportAssetsCommand>(
        externalPaths, static_cast<ProjectFileBinding*>(nullptr));
    command->sourceFolder_ = sourceFolder;
    command->sourceMode_ = true;
    QMap<QString, QString> fileChanges = replacements;
    for (const QString& path : externalPaths) {
        const QString relative = sourceFolder.isEmpty() ? QFileInfo(path).fileName()
            : sourceFolder + QLatin1Char('/') + QFileInfo(path).fileName();
        const QString destination = SourceFileSystem::absolutePath(relative);
        if (!fileChanges.contains(relative) && QFileInfo(destination).isFile()
            && !AssetRegistry::instance().findAssetByPath(destination)) {
            const QString hash = AssetImporter::computeContentHash(path);
            if (!hash.isEmpty() && hash == AssetImporter::computeContentHash(destination))
                fileChanges.insert(relative, path);
        }
    }
    if (!fileChanges.isEmpty()) {
        QString error;
        auto replacement = SourceFileSystem::replaceFiles(fileChanges, &error);
        if (!replacement) {
            command->failCount_ = externalPaths.size();
            LogPanel::logError(LogCategory::Import, error);
            return pushCollected(std::move(command));
        }
        command->fileChanges_.append(std::move(replacement));
        for (auto it = fileChanges.cbegin(); it != fileChanges.cend(); ++it) {
            command->replacedPaths_.insert(it.key());
            for (QString& path : command->externalPaths_)
                if (path == it.value())
                    path = SourceFileSystem::absolutePath(it.key());
        }
    }
    command->collectParentEntries();
    return pushCollected(std::move(command));
}

QList<uint32_t> ImportAssetsCommand::importedIDs() const
{
    QList<uint32_t> ids;
    ids.reserve(entries_.size());
    for (const ImportedEntry& entry : entries_) {
        ids.append(entry.sourceID);
    }
    return ids;
}

QList<QUuid> ImportAssetsCommand::importedItemGuids() const
{
    QList<QUuid> guids;
    guids.reserve(entries_.size());
    for (const ImportedEntry& entry : entries_) {
        guids.append(entry.itemGuid);
    }
    return guids;
}

ProjectItem* ImportAssetsCommand::resolveParentForRedo() const
{
    if (!targetBinding_ || !targetBinding_->rootItem) {
        return nullptr;
    }
    ProjectItem* root = targetBinding_->rootItem.get();
    ProjectItem* preferred = findItemByObjectIDRecursive(root, targetParentObjectID_);
    const auto placement = AudioImportGate::resolveImportParent(preferred ? preferred : root);
    return placement.kind == ImportPlacementKind::Parent ? placement.parent : nullptr;
}

void ImportAssetsCommand::collectParentEntries()
{
    ProjectItem* importParent = resolveParentForRedo();
    if (importParent) {
        auto& manager = ProjectManager::instance();
        ProjectItem candidate(QStringLiteral("Imported Sound"), ProjectItem::Type::Sound);
        candidate.attachDefaultComponents();
        const auto authoring = AuthoringPolicy::validateNewSubtree(
            candidate, importParent,
            [&manager](const QUuid& guid) { return manager.findItemByGuid(guid); },
            [&manager](uint32_t objectID) { return manager.findItemByObjectID(objectID); });
        if (!authoring) {
            failCount_ = externalPaths_.size();
            setObsolete(true);
            LogPanel::logError(LogCategory::Import, authoring.message);
            return;
        }
    }
    QString projectRoot = AssetRegistry::instance().projectRoot();
    LogPanel::logInfo(LogCategory::Import, QString("ProjectRoot: %1").arg(projectRoot));

    if (projectRoot.isEmpty()) {
        LogPanel::logError(LogCategory::Import, "ERROR: projectRoot is empty! AssetRegistry not initialized.");
    }

    AssetImporter importer;
    importer.setProjectRoot(projectRoot);

    entries_.clear();
    successCount_ = 0;
    failCount_ = 0;
    duplicateCount_ = 0;
    for (const QString& path : externalPaths_) {
        qDebug() << "[ImportAssetsCommand] Importing file:" << path;

        if (importParent) {
            NameValidator::Result vr;
            if (!NameValidator::validateDisplayName(QFileInfo(path).completeBaseName(), &vr)) {
                ++failCount_;
                LogPanel::logError(LogCategory::Import,
                    QString("Skipped %1: %2 Rename the file, then import it again.")
                        .arg(QFileInfo(path).fileName(),
                             NameValidator::resultMessage(vr)));
                continue;
            }
        }

        ImportResult result = importer.importFile(path, targetBinding_, sourceFolder_, !sourceMode_);

        if (!result.success) {
            ++failCount_;
            LogPanel::logError(LogCategory::Import, QString("Failed: %1 - %2").arg(path, result.errorMessage));
            continue;
        }

        if (sourceMode_ && result.deduplicated && !result.fileChange
            && !replacedPaths_.contains(result.relativePath)) {
            ++duplicateCount_;
            continue;
        }

        ImportedEntry entry;
        entry.sourceID = result.sourceID;
        entry.relativePath = result.relativePath;

        if (importParent) {
            entry.itemName = QFileInfo(result.absolutePath).completeBaseName();

            ProjectItem* newItem = ProjectManager::instance().createAudioItem(
                importParent,
                entry.itemName,
                entry.relativePath,
                entry.sourceID
            );

            if (!newItem) {
                ++failCount_;
                LogPanel::logError(LogCategory::Import,
                    QString("Failed: %1 - the object could not be created under '%2'")
                        .arg(path, importParent->name));
                if (result.fileChange)
                    result.fileChange->tryReplay(false);
                continue;
            }

            entry.itemGuid = newItem->guid;
            entry.itemName = newItem->name;
            const auto item = std::find_if(importParent->children.begin(), importParent->children.end(),
                [newItem](const auto& child) { return child.get() == newItem; });
            entry.itemCreation = std::make_shared<CreateProjectItemCommand>(importParent, *item);
            entry.itemCreation->redo();
            ProjectManager::resyncItemToRuntime(newItem);
        }

        if (result.fileChange)
            fileChanges_.append(result.fileChange);
        entries_.append(entry);

        ++successCount_;
        if (result.deduplicated && result.fileChange) {
            LogPanel::logInfo(LogCategory::Import, QString("Restored: %1").arg(result.absolutePath));
        } else if (sourceMode_ && replacedPaths_.contains(result.relativePath)) {
            LogPanel::logInfo(LogCategory::Import, QString("Replaced: %1").arg(result.absolutePath));
        } else if (result.deduplicated) {
            ++duplicateCount_;
            LogPanel::logInfo(LogCategory::Import,
                QString("Reused existing asset (same content): %1").arg(QFileInfo(path).fileName()));
        } else {
            const QString originalName = QFileInfo(path).fileName();
            const QString storedName = QFileInfo(result.absolutePath).fileName();
            if (storedName != originalName) {
                LogPanel::logInfo(LogCategory::Import,
                    QString("Name conflict: %1 stored as %2").arg(originalName, storedName));
            }
            LogPanel::logInfo(LogCategory::Import, QString("Success: %1").arg(result.absolutePath));
        }
    }

    if (targetBinding_ && targetBinding_->rootItem) {
        targetBinding_->rootItem->markDirty();
        qDebug() << "[ImportAssetsCommand] Marked binding root dirty:"
                 << targetBinding_->rootItem->name
                 << "rootItem children count:" << targetBinding_->rootItem->children.size()
                 << "filePath:" << ProjectManager::instance().currentProject()->objectFilePath(targetBinding_->rootItem.get());
    }
}

void ImportAssetsCommand::collectRootEntries()
{
    if (!rootOutputBusGuid_.isNull()) {
        const auto* bus = ProjectManager::instance().findItemByGuid(rootOutputBusGuid_);
        if (!bus || bus->type != ProjectItem::Type::Bus) {
            failCount_ = externalPaths_.size();
            return;
        }
    }
    QString projectRoot = AssetRegistry::instance().projectRoot();
    LogPanel::logInfo(LogCategory::Import, QString("ProjectRoot: %1").arg(projectRoot));

    if (projectRoot.isEmpty()) {
        LogPanel::logError(LogCategory::Import, "ERROR: projectRoot is empty! AssetRegistry not initialized.");
    }

    AssetImporter importer;
    importer.setProjectRoot(projectRoot);

    entries_.clear();
    successCount_ = 0;
    failCount_ = 0;
    duplicateCount_ = 0;

    for (const QString& path : externalPaths_) {
        NameValidator::Result vr;
        if (!NameValidator::validateDisplayName(QFileInfo(path).completeBaseName(), &vr)) {
            ++failCount_;
            LogPanel::logError(LogCategory::Import,
                QString("Skipped %1: %2 Rename the file, then import it again.")
                    .arg(QFileInfo(path).fileName(),
                         NameValidator::resultMessage(vr)));
            continue;
        }

        ImportResult result = importer.importFile(path, nullptr);

        if (!result.success) {
            ++failCount_;
            LogPanel::logError(LogCategory::Import, QString("Failed: %1 - %2").arg(path, result.errorMessage));
            continue;
        }

        ImportedEntry entry;
        entry.sourceID = result.sourceID;
        entry.relativePath = result.relativePath;

        entry.itemName = ProjectManager::instance().suggestUniqueName(
            ProjectItem::Type::Sound,
            QFileInfo(result.absolutePath).completeBaseName());

        ProjectItem* newItem = ProjectManager::instance().createRootAudioItem(
            entry.itemName, entry.relativePath, entry.sourceID);

        if (newItem && !rootOutputBusGuid_.isNull()) {
            auto groups = newItem->fieldGroups;
            groups.mainOutput.shared = {FieldGroupMode::Inherit,
                MainOutputFieldGroup{toFieldGroupGuid(rootOutputBusGuid_), 0.0f}};
            if (!ProjectManager::instance().restoreFieldGroupsInternal(newItem, groups)) {
                RootBindingSlot discarded;
                if (ProjectManager::detachRootBinding(
                        ProjectManager::instance().currentProject(), newItem, &discarded))
                    emit ProjectManager::instance().itemDeleted(newItem);
                newItem = nullptr;
            }
        }

        if (!newItem) {
            ++failCount_;
            LogPanel::logError(LogCategory::Import,
                QString("Failed: %1 - the object could not be created at root level").arg(path));
            if (result.fileChange)
                result.fileChange->tryReplay(false);
            continue;
        }

        entry.itemGuid = newItem->guid;

        if (!result.deduplicated) {
            if (AssetInfo* info = AssetRegistry::instance().getAsset(entry.sourceID)) {
                info->ownerObjectGuid = newItem->guid.toString(QUuid::WithoutBraces);
            }
        }

        ProjectManager::resyncItemToRuntime(newItem);

        if (result.fileChange)
            fileChanges_.append(result.fileChange);
        entries_.append(entry);

        ++successCount_;
        if (result.deduplicated && result.fileChange) {
            LogPanel::logInfo(LogCategory::Import, QString("Restored: %1").arg(result.absolutePath));
        } else if (result.deduplicated) {
            ++duplicateCount_;
            LogPanel::logInfo(LogCategory::Import,
                QString("Reused existing asset (same content): %1").arg(QFileInfo(path).fileName()));
        } else {
            const QString originalName = QFileInfo(path).fileName();
            const QString storedName = QFileInfo(result.absolutePath).fileName();
            if (storedName != originalName) {
                LogPanel::logInfo(LogCategory::Import,
                    QString("Name conflict: %1 stored as %2").arg(originalName, storedName));
            }
            LogPanel::logInfo(LogCategory::Import, QString("Success: %1").arg(result.absolutePath));
        }
    }
}

bool ImportAssetsCommand::apply(bool redo)
{
    bool rollbackFailed = false;
    if (!SourceFileSystem::replayFiles(fileChanges_, redo, &rollbackFailed)) {
        setObsolete(rollbackFailed);
        return false;
    }
    if (redo) redoEntries();
    else undoEntries();
    return true;
}

void ImportAssetsCommand::redoEntries()
{
    if (rootMode_) {
        redoRootMode();
        return;
    }

    if (firstRedo_) {
        if (entries_.isEmpty() && successCount_ == 0 && failCount_ == 0)
            collectParentEntries();
        firstRedo_ = false;
        return;
    }

    for (ImportedEntry& entry : entries_) {
        if (entry.itemCreation) entry.itemCreation->redo();
    }

    if (targetBinding_ && targetBinding_->rootItem) {
        targetBinding_->rootItem->markDirty();
    }
}

void ImportAssetsCommand::undoEntries()
{
    if (rootMode_) {
        undoRootMode();
        return;
    }

    for (auto it = entries_.crbegin(); it != entries_.crend(); ++it) {
        const ImportedEntry& entry = *it;
        if (entry.itemCreation) entry.itemCreation->undo();

    }

    if (targetBinding_ && targetBinding_->rootItem) {
        targetBinding_->rootItem->markDirty();
    }

    qDebug() << "Undo import: rolled back" << entries_.size() << "imported entries";
}

void ImportAssetsCommand::redoRootMode()
{
    if (firstRedo_) {
        if (entries_.isEmpty() && successCount_ == 0 && failCount_ == 0)
            collectRootEntries();
        firstRedo_ = false;
        return;
    }

    ProjectData* project = ProjectManager::instance().currentProject();

    for (ImportedEntry& entry : entries_) {
        if (!project || !entry.rootSlot.valid()
            || ProjectManager::instance().findItemByGuid(entry.itemGuid)) {
            continue;
        }

        if (!ProjectManager::attachRootBinding(project, entry.rootSlot)) {
            continue;
        }
        emit ProjectManager::instance().itemCreated(entry.rootSlot.binding.rootItem.get());
        ProjectManager::resyncItemToRuntime(entry.rootSlot.binding.rootItem.get());
    }
}

void ImportAssetsCommand::undoRootMode()
{
    ProjectData* project = ProjectManager::instance().currentProject();

    for (ImportedEntry& entry : entries_) {
        if (project) {
            ProjectItem* rootItem = ProjectManager::instance().findItemByGuid(entry.itemGuid);
            if (rootItem && ProjectManager::detachRootBinding(project, rootItem, &entry.rootSlot)) {
                emit ProjectManager::instance().itemDeleted(entry.rootSlot.binding.rootItem.get());
            }
        }

    }

    qDebug() << "Undo import: rolled back" << entries_.size() << "root-level imported entries";
}


DeleteAssetsCommand::DeleteAssetsCommand(const QList<uint32_t>& sourceIDs,
                                         QUndoCommand* parent)
    : QUndoCommand(parent)
    , sourceIDs_(sourceIDs)
{
    if (sourceIDs.size() == 1) {
        const AssetInfo* info = AssetRegistry::instance().getAsset(sourceIDs.first());
        if (info) {
            setText(QObject::tr("Delete %1").arg(QFileInfo(info->relativePath).fileName()));
        } else {
            setText(QObject::tr("Delete asset"));
        }
    } else {
        setText(QObject::tr("Delete %1 assets").arg(sourceIDs.size()));
    }
}

DeleteAssetsCommand::~DeleteAssetsCommand() = default;

void DeleteAssetsCommand::redo()
{
    auto* project = ProjectManager::instance().currentProject();
    for (uint32_t id : sourceIDs_) {
        AssetInfo* info = AssetRegistry::instance().getAsset(id);
        if (info) {
            if (project) {
                if (auto* binding = findFileBindingByRootGuid(*project, info->ownerObjectGuid)) {
                    if (binding->rootItem) {
                        binding->rootItem->markDirty();
                    }
                }
            }

            AssetRegistry::instance().markAsOrphaned(id);
        }
    }

    qDebug() << "Delete: marked" << sourceIDs_.size() << "assets as orphaned";
}

void DeleteAssetsCommand::undo()
{
    auto* project = ProjectManager::instance().currentProject();
    for (uint32_t id : sourceIDs_) {
        AssetRegistry::instance().restoreOrphaned(id);

        AssetInfo* info = AssetRegistry::instance().getAsset(id);
        if (info && project) {
            if (auto* binding = findFileBindingByRootGuid(*project, info->ownerObjectGuid)) {
                if (binding->rootItem) {
                    binding->rootItem->markDirty();
                }
            }
        }
    }

    qDebug() << "Undo delete: restored" << sourceIDs_.size() << "assets";
}

}
