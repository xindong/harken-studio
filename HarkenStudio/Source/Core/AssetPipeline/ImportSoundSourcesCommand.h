#pragma once

#include "AssetTypes.h"
#include "../Components/VSound.h"
#include "../UndoCommands.h"

#include <QList>
#include <QStringList>
#include <QUndoCommand>

#include <optional>

namespace VNX {

struct ProjectFileBinding;
struct ProjectItem;

class ImportSoundSourcesCommand : public TransactionalUndoCommand
{
public:
    ImportSoundSourcesCommand(const QStringList& externalPaths,
                              ProjectFileBinding* targetBinding,
                              ProjectItem* targetSound,
                              QString language = {},
                              QUndoCommand* parent = nullptr);

    static ImportSoundSourcesCommand* pushImport(const QStringList& externalPaths,
                                                  ProjectFileBinding* targetBinding,
                                                  ProjectItem* targetSound,
                                                  const QString& language = {},
                                                  const QUuid& targetEntry = {});

    QList<uint32_t> importedIDs() const;
    int successCount() const { return successCount_; }
    int failCount() const { return failCount_; }
    int duplicateCount() const { return duplicateCount_; }

private:
    struct ImportedEntry {
        SoundSourceDef source;
        uint32_t sourceID = 0;
    };

    ProjectItem* resolveTarget() const;
    void notifyChanged(ProjectItem* target) const;
    void collectEntries();
    void applyEntries(ProjectItem* target, VSound* sound);
    bool apply(bool redo) override;
    void redoEntries();
    void undoEntries();

    QStringList externalPaths_;
    ProjectFileBinding* targetBinding_ = nullptr;
    uint32_t targetObjectID_ = 0;
    QString language_;
    QUuid targetEntry_;
    QList<std::shared_ptr<TransactionalUndoCommand>> fileChanges_;
    std::vector<ImportedEntry> entries_;
    bool localized_ = false;
    bool hadSourceSet_ = false;
    SoundSourceSet previousSources_;
    int successCount_ = 0;
    int failCount_ = 0;
    int duplicateCount_ = 0;
    bool firstRedo_ = true;
};

}
