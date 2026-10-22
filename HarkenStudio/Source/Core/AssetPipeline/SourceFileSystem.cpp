#include "SourceFileSystem.h"

#include "AssetRegistry.h"
#include "AssetImporter.h"
#include "MediaPool.h"
#include "PathUtils.h"
#include "WaveformCache.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "../ProjectDataPolicy.h"
#include "../NameValidator.h"
#include "../ProjectManager.h"
#include "../LanguageContextService.h"
#include "../PlatformContextService.h"
#include "../SoundSourceEditing.h"
#include "../UndoCommands.h"
#include "../../Widgets/LogPanel/LogPanel.h"
#include "Utils/FileOperationRetry.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QDebug>
#include <QScopeGuard>

#include <algorithm>

namespace VNX::SourceFileSystem {
namespace {

struct FileRevision {
    std::shared_ptr<QTemporaryDir> storage;
    QString file;
    AudioMetadata metadata;
    QString hash;
};

struct Change {
    QString from;
    QString to;
    bool directory = false;
    std::shared_ptr<FileRevision> before = {};
    std::shared_ptr<FileRevision> after = {};

    Change reversed() const { return {to, from, directory, after, before}; }
};

QList<Change> pendingChanges;
QList<Change> discardRollbackChanges;

enum class ApplyMode { Record, Transient, Discard };

QString pathKey(const QString& path)
{
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
}

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

void syncSoundPaths()
{
    auto& manager = ProjectManager::instance();
    const auto* project = manager.currentProject();
    if (!project)
        return;
    const auto visit = [&](auto&& self, ProjectItem* item) -> void {
        if (item->type == ProjectItem::Type::Sound) {
            const auto* asset = AssetRegistry::instance().getAsset(item->sourceID);
            if (asset && item->sourcePath != asset->relativePath) {
                item->sourcePath = asset->relativePath;
                item->markDirty();
                emit manager.itemPropertyChanged(item, QStringLiteral("path"));
                ProjectManager::resyncItemToRuntime(item);
            }
        }
        for (const auto& child : item->children)
            self(self, child.get());
    };
    for (const auto* binding : project->allFileBindings())
        if (binding->rootItem)
            visit(visit, binding->rootItem.get());
}

bool applyFileChange(const Change& change, QString* error)
{
    const QString from = absolutePath(change.from);
    const QString to = absolutePath(change.to);
    if (from.isEmpty() || to.isEmpty())
        return fail(error, QObject::tr("The source path must stay inside AudioSources."));

    if (change.after && !change.after->file.isEmpty())
        return copyFile(change.after->file, to, error);
    const bool ok = retryFileOperation([&] {
        if (change.after)
            return !QFileInfo::exists(to) || QFile::remove(to);
        if (change.from.isEmpty())
            return QDir().mkdir(to);
        if (change.to.isEmpty())
            return QDir().rmdir(from);
        return QDir().rename(from, to);
    });
    if (!ok)
        return fail(error, QObject::tr("Could not update '%1'. Check for a name conflict or a file in use.")
            .arg(change.from.isEmpty() ? change.to : change.from));

    return true;
}

void publishChanges(const QList<Change>& changes, bool record)
{
    auto& registry = AssetRegistry::instance();
    for (const Change& change : changes) {
        const auto* asset = change.directory ? nullptr : registry.getAssetByPath(change.from);
        const bool retain = change.directory || (change.after && !asset)
            || (asset && !registry.isUncommitted(asset->sourceID));
        if (record && retain)
            pendingChanges.append(change);
        if (change.after) {
            const QString path = absolutePath(change.to);
            WaveformCache::remove(path);
            if (asset) {
                const uint32_t sourceID = asset->sourceID;
                registry.invalidateAllCaches(sourceID);
                if (!registry.updateSourceFile(sourceID, change.after->metadata, path,
                    change.after->hash, QFileInfo(path).lastModified().toSecsSinceEpoch()))
                    emit registry.assetModified(sourceID);
                HarkenEditor::Bridge::mediaRegistryRegister(sourceID, path);
                emit MediaPool::instance().mediaHotSwapped(sourceID, path);
                if (!change.after->file.isEmpty())
                    MediaPool::instance().refreshWaveform(sourceID);
            }
            if (!asset && !change.after->file.isEmpty())
                WaveformCache::generateAsync(path);
        } else if (!change.from.isEmpty() && !change.to.isEmpty())
            registry.relocateSourcePaths(change.from, change.to);
    }
    syncSoundPaths();
    emit registry.sourceFoldersChanged();
}

bool validateSourcePaths(const QList<Change>& changes, QString* error)
{
    QSet<QString> destinations;
    for (const auto& asset : AssetRegistry::instance().snapshot()) {
        QString path = asset.relativePath;
        bool relocated = false;
        for (const auto& change : changes) {
            if (change.after || change.from.isEmpty() || change.to.isEmpty()) continue;
            const QString from = pathKey(change.from);
            if (pathKey(path) == from || (change.directory && pathKey(path).startsWith(from + QLatin1Char('/')))) {
                path = change.to + path.mid(change.from.size());
                relocated = true;
            }
        }
        const QString key = pathKey(path);
        const bool occupied = !relocated && std::any_of(changes.cbegin(), changes.cend(), [&](const Change& change) {
            return !change.after && !change.to.isEmpty() && pathKey(change.to) == key;
        });
        if (occupied || destinations.contains(key))
            return fail(error, QObject::tr("'%1' is still used by another source record. Restore that source or choose another name.")
                .arg(path));
        destinations.insert(key);
    }
    return true;
}

bool applyBatch(const QList<Change>& changes, ApplyMode mode, QString* error, bool* rollbackFailed = nullptr)
{
    if (mode != ApplyMode::Discard && !recoverDiscard(error)) return false;
    if (mode != ApplyMode::Discard && !validateSourcePaths(changes, error)) return false;
    auto& mediaPool = MediaPool::instance();
    mediaPool.beginSourceFileChange();
    const auto resumeReaders = qScopeGuard([&] { mediaPool.endSourceFileChange(); });
    QList<Change> steps;
    for (const Change& change : changes) {
        if (change.from != change.to && pathKey(change.from) == pathKey(change.to)) {
            const int slash = change.from.lastIndexOf(QLatin1Char('/'));
            const QString intermediate = change.from.left(slash + 1)
                + QStringLiteral(".rename-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            steps.append({change.from, intermediate, change.directory});
            steps.append({intermediate, change.to, change.directory});
        } else {
            steps.append(change);
        }
    }
    QList<Change> applied;
    for (const Change& step : steps) {
        QString failure;
        if (!applyFileChange(step, &failure)) {
            QList<Change> remaining = applied;
            QStringList rollbackErrors;
            for (auto it = applied.crbegin(); it != applied.crend(); ++it) {
                QString rollbackError;
                if (!applyFileChange(it->reversed(), &rollbackError)) {
                    rollbackErrors.append(rollbackError);
                    break;
                }
                remaining.removeLast();
            }
            if (!remaining.isEmpty()) {
                if (mode == ApplyMode::Discard)
                    discardRollbackChanges = remaining;
                else
                    publishChanges(remaining, mode == ApplyMode::Record);
                if (rollbackFailed)
                    *rollbackFailed = true;
                failure += QLatin1Char('\n') + QObject::tr(
                    "Some source changes could not be restored. Undo history has been cleared.\n%1")
                    .arg(rollbackErrors.join(QLatin1Char('\n')));
            }
            return fail(error, failure);
        }
        applied.append(step);
    }
    if (mode != ApplyMode::Discard)
        publishChanges(changes, mode == ApplyMode::Record);
    return true;
}

class FileCommand : public TransactionalUndoCommand
{
public:
    FileCommand(const QList<Change>& changes, const QString& label)
        : TransactionalUndoCommand(label), changes_(changes) {}

private:
    bool apply(bool redo) override
    {
        QList<Change> changes;
        if (redo) {
            changes = changes_;
        } else {
            for (auto it = changes_.crbegin(); it != changes_.crend(); ++it)
                changes.append(it->reversed());
        }
        QString error;
        bool rollbackFailed = false;
        if (applyBatch(changes, ApplyMode::Record, &error, &rollbackFailed))
            return true;
        setObsolete(rollbackFailed);
        LogPanel::logError(LogCategory::File, error);
        return false;
    }

    QList<Change> changes_;
};

class ImportCommand : public TransactionalUndoCommand
{
public:
    ImportCommand(const AssetInfo& asset, const QList<Change>& changes)
        : TransactionalUndoCommand(QObject::tr("Import audio source")), asset_(asset), changes_(changes) {}

private:
    bool apply(bool redo) override
    {
        auto& registry = AssetRegistry::instance();
        QString error;
        if (!recoverDiscard(&error)) {
            LogPanel::logError(LogCategory::File, error);
            return false;
        }
        if (redo && !removed_) return true;
        if (!redo && !registry.isUncommitted(asset_.sourceID)) return true;
        if (redo && (registry.contains(asset_.sourceID)
            || registry.findAssetByPath(asset_.absolutePath) || QFileInfo::exists(asset_.absolutePath))) {
            error = QObject::tr("Cannot restore '%1': the source ID or path is already in use.")
                .arg(asset_.relativePath);
        } else {
            QList<Change> changes;
            if (redo) changes = changes_;
            else {
                if (const auto* current = registry.getAsset(asset_.sourceID))
                    asset_.ownerObjectGuid = current->ownerObjectGuid;
                for (auto it = changes_.crbegin(); it != changes_.crend(); ++it)
                    changes.append(it->reversed());
            }
            bool rollbackFailed = false;
            if (applyBatch(changes, ApplyMode::Transient, &error, &rollbackFailed)) {
                if (!redo) {
                    registry.unregisterAsset(asset_.sourceID);
                    removed_ = true;
                    return true;
                }
                if (registry.registerAsset(asset_)) {
                    removed_ = false;
                    MediaPool::instance().refreshWaveform(asset_.sourceID);
                    return true;
                }
                QList<Change> rollback;
                for (auto it = changes.crbegin(); it != changes.crend(); ++it)
                    rollback.append(it->reversed());
                rollbackFailed = !applyBatch(rollback, ApplyMode::Transient, nullptr);
                error = QObject::tr("Could not restore the source record for '%1'.").arg(asset_.relativePath);
            }
            setObsolete(rollbackFailed);
        }
        LogPanel::logError(LogCategory::File, error);
        return false;
    }

    AssetInfo asset_;
    QList<Change> changes_;
    bool removed_ = false;
};

class RestoreByRenameCommand : public TransactionalUndoCommand
{
public:
    RestoreByRenameCommand(const AssetInfo& source, const AssetInfo& missing)
        : TransactionalUndoCommand(QObject::tr("Rename and restore audio source"), nullptr, false)
        , source_(source), missing_(missing)
        , missingUncommitted_(AssetRegistry::instance().isUncommitted(missing.sourceID))
    {
        const auto collect = [&](auto&& self, ProjectItem* item) -> void {
            if (const auto* sound = item->comp<VSound>()) {
                Reference reference{item->guid, sound->sources, sound->sources};
                const auto replace = [&](SoundSourceSet& set) {
                    for (auto& entry : set.entries)
                        if (entry.sourceId == missing_.sourceID) entry.sourceId = source_.sourceID;
                };
                replace(reference.after.plain);
                for (auto& set : reference.after.localized) replace(set);
                if (reference.before != reference.after) references_.append(reference);
            }
            for (const auto& child : item->children) self(self, child.get());
        };
        for (const auto* binding : ProjectManager::instance().currentProject()->allFileBindings())
            if (binding->rootItem) collect(collect, binding->rootItem.get());
    }

private:
    bool apply(bool redo) override
    {
        auto& registry = AssetRegistry::instance();
        auto& manager = ProjectManager::instance();
        QString error;
        if (!recoverDiscard(&error)) {
            LogPanel::logError(LogCategory::File, error);
            return false;
        }
        const auto* source = registry.getAsset(source_.sourceID);
        const auto* missing = registry.getAsset(missing_.sourceID);
        const QString from = redo ? source_.relativePath : missing_.relativePath;
        const QString to = redo ? missing_.relativePath : source_.relativePath;
        if (!source || source->relativePath != from || !registry.sourceFileExists(source_.sourceID)
            || QFileInfo::exists(absolutePath(to)) || (redo && (!missing || missing->relativePath != to))
            || (!redo && missing)
            || std::any_of(references_.cbegin(), references_.cend(), [&](const Reference& ref) {
                const auto* item = manager.findItemByGuid(ref.guid);
                return !item || !item->comp<VSound>();
            })) {
            error = QObject::tr("The sources changed before the rename could be applied. Review them and try again.");
        } else {
            if (redo) registry.unregisterAsset(missing_.sourceID);
            bool rollbackFailed = false;
            if (applyBatch({{from, to, false}}, ApplyMode::Record, &error, &rollbackFailed)) {
                if (!redo && !registry.registerAsset(missing_, missingUncommitted_)) {
                    rollbackFailed = !applyBatch({{to, from, false}}, ApplyMode::Record, nullptr);
                    error = QObject::tr("Could not restore the missing source record.");
                } else {
                    for (const auto& ref : references_) {
                        auto* item = manager.findItemByGuid(ref.guid);
                        item->comp<VSound>()->sources = redo ? ref.after : ref.before;
                        SoundSourceEditing::syncActiveProjection(*item,
                            PlatformContextService::instance().context().primary,
                            SoundSourceEditing::previewLanguage(*item, LanguageContextService::instance().language()));
                        item->markDirty();
                        emit manager.itemPropertyChanged(item, QStringLiteral("sources"));
                        ProjectManager::resyncItemToRuntime(item);
                    }
                    MediaPool::instance().refreshSourceFile(source_.sourceID, absolutePath(to));
                    return true;
                }
            } else if (redo && !registry.registerAsset(missing_, missingUncommitted_)) {
                rollbackFailed = true;
            }
            setObsolete(rollbackFailed);
        }
        LogPanel::logError(LogCategory::File, error);
        return false;
    }

    struct Reference { QUuid guid; SoundSources before; SoundSources after; };
    AssetInfo source_;
    AssetInfo missing_;
    bool missingUncommitted_;
    QList<Reference> references_;
};

bool push(const QList<Change>& changes, const QString& label, QString* error)
{
    if (changes.isEmpty())
        return false;
    bool rollbackFailed = false;
    if (!applyBatch(changes, ApplyMode::Record, error, &rollbackFailed)) {
        if (rollbackFailed) {
            UndoRedoManager::instance().clear();
            UndoRedoManager::instance().undoStack()->resetClean();
        }
        return false;
    }
    UndoRedoManager::instance().push(new FileCommand(changes, label));
    return true;
}

bool validateMove(const QString& from, const QString& to, QString* error)
{
    if (from.isEmpty() || to.isEmpty() || absolutePath(from).isEmpty() || absolutePath(to).isEmpty())
        return fail(error, QObject::tr("The source path must stay inside AudioSources."));
    if (!QFileInfo::exists(absolutePath(from)))
        return fail(error, QObject::tr("The selected source no longer exists."));
    if (!QFileInfo(absolutePath(to)).dir().exists())
        return fail(error, QObject::tr("The destination folder no longer exists."));
    if (pathKey(to).startsWith(pathKey(from) + QLatin1Char('/')))
        return fail(error, QObject::tr("A folder cannot be moved into itself."));
    if (pathKey(from) != pathKey(to) && QFileInfo::exists(absolutePath(to)))
        return fail(error, QObject::tr("'%1' already exists.").arg(to));
    return validateSourcePaths({{from, to, QFileInfo(absolutePath(from)).isDir()}}, error);
}

}

QString absolutePath(const QString& relativePath)
{
    if (!ProjectManager::instance().isProjectOpen())
        return {};
    const QString root = AssetRegistry::instance().projectRoot();
    if (root.isEmpty())
        return {};
    const QString audioRoot = PathUtils::getAudioSourcesDir(root);
    if (QFileInfo(audioRoot).isSymLink())
        return {};
    if (relativePath.isEmpty())
        return audioRoot;
    if (!ProjectDataPolicy::isSafeRelativePath(relativePath)
        || ProjectDataPolicy::normalizeRelativePath(relativePath) != relativePath
        || PathUtils::hasUnsafeCharacters(relativePath))
        return {};
    QString current = audioRoot;
    for (const QString& segment : relativePath.split(QLatin1Char('/'))) {
        current = QDir(current).filePath(segment);
        if (QFileInfo(current).isSymLink())
            return {};
    }
    return current;
}

bool validName(const QString& name)
{
    return !name.isEmpty() && name == name.trimmed() && name != QLatin1String(".")
        && name != QLatin1String("..") && !name.endsWith(QLatin1Char('.'))
        && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && !PathUtils::hasUnsafeCharacters(name);
}

bool createFolder(const QString& parentPath, const QString& name, QString* error, QString* createdName)
{
    if (!recoverDiscard(error)) return false;
    if (!validName(name))
        return fail(error, QObject::tr("Enter a valid folder name."));
    const QString prefix = parentPath.isEmpty() ? QString() : parentPath + QLatin1Char('/');
    if (absolutePath(parentPath).isEmpty() || !QFileInfo(absolutePath(parentPath)).isDir())
        return fail(error, QObject::tr("The destination folder no longer exists."));
    const QString uniqueName = NameValidator::makeUniqueName(name, [&](const QString& candidate) {
        return !QFileInfo::exists(absolutePath(prefix + candidate));
    }, 255);
    const QString path = prefix + uniqueName;
    const QString absolute = absolutePath(path);
    if (absolute.isEmpty() || !QFileInfo(absolute).dir().exists())
        return fail(error, QObject::tr("The destination folder no longer exists."));
    if (!push({{{}, path, true}}, QObject::tr("Create source folder %1").arg(uniqueName), error))
        return false;
    if (createdName) *createdName = uniqueName;
    return true;
}

bool validateRename(const QString& path, const QString& name, QString* error)
{
    if (error) error->clear();
    if (!validName(name))
        return fail(error, QObject::tr("Enter a valid file or folder name."));
    if (absolutePath(path).isEmpty() || !QFileInfo::exists(absolutePath(path)))
        return fail(error, QObject::tr("The selected source no longer exists."));
    const bool directory = QFileInfo(absolutePath(path)).isDir();
    if (!directory && QFileInfo(path).suffix().compare(QFileInfo(name).suffix(), Qt::CaseInsensitive) != 0)
        return fail(error, QObject::tr("Keep the original audio file extension."));
    return true;
}

bool rename(const QString& path, const QString& name, QString* error, QString* renamedName)
{
    if (!recoverDiscard(error)) return false;
    if (!validateRename(path, name, error)) return false;
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const QString prefix = slash < 0 ? QString() : path.left(slash + 1);
    const bool directory = QFileInfo(absolutePath(path)).isDir();
    const QString suffix = QFileInfo(name).suffix();
    const QString extension = directory || suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix;
    const QString stem = directory ? name : name.left(name.size() - extension.size());
    const QString uniqueName = NameValidator::makeUniqueName(stem, [&](const QString& candidate) {
        const QString destination = prefix + candidate + extension;
        return pathKey(destination) == pathKey(path) || !QFileInfo::exists(absolutePath(destination));
    }, 255 - extension.size()) + extension;
    const QString destination = prefix + uniqueName;
    if (destination == path) return false;
    if (!validateMove(path, destination, error)) return false;
    if (!push({{path, destination, directory}}, QObject::tr("Rename source %1").arg(path), error))
        return false;
    if (renamedName) *renamedName = uniqueName;
    return true;
}

bool restoreByRename(uint32_t sourceID, uint32_t missingID, QString* error)
{
    if (!recoverDiscard(error)) return false;
    auto& registry = AssetRegistry::instance();
    const auto* source = registry.getAsset(sourceID);
    const auto* missing = registry.getAsset(missingID);
    if (sourceID == missingID || !source || !missing || !registry.sourceFileExists(sourceID)
        || registry.sourceFileExists(missingID) || absolutePath(source->relativePath).isEmpty()
        || absolutePath(missing->relativePath).isEmpty())
        return fail(error, QObject::tr("The source or missing destination changed. Review them and try again."));
    auto command = std::make_unique<RestoreByRenameCommand>(*source, *missing);
    if (!command->tryReplay(true)) {
        if (command->isObsolete()) {
            UndoRedoManager::instance().clear();
            UndoRedoManager::instance().undoStack()->resetClean();
        }
        return fail(error, QObject::tr("Could not rename and restore the source."));
    }
    UndoRedoManager::instance().push(command.release());
    return true;
}

static QList<Change> planMove(const QStringList& paths, const QString& parentPath, QString* error)
{
    if (absolutePath(parentPath).isEmpty() || !QFileInfo(absolutePath(parentPath)).isDir()) {
        fail(error, QObject::tr("The destination folder no longer exists."));
        return {};
    }
    QStringList roots = paths;
    roots.removeDuplicates();
    std::sort(roots.begin(), roots.end());
    QList<Change> changes;
    QSet<QString> destinations;
    for (const QString& path : roots) {
        if (std::any_of(roots.cbegin(), roots.cend(), [&](const QString& ancestor) {
                return path != ancestor && pathKey(path).startsWith(pathKey(ancestor) + QLatin1Char('/'));
            }))
            continue;
        const QString name = QFileInfo(path).fileName();
        const QString destination = parentPath.isEmpty() ? name : parentPath + QLatin1Char('/') + name;
        if (destination == path)
            continue;
        if (!validateMove(path, destination, error))
            return {};
        if (destinations.contains(pathKey(destination))) {
            fail(error, QObject::tr("Multiple sources have the same destination name."));
            return {};
        }
        destinations.insert(pathKey(destination));
        changes.append({path, destination, QFileInfo(absolutePath(path)).isDir()});
    }
    return changes;
}

bool canMove(const QStringList& paths, const QString& parentPath)
{
    return !planMove(paths, parentPath, nullptr).isEmpty();
}

bool move(const QStringList& paths, const QString& parentPath, QString* error)
{
    if (!recoverDiscard(error)) return false;
    return push(planMove(paths, parentPath, error), QObject::tr("Move sources"), error);
}

bool removeEmptyFolder(const QString& path, QString* error)
{
    if (!recoverDiscard(error)) return false;
    const QString absolute = absolutePath(path);
    if (path.isEmpty() || absolute.isEmpty() || !QFileInfo(absolute).isDir() || !QDir(absolute).isEmpty())
        return fail(error, QObject::tr("Only empty source folders can be deleted."));
    return push({{path, {}, true}}, QObject::tr("Delete source folder %1").arg(path), error);
}

std::unique_ptr<TransactionalUndoCommand> replaceFiles(
    const QMap<QString, QString>& replacements, QString* error)
{
    if (!recoverDiscard(error)) return {};
    if (replacements.isEmpty())
        return {};
    auto& mediaPool = MediaPool::instance();
    mediaPool.beginSourceFileChange();
    const auto resumeReaders = qScopeGuard([&] { mediaPool.endSourceFileChange(); });
    const auto storage = std::make_shared<QTemporaryDir>(
        QDir(PathUtils::getDotCacheDir(AssetRegistry::instance().projectRoot()))
            .filePath(QStringLiteral("source-import-XXXXXX")));
    if (!storage->isValid()) {
        fail(error, QObject::tr("Could not keep a copy of the audio files for undo."));
        return {};
    }

    AssetImporter importer;
    QList<Change> changes;
    for (auto it = replacements.cbegin(); it != replacements.cend(); ++it) {
        const QString destination = absolutePath(it.key());
        if (destination.isEmpty() || QFileInfo(destination).isDir()) {
            fail(error, QObject::tr("The destination source path is invalid."));
            return {};
        }
        auto before = std::make_shared<FileRevision>();
        auto after = std::make_shared<FileRevision>();
        before->storage = after->storage = storage;
        if (const auto* asset = AssetRegistry::instance().getAssetByPath(it.key())) {
            before->metadata = asset->metadata;
            before->hash = asset->contentHash;
        }
        const auto capture = [&](const QString& source, const QString& name,
                                 FileRevision& revision) {
            revision.file = storage->filePath(name + QLatin1Char('.') + QFileInfo(source).suffix());
            QFile input(source);
            if (!input.copy(revision.file))
                return fail(error, QObject::tr("Could not prepare '%1' for replacement: %2 (file error %3).")
                    .arg(source, input.errorString()).arg(static_cast<int>(input.error())));
            revision.metadata = importer.analyzeFile(revision.file);
            revision.hash = AssetImporter::computeContentHash(revision.file);
            return !revision.hash.isEmpty()
                || fail(error, QObject::tr("Could not read '%1'.").arg(source));
        };
        if (QFileInfo::exists(destination)
            && !capture(destination, QString::number(changes.size()) + QStringLiteral("-before"), *before))
            return {};
        if (!capture(it.value(), QString::number(changes.size()) + QStringLiteral("-after"), *after))
            return {};
        if (after->metadata.sampleRate == 0 || after->metadata.totalSamples == 0) {
            fail(error, QObject::tr("Could not read audio data from '%1'.").arg(it.value()));
            return {};
        }
        changes.append({it.key(), it.key(), false, before, after});
    }
    bool rollbackFailed = false;
    if (!applyBatch(changes, ApplyMode::Record, error, &rollbackFailed)) {
        if (rollbackFailed) {
            UndoRedoManager::instance().clear();
            UndoRedoManager::instance().undoStack()->resetClean();
        }
        return {};
    }
    return std::make_unique<FileCommand>(changes, QObject::tr("Replace audio sources"));
}

std::unique_ptr<TransactionalUndoCommand> registerImport(
    const AssetInfo& asset, bool copiedFile, QString* error)
{
    if (!recoverDiscard(error)) return {};
    QList<Change> changes;
    if (copiedFile) {
        const QString cache = PathUtils::getDotCacheDir(AssetRegistry::instance().projectRoot());
        if (!PathUtils::ensureDirectory(cache)) {
            fail(error, QObject::tr("Could not create the audio undo directory."));
            return {};
        }
        const auto storage = std::make_shared<QTemporaryDir>(
            QDir(cache).filePath(QStringLiteral("source-import-XXXXXX")));
        if (!storage->isValid()) {
            fail(error, QObject::tr("Could not keep a copy of the audio file for undo."));
            return {};
        }
        auto before = std::make_shared<FileRevision>();
        before->metadata = asset.metadata;
        before->hash = asset.contentHash;
        auto after = std::make_shared<FileRevision>(*before);
        after->storage = storage;
        after->file = storage->filePath(QStringLiteral("import.") + QFileInfo(asset.absolutePath).suffix());
        if (!QFile::copy(asset.absolutePath, after->file)
            || AssetImporter::computeContentHash(after->file) != asset.contentHash) {
            fail(error, QObject::tr("Could not keep a copy of '%1' for undo.").arg(asset.relativePath));
            return {};
        }
        changes.append({asset.relativePath, asset.relativePath, false, before, after});
    }
    if (!AssetRegistry::instance().registerAsset(asset, copiedFile)) {
        fail(error, QObject::tr("Could not register '%1': the source ID or path is already in use.")
            .arg(asset.relativePath));
        return {};
    }
    return std::make_unique<ImportCommand>(asset, changes);
}

bool replayFiles(const QList<std::shared_ptr<TransactionalUndoCommand>>& commands,
                 bool redo, bool* rollbackFailed)
{
    *rollbackFailed = false;
    QList<TransactionalUndoCommand*> applied;
    for (qsizetype step = 0; step < commands.size(); ++step) {
        auto* command = commands[redo ? step : commands.size() - step - 1].get();
        if (!command->tryReplay(redo)) {
            *rollbackFailed = command->isObsolete();
            for (auto it = applied.crbegin(); it != applied.crend(); ++it)
                if (!(*it)->tryReplay(!redo)) *rollbackFailed = true;
            return false;
        }
        applied.append(command);
    }
    return true;
}

bool copyFile(const QString& source, const QString& destination, QString* error)
{
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return fail(error, QObject::tr("Could not create the destination folder for '%1'.").arg(destination));
    QString failure;
    const auto fileError = [&](const QFileDevice& file) {
        failure = QObject::tr("Could not copy '%1' to '%2': %3 (file error %4).")
            .arg(source, destination, file.errorString()).arg(static_cast<int>(file.error()));
        return false;
    };
    const bool copied = retryFileOperation([&] {
        QFile input(source);
        QSaveFile output(destination);
        if (!input.open(QIODevice::ReadOnly))
            return fileError(input);
        if (!output.open(QIODevice::WriteOnly))
            return fileError(output);
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        qint64 count = 0;
        while ((count = input.read(buffer.data(), buffer.size())) > 0) {
            if (output.write(buffer.constData(), count) != count)
                return fileError(output);
        }
        if (count < 0)
            return fileError(input);
        input.close();
        return output.commit() || fileError(output);
    });
    return copied || fail(error, failure);
}

void commitChanges()
{
    pendingChanges.clear();
}

bool recoverDiscard(QString* error)
{
    if (discardRollbackChanges.isEmpty())
        return true;
    auto& mediaPool = MediaPool::instance();
    mediaPool.beginSourceFileChange();
    const auto resumeReaders = qScopeGuard([&] { mediaPool.endSourceFileChange(); });
    while (!discardRollbackChanges.isEmpty()) {
        if (!applyFileChange(discardRollbackChanges.last().reversed(), error))
            return false;
        discardRollbackChanges.removeLast();
    }
    return true;
}

bool discardChanges(QString* error, bool* rollbackFailed)
{
    if (rollbackFailed)
        *rollbackFailed = false;
    if (!recoverDiscard(error))
        return false;
    auto& mediaPool = MediaPool::instance();
    mediaPool.beginSourceFileChange();
    const auto resumeReaders = qScopeGuard([&] { mediaPool.endSourceFileChange(); });
    QList<Change> changes;
    std::shared_ptr<QTemporaryDir> storage;
    auto& registry = AssetRegistry::instance();
    for (const auto& asset : registry.snapshot()) {
        if (!registry.isUncommitted(asset.sourceID) || !QFileInfo::exists(asset.absolutePath))
            continue;
        if (!storage) {
            storage = std::make_shared<QTemporaryDir>(
                QDir(PathUtils::getDotCacheDir(registry.projectRoot()))
                    .filePath(QStringLiteral("source-discard-XXXXXX")));
            if (!storage->isValid())
                return fail(error, QObject::tr("Could not keep the imported audio files while closing the project."));
        }
        auto before = std::make_shared<FileRevision>();
        before->storage = storage;
        before->file = storage->filePath(QString::number(asset.sourceID));
        before->metadata = asset.metadata;
        before->hash = asset.contentHash;
        if (!copyFile(asset.absolutePath, before->file, error))
            return false;
        changes.append({asset.relativePath, asset.relativePath, false,
            before, std::make_shared<FileRevision>()});
    }
    for (auto it = pendingChanges.crbegin(); it != pendingChanges.crend(); ++it)
        changes.append(it->reversed());
    if (!applyBatch(changes, ApplyMode::Discard, error, rollbackFailed))
        return false;
    pendingChanges.clear();
    return true;
}

}
