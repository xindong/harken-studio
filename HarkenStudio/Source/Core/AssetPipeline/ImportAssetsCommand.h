
#pragma once

#include "AssetTypes.h"
#include "../ProjectTypes.h"
#include "../UndoCommands.h"

#include <QUndoCommand>
#include <QStringList>
#include <QList>
#include <QUuid>
#include <cstdint>
#include <memory>

namespace VNX {

struct ProjectFileBinding;
struct ImportResult;
struct ProjectItem;
class CreateProjectItemCommand;

class ImportAssetsCommand : public TransactionalUndoCommand
{
public:
    ImportAssetsCommand(const QStringList& externalPaths,
                        ProjectFileBinding* targetBinding,
                        ProjectItem* targetParent = nullptr,
                        QUndoCommand* parent = nullptr);

    // Root-level mode: each file becomes its own independent root Sound
    // (Layers bucket), with no target binding/parent at all.
    explicit ImportAssetsCommand(const QStringList& externalPaths,
                                 QUndoCommand* parent = nullptr);

    ~ImportAssetsCommand() override;

    // Single construction+push chokepoint for every import entry (drag/drop,
    // menus, actions, IPC). Collects entries first and pushes only when at
    // least one file succeeded. Inspect successCount()/failCount()/importedIDs()
    // on the returned pointer immediately. A pushed command is owned by the
    // undo stack. A failed command is not pushed and is retained by this
    // module until process exit; do not delete it.
    static ImportAssetsCommand* pushImport(const QStringList& externalPaths,
                                           ProjectFileBinding* targetBinding,
                                           ProjectItem* targetParent = nullptr);

    // Same chokepoint for root-level imports (no target binding/parent).
    static ImportAssetsCommand* pushImportAtRoot(const QStringList& externalPaths,
                                                const QUuid& outputBusGuid = {});
    static ImportAssetsCommand* pushImportSources(const QStringList& externalPaths,
                                                const QString& sourceFolder = {},
                                                const QMap<QString, QString>& replacements = {});

    QList<uint32_t> importedIDs() const;
    QList<QUuid> importedItemGuids() const;

    int successCount() const { return successCount_; }

    int failCount() const { return failCount_; }

    int duplicateCount() const { return duplicateCount_; }

private:
    struct ImportedEntry {
        uint32_t sourceID = 0;
        QString relativePath;
        QString itemName;
        QUuid itemGuid;
        std::shared_ptr<CreateProjectItemCommand> itemCreation;

        // Root-mode only: the command holds the root binding slot itself (the
        // slot's shared_ptr becomes the sole owner once undo() detaches the
        // binding, so the object would otherwise be destroyed before redo()
        // can restore it).
        RootBindingSlot rootSlot;
    };

    ProjectItem* resolveParentForRedo() const;
    bool apply(bool redo) override;
    void redoEntries();
    void undoEntries();
    void collectParentEntries();
    void collectRootEntries();
    void redoRootMode();
    void undoRootMode();

    QStringList externalPaths_;
    ProjectFileBinding* targetBinding_ = nullptr;
    uint32_t targetParentObjectID_ = 0;
    bool rootMode_ = false;
    QUuid rootOutputBusGuid_;
    QString sourceFolder_;
    bool sourceMode_ = false;
    QSet<QString> replacedPaths_;
    QList<std::shared_ptr<TransactionalUndoCommand>> fileChanges_;

    QList<ImportedEntry> entries_;
    int successCount_ = 0;
    int failCount_ = 0;
    int duplicateCount_ = 0;

    bool firstRedo_ = true;
};

class DeleteAssetsCommand : public QUndoCommand
{
public:
    DeleteAssetsCommand(const QList<uint32_t>& sourceIDs,
                        QUndoCommand* parent = nullptr);

    ~DeleteAssetsCommand() override;

    void undo() override;
    void redo() override;

private:
    QList<uint32_t> sourceIDs_;
};

}
