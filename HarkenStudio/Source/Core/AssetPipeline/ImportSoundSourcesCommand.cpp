#include "ImportSoundSourcesCommand.h"

#include "AssetImporter.h"
#include "AssetRegistry.h"
#include "SourceFileSystem.h"
#include "../LanguageContextService.h"
#include "../PlatformContextService.h"
#include "../ProjectLanguageSemantics.h"
#include "../ProjectManager.h"
#include "../ProjectTypes.h"
#include "../SoundSourceEditing.h"
#include "../SoundSourceSemantics.h"
#include "../UndoCommands.h"
#include "../../Widgets/LogPanel/LogPanel.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

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
    : TransactionalUndoCommand({}, parent, false)
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
    const QString& language,
    const QUuid& targetEntry)
{
    auto command = std::make_unique<ImportSoundSourcesCommand>(
        externalPaths, targetBinding, targetSound, language);
    command->targetEntry_ = targetEntry;
    command->collectEntries();
    if (command->successCount() == 0) {
        static std::vector<std::unique_ptr<ImportSoundSourcesCommand>> failed;
        ImportSoundSourcesCommand* raw = command.get();
        failed.push_back(std::move(command));
        return raw;
    }
    ImportSoundSourcesCommand* raw = command.release();
    UndoRedoManager::instance().push(raw);
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
    uint32_t explicitSource = 0;
    if (!targetEntry_.isNull()) {
        const auto locate = [&](const SoundSourceSet& set, const QString& language) {
            for (const auto& source : set.entries) {
                if (source.entryId == targetEntry_) {
                    explicitSource = source.sourceId;
                    language_ = language;
                    return;
                }
            }
        };
        if (localized_) {
            for (auto it = sound->sources.localized.cbegin(); it != sound->sources.localized.cend(); ++it)
                locate(it.value(), it.key());
        } else {
            locate(sound->sources.plain, {});
        }
        if (explicitSource == 0) {
            failCount_ = externalPaths_.size();
            return;
        }
        if (AssetRegistry::instance().sourceFileExists(explicitSource))
            explicitSource = 0;
    }
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

    auto& registry = AssetRegistry::instance();
    const auto sameName = [](const QString& path, const QString& other) {
        return QFileInfo(path).fileName().compare(QFileInfo(other).fileName(),
#ifdef Q_OS_WIN
            Qt::CaseInsensitive
#else
            Qt::CaseSensitive
#endif
        ) == 0;
    };
    const auto report = [](const QString& error) {
        LogPanel::logError(LogCategory::Import, error);
    };
    struct PlannedImport { QString path; uint32_t restoreID = 0; };
    QList<PlannedImport> plan;
    QMap<uint32_t, QString> versions;
    QSet<uint32_t> conflicts;
    QStringList files = externalPaths_;
    files.removeDuplicates();
    for (const QString& path : files) {
        QSet<uint32_t> candidates;
        const auto* explicitAsset = registry.getAsset(explicitSource);
        if (explicitAsset && sameName(path, explicitAsset->relativePath)) {
            candidates.insert(explicitSource);
        } else {
            for (const auto& source : previousSources_.entries) {
                const auto* asset = registry.getAsset(source.sourceId);
                if (asset && !registry.sourceFileExists(source.sourceId)
                    && sameName(path, asset->relativePath))
                    candidates.insert(source.sourceId);
            }
        }
        if (candidates.size() > 1) {
            ++failCount_;
            report(QObject::tr("Several missing sources are named '%1'. Drop the file onto the source row to restore.")
                .arg(QFileInfo(path).fileName()));
            continue;
        }
        const uint32_t restoreID = candidates.isEmpty() ? 0 : *candidates.cbegin();
        if (restoreID != 0) {
            const QString hash = AssetImporter::computeContentHash(path);
            if (versions.contains(restoreID)) {
                if (!hash.isEmpty() && hash == versions.value(restoreID))
                    continue;
                conflicts.insert(restoreID);
            } else {
                versions.insert(restoreID, hash);
            }
        }
        plan.append({path, restoreID});
    }
    for (uint32_t id : conflicts)
        report(QObject::tr("Several files target '%1'. Import one version at a time.")
            .arg(registry.getAsset(id)->relativePath));

    AssetImporter importer;
    importer.setProjectRoot(AssetRegistry::instance().projectRoot());
    for (const auto& planned : plan) {
        if (conflicts.contains(planned.restoreID)) {
            ++failCount_;
            continue;
        }
        const QString& path = planned.path;
        const ImportResult result = importer.importFile(path, targetBinding_, {}, true, planned.restoreID);
        if (!result.success) {
            ++failCount_;
            LogPanel::logError(LogCategory::Import, QString("Failed: %1 - %2")
                .arg(path, result.errorMessage));
            continue;
        }

        ImportedEntry entry;
        entry.sourceID = result.sourceID;
        if (planned.restoreID == 0)
            entry.source.entryId = QUuid::createUuid();
        entry.source.sourceId = result.sourceID;
        const AssetInfo* info = AssetRegistry::instance().getAsset(result.sourceID);
        if (info)
            entry.source.rangeContentHash = info->contentHash;
        if (const auto seed = SoundSourceSemantics::fileLoopRegion(info)) {
            entry.source.loopRegion = seed;
            entry.source.loopProvenance = SoundLoopProvenance::File;
        }
        entries_.push_back(entry);
        if (result.fileChange)
            fileChanges_.append(result.fileChange);
        ++successCount_;
        if (result.deduplicated && !result.fileChange) ++duplicateCount_;
    }
}

void ImportSoundSourcesCommand::applyEntries(ProjectItem* target, VSound* sound)
{
    if (!target || !sound)
        return;
    auto& set = localized_ ? sound->sources.localized[language_] : sound->sources.plain;
    for (const ImportedEntry& entry : entries_) {
        if (entry.source.entryId.isNull())
            continue;
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

bool ImportSoundSourcesCommand::apply(bool redo)
{
    if (!resolveTarget())
        return false;
    bool rollbackFailed = false;
    if (!SourceFileSystem::replayFiles(fileChanges_, redo, &rollbackFailed)) {
        setObsolete(rollbackFailed);
        return false;
    }
    if (redo) redoEntries();
    else undoEntries();
    return true;
}

void ImportSoundSourcesCommand::redoEntries()
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

    applyEntries(target, sound);
}

void ImportSoundSourcesCommand::undoEntries()
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

    notifyChanged(target);
}

}
