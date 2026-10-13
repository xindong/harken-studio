#pragma once

#include "AssetTypes.h"
#include "../Components/VSound.h"

#include <QList>
#include <QStringList>
#include <QUndoCommand>

#include <optional>

namespace VNX {

struct ProjectFileBinding;
struct ProjectItem;

class ImportSoundSourcesCommand : public QUndoCommand
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
                                                  const QString& language = {});

    void undo() override;
    void redo() override;

    QList<uint32_t> importedIDs() const;
    int successCount() const { return successCount_; }
    int failCount() const { return failCount_; }
    int duplicateCount() const { return duplicateCount_; }

private:
    struct ImportedEntry {
        SoundSourceDef source;
        uint32_t sourceID = 0;
        QString sourcePath;
        QString destPath;
        bool ownsAsset = false;
        AssetInfo assetInfo;
    };

    ProjectItem* resolveTarget() const;
    void notifyChanged(ProjectItem* target) const;
    void collectEntries();
    void applyEntries(ProjectItem* target, VSound* sound);

    QStringList externalPaths_;
    ProjectFileBinding* targetBinding_ = nullptr;
    uint32_t targetObjectID_ = 0;
    QString language_;
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
