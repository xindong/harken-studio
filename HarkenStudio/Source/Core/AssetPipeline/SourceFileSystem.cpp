#include "SourceFileSystem.h"

#include "AssetRegistry.h"
#include "PathUtils.h"
#include "../ProjectDataPolicy.h"
#include "../NameValidator.h"
#include "../ProjectManager.h"
#include "../ToastManager.h"
#include "../UndoCommands.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUuid>
#include <QDebug>

#include <algorithm>

namespace VNX::SourceFileSystem {
namespace {

struct Change {
    QString from;
    QString to;
    bool directory = false;

    Change reversed() const { return {to, from, directory}; }
};

QList<Change> pendingChanges;

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

    bool ok = false;
    if (change.from.isEmpty()) {
        ok = QDir().mkdir(to);
    } else if (change.to.isEmpty()) {
        ok = QDir().rmdir(from);
    } else {
        ok = QDir().rename(from, to);
    }
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
        const bool retain = change.directory || (asset && !registry.isUncommitted(asset->sourceID));
        if (record && retain)
            pendingChanges.append(change);
        if (!change.from.isEmpty() && !change.to.isEmpty())
            registry.relocateSourcePaths(change.from, change.to);
    }
    syncSoundPaths();
    emit registry.sourceFoldersChanged();
}

bool applyBatch(const QList<Change>& changes, bool record, QString* error, bool* rollbackFailed = nullptr)
{
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
            QList<Change> remaining;
            QStringList rollbackErrors;
            for (auto it = applied.crbegin(); it != applied.crend(); ++it) {
                QString rollbackError;
                if (!applyFileChange(it->reversed(), &rollbackError)) {
                    remaining.prepend(*it);
                    rollbackErrors.append(rollbackError);
                }
            }
            if (!remaining.isEmpty()) {
                publishChanges(remaining, record);
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
    publishChanges(changes, record);
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
        if (applyBatch(changes, true, &error, &rollbackFailed))
            return true;
        setObsolete(rollbackFailed);
        qWarning().noquote() << error;
        ToastManager::instance().showToast(error, 5000, ToastType::Error);
        return false;
    }

    QList<Change> changes_;
};

bool push(const QList<Change>& changes, const QString& label, QString* error)
{
    if (changes.isEmpty())
        return false;
    bool rollbackFailed = false;
    if (!applyBatch(changes, true, error, &rollbackFailed)) {
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
    return true;
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
    return push(planMove(paths, parentPath, error), QObject::tr("Move sources"), error);
}

bool removeEmptyFolder(const QString& path, QString* error)
{
    const QString absolute = absolutePath(path);
    if (path.isEmpty() || absolute.isEmpty() || !QFileInfo(absolute).isDir() || !QDir(absolute).isEmpty())
        return fail(error, QObject::tr("Only empty source folders can be deleted."));
    return push({{path, {}, true}}, QObject::tr("Delete source folder %1").arg(path), error);
}

void commitChanges()
{
    pendingChanges.clear();
}

void discardChanges()
{
    for (auto it = pendingChanges.crbegin(); it != pendingChanges.crend(); ++it) {
        QString error;
        if (!applyBatch({it->reversed()}, false, &error))
            qWarning().noquote() << error;
    }
    pendingChanges.clear();
}

}
