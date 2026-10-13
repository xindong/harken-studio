#include "ImportSoundSourcesCommand.h"

#include "AssetImporter.h"
#include "AssetRegistry.h"
#include "../LanguageContextService.h"
#include "../PlatformContextService.h"
#include "../ProjectLanguageSemantics.h"
#include "../ProjectManager.h"
#include "../ProjectTypes.h"
#include "../SoundSourceEditing.h"
#include "../SoundSourceSemantics.h"
#include "../ToastManager.h"
#include "../UndoCommands.h"
#include "../../Widgets/LogPanel/LogPanel.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <memory>
#include <vector>

namespace VNX {

ImportSoundSourcesCommand::ImportSoundSourcesCommand(
    const QStringList& externalPaths,
    ProjectFileBinding* targetBinding,
    ProjectItem* targetSound,
    QString language,
    QUndoCommand* parent)
    : QUndoCommand(parent)
    , externalPaths_(externalPaths)
    , targetBinding_(targetBinding)
    , targetObjectID_(targetSound ? targetSound->objectID : 0)
    , language_(std::move(language))
{
    setText(externalPaths.size() == 1
        ? QObject::tr("Import source %1").arg(QFileInfo(externalPaths.first()).fileName())
        : QObject::tr("Import %1 sources").arg(externalPaths.size()));
}

ImportSoundSourcesCommand* ImportSoundSourcesCommand::pushImport(
    const QStringList& externalPaths,
    ProjectFileBinding* targetBinding,
    ProjectItem* targetSound,
    const QString& language)
{
    auto command = std::make_unique<ImportSoundSourcesCommand>(
        externalPaths, targetBinding, targetSound, language);
    command->collectEntries();
    if (command->successCount() == 0) {
        static std::vector<std::unique_ptr<ImportSoundSourcesCommand>> failed;
        ImportSoundSourcesCommand* raw = command.get();
        failed.push_back(std::move(command));
        return raw;
    }
    ImportSoundSourcesCommand* raw = command.release();
    UndoRedoManager::instance().push(raw);
    if (raw->duplicateCount() > 0) {
        ToastManager::instance().showToast(
            QObject::tr("%1 file(s) matched existing audio content and were reused")
                .arg(raw->duplicateCount()),
            5000, ToastType::Info);
    }
    return raw;
}

QList<uint32_t> ImportSoundSourcesCommand::importedIDs() const
{
    QList<uint32_t> result;
    result.reserve(static_cast<qsizetype>(entries_.size()));
    for (const ImportedEntry& entry : entries_)
        result.append(entry.sourceID);
    return result;
}

ProjectItem* ImportSoundSourcesCommand::resolveTarget() const
{
    ProjectData* project = ProjectManager::instance().currentProject();
    if (!project) return nullptr;
    ProjectItem* target = ProjectManager::instance().findItemByObjectID(targetObjectID_);
    if (!target || target->type != ProjectItem::Type::Sound) return nullptr;
    if (!targetBinding_ || findFileBindingForItem(*project, target) != targetBinding_)
        return nullptr;
    return target;
}

void ImportSoundSourcesCommand::notifyChanged(ProjectItem* target) const
{
    if (!target) return;
    SoundSourceEditing::syncActiveProjection(
        *target, PlatformContextService::instance().context().primary,
        SoundSourceEditing::previewLanguage(
            *target, LanguageContextService::instance().language()));
    target->markDirty();
    emit ProjectManager::instance().itemPropertyChanged(
        target, QStringLiteral("sources"));
    ProjectManager::resyncItemToRuntime(target);
}

void ImportSoundSourcesCommand::collectEntries()
{
    entries_.clear();
    successCount_ = 0;
    failCount_ = 0;
    duplicateCount_ = 0;

    ProjectItem* target = resolveTarget();
    auto* sound = target ? target->comp<VSound>() : nullptr;
    if (!sound) {
        failCount_ = externalPaths_.size();
        return;
    }

    localized_ = sound->distinguishLanguage;
    if (localized_) {
        if (language_.isEmpty())
            language_ = LanguageContextService::instance().language();
        const QStringList languages = ProjectLanguageSemantics::buildLanguages(
            *ProjectManager::instance().currentProject());
        if (!languages.contains(language_)
            || std::any_of(sound->sources.localized.keyBegin(), sound->sources.localized.keyEnd(),
                           [&languages](const QString& language) {
                               return !languages.contains(language);
                           })) {
            failCount_ = externalPaths_.size();
            LogPanel::logError(LogCategory::Import, QObject::tr(
                "Per-language sources must use project languages before importing."));
            return;
        }
    } else {
        language_.clear();
    }

    hadSourceSet_ = !localized_ || sound->sources.localized.contains(language_);
    previousSources_ = localized_ ? sound->sources.localized.value(language_) : sound->sources.plain;

    AssetImporter importer;
    importer.setProjectRoot(AssetRegistry::instance().projectRoot());
    for (const QString& path : externalPaths_) {
        const ImportResult result = importer.importFile(path, targetBinding_);
        if (!result.success) {
            ++failCount_;
            LogPanel::logError(LogCategory::Import, QString("Failed: %1 - %2")
                .arg(path, result.errorMessage));
            continue;
        }

        ImportedEntry entry;
        entry.sourceID = result.sourceID;
        entry.sourcePath = path;
        entry.destPath = result.absolutePath;
        entry.ownsAsset = !result.deduplicated;
        entry.source.entryId = QUuid::createUuid();
        entry.source.sourceId = result.sourceID;
        const AssetInfo* info = AssetRegistry::instance().getAsset(result.sourceID);
        if (entry.ownsAsset && info)
            entry.assetInfo = *info;
        if (info)
            entry.source.rangeContentHash = info->contentHash;
        if (const auto seed = SoundSourceSemantics::fileLoopRegion(info)) {
            entry.source.loopRegion = seed;
            entry.source.loopProvenance = SoundLoopProvenance::File;
        }
        entries_.push_back(entry);
        ++successCount_;
        if (result.deduplicated) ++duplicateCount_;
    }
}

void ImportSoundSourcesCommand::applyEntries(ProjectItem* target, VSound* sound)
{
    if (!target || !sound)
        return;
    auto& set = localized_ ? sound->sources.localized[language_] : sound->sources.plain;
    for (const ImportedEntry& entry : entries_) {
        const bool present = std::any_of(
            set.entries.begin(), set.entries.end(),
            [&entry](const SoundSourceDef& source) {
                return source.entryId == entry.source.entryId;
            });
        if (!present) set.entries.push_back(entry.source);
    }
    if (previousSources_.entries.empty() && !entries_.empty())
        set.active = entries_.front().source.entryId;
    notifyChanged(target);
}

void ImportSoundSourcesCommand::redo()
{
    ProjectItem* target = resolveTarget();
    auto* sound = target ? target->comp<VSound>() : nullptr;
    if (!sound) {
        if (firstRedo_) failCount_ = externalPaths_.size();
        firstRedo_ = false;
        return;
    }

    if (firstRedo_) {
        if (entries_.empty() && successCount_ == 0 && failCount_ == 0)
            collectEntries();
        if (!entries_.empty())
            applyEntries(target, sound);
        firstRedo_ = false;
        return;
    }

    for (const ImportedEntry& entry : entries_) {
        if (entry.ownsAsset && !AssetRegistry::instance().contains(entry.sourceID)) {
            if (!QFileInfo::exists(entry.destPath) && !entry.sourcePath.isEmpty())
                QFile::copy(entry.sourcePath, entry.destPath);
            AssetRegistry::instance().registerAsset(entry.assetInfo);
        }
    }
    applyEntries(target, sound);
}

void ImportSoundSourcesCommand::undo()
{
    ProjectItem* target = resolveTarget();
    auto* sound = target ? target->comp<VSound>() : nullptr;
    if (!sound) return;

    if (localized_) {
        if (hadSourceSet_) sound->sources.localized[language_] = previousSources_;
        else sound->sources.localized.remove(language_);
    } else {
        sound->sources.plain = previousSources_;
    }

    for (const ImportedEntry& entry : entries_) {
        if (entry.ownsAsset && AssetRegistry::instance().isUncommitted(entry.sourceID)) {
            if (!entry.destPath.isEmpty() && QFileInfo::exists(entry.destPath))
                QFile::remove(entry.destPath);
            AssetRegistry::instance().unregisterAsset(entry.sourceID);
        }
    }
    notifyChanged(target);
}

}
