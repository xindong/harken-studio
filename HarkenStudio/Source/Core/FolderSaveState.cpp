#include "FolderSaveState.h"

#include "BucketFolderStore.h"
#include "ProjectDataPolicy.h"
#include "ProjectTypes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <map>

namespace VNX {
namespace {

const QStringList& managedBuckets()
{
    static const QStringList buckets{QStringLiteral("Actions"), QStringLiteral("Layers"),
        QStringLiteral("Attenuations"), QStringLiteral("Variables"), QStringLiteral("Busses")};
    return buckets;
}

bool copyFiles(const QString& source, const QString& destination,
               const QSet<QString>& excluded, QString* error)
{
    if (!QDir().mkpath(destination)) {
        if (error) *error = QStringLiteral("Cannot create folder save directory: %1").arg(destination);
        return false;
    }
    for (const auto& entry : QDir(source).entryInfoList(
             QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (excluded.contains(BucketFolderStore::pathComparisonKey(entry.absoluteFilePath())))
            continue;
        if (entry.isSymLink() || !QFile::copy(entry.absoluteFilePath(),
                                             QDir(destination).filePath(entry.fileName()))) {
            if (error) *error = QStringLiteral("Cannot preserve folder file: %1").arg(entry.absoluteFilePath());
            return false;
        }
    }
    return true;
}

bool copyTree(const QString& source, const QString& destination,
              const QSet<QString>& excluded, QString* error)
{
    if (!copyFiles(source, destination, excluded, error)) return false;
    for (const auto& entry : QDir(source).entryInfoList(
             QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (entry.isSymLink() || !copyTree(entry.absoluteFilePath(),
            QDir(destination).filePath(entry.fileName()), excluded, error))
            return false;
    }
    return true;
}

}

struct FolderSaveState::Data
{
    struct FolderContent {
        std::weak_ptr<PhysicalFolder> folder;
        QString path;
    };

    struct SavedFolder {
        std::weak_ptr<PhysicalFolder> folder;
        QString path;
    };

    QTemporaryDir contents;
    std::map<const PhysicalFolder*, FolderContent> folders;
    std::map<const PhysicalFolder*, SavedFolder> savedFolders;
    QHash<QUuid, QString> savedObjectDirectories;
};

FolderSaveState::FolderSaveState(std::unique_ptr<Data> data)
    : data_(std::move(data))
{
}

FolderSaveState::~FolderSaveState() = default;

std::unique_ptr<FolderSaveState> FolderSaveState::capture(const ProjectData& project,
                                                        QString* error)
{
    auto data = std::make_unique<Data>();
    if (!data->contents.isValid()) {
        if (error) *error = QStringLiteral("Cannot prepare folder file storage.");
        return {};
    }
    QSet<QString> documents;
    for (const auto* binding : project.allDocumentBindings())
        if (!binding->filePath.isEmpty())
            documents.insert(BucketFolderStore::pathComparisonKey(binding->filePath));

    const QDir projectRoot(QFileInfo(project.projectPath).absolutePath());
    for (const auto& bucket : managedBuckets()) {
        const auto copy = bucket == QStringLiteral("Busses") ? copyTree : copyFiles;
        if (!copy(projectRoot.filePath(bucket),
                  QDir(data->contents.path()).filePath(bucket), documents, error))
            return {};
    }
    const auto captureFolder = [&](auto&& self, const std::shared_ptr<PhysicalFolder>& folder,
                                   const QString& bucket) -> bool {
        const QString storagePath = QDir(data->contents.path()).filePath(
            QString::number(data->folders.size()));
        const QString sourcePath = projectRoot.filePath(bucket + QLatin1Char('/')
            + project.folderRelPath(folder.get()));
        if (!copyFiles(sourcePath, storagePath, documents, error)) return false;
        data->folders.emplace(folder.get(), Data::FolderContent{folder, storagePath});
        for (const auto& child : folder->children)
            if (!self(self, child, bucket)) return false;
        return true;
    };
    for (const auto& binding : project.folders)
        if (!captureFolder(captureFolder, binding.rootFolder, binding.parentFolder)) return {};
    auto state = std::unique_ptr<FolderSaveState>(new FolderSaveState(std::move(data)));
    state->markSaved(project);
    return state;
}

void FolderSaveState::markSaved(const ProjectData& project)
{
    data_->savedFolders.clear();
    const auto remember = [&](auto&& self, const std::shared_ptr<PhysicalFolder>& folder,
                              const QString& path) -> void {
        data_->savedFolders.emplace(folder.get(), Data::SavedFolder{folder, path});
        for (const auto& child : folder->children)
            self(self, child, path + QLatin1Char('/') + child->name);
    };
    for (const auto& binding : project.folders)
        remember(remember, binding.rootFolder,
                 binding.parentFolder + QLatin1Char('/') + binding.rootFolder->name);
    data_->savedObjectDirectories.clear();
    const auto rememberObjects = [&](auto&& self, const ProjectItem* item,
                                     const QString& bucket) -> void {
        const QString folderPath = project.folderRelPath(project.objectFolder(item));
        data_->savedObjectDirectories.insert(item->guid,
            folderPath.isEmpty() ? bucket : bucket + QLatin1Char('/') + folderPath);
        for (const auto& child : item->children) self(self, child.get(), bucket);
    };
    for (const auto* binding : project.allFileBindings())
        rememberObjects(rememberObjects, binding->rootItem.get(), binding->parentFolder);
}

bool FolderSaveState::isFolderDirty(const ProjectData& project, const PhysicalFolder* folder) const
{
    const auto saved = data_->savedFolders.find(folder);
    return saved == data_->savedFolders.end() || saved->second.folder.lock().get() != folder
        || saved->second.path != project.folderBucket(folder) + QLatin1Char('/') + project.folderRelPath(folder);
}

QSet<QString> FolderSaveState::bucketsWithPendingFileChanges(const ProjectData& project) const
{
    QHash<QUuid, QString> objectDirectories;
    const auto collectObjects = [&](auto&& self, const ProjectItem* item,
                                    const QString& bucket) -> void {
        const QString folderPath = project.folderRelPath(project.objectFolder(item));
        objectDirectories.insert(item->guid,
            folderPath.isEmpty() ? bucket : bucket + QLatin1Char('/') + folderPath);
        for (const auto& child : item->children) self(self, child.get(), bucket);
    };
    for (const auto* binding : project.allFileBindings())
        collectObjects(collectObjects, binding->rootItem.get(), binding->parentFolder);
    QSet<const PhysicalFolder*> folders;
    const auto collectFolders = [&](auto&& self, const PhysicalFolder* folder) -> void {
        folders.insert(folder);
        for (const auto& child : folder->children) self(self, child.get());
    };
    for (const auto& binding : project.folders)
        collectFolders(collectFolders, binding.rootFolder.get());

    QSet<QString> buckets;
    for (auto it = data_->savedObjectDirectories.cbegin(); it != data_->savedObjectDirectories.cend(); ++it) {
        const auto current = objectDirectories.constFind(it.key());
        if (current == objectDirectories.cend() || current.value() != it.value()) {
            buckets.insert(it.value().section(QLatin1Char('/'), 0, 0));
            if (current != objectDirectories.cend())
                buckets.insert(current.value().section(QLatin1Char('/'), 0, 0));
        }
    }
    for (const auto& [folder, saved] : data_->savedFolders)
        if (saved.folder.expired() || !folders.contains(folder))
            buckets.insert(saved.path.section(QLatin1Char('/'), 0, 0));
    return buckets;
}

bool FolderSaveState::stage(const ProjectData& project, const QString& directory, QString* error)
{
    for (auto entry = data_->folders.begin(); entry != data_->folders.end();) {
        if (entry->second.folder.expired()) {
            QDir(entry->second.path).removeRecursively();
            entry = data_->folders.erase(entry);
        } else {
            ++entry;
        }
    }
    for (const auto& bucket : managedBuckets())
        if (!copyTree(QDir(data_->contents.path()).filePath(bucket),
                      QDir(directory).filePath(bucket), {}, error)) return false;

    const auto stageFolder = [&](auto&& self, const std::shared_ptr<PhysicalFolder>& folder,
                                 const QString& bucket) -> bool {
        const QString relativePath = bucket + QLatin1Char('/') + project.folderRelPath(folder.get());
        if (!ProjectDataPolicy::isSafeRelativePath(relativePath)) {
            if (error) *error = QStringLiteral("Invalid project folder location.");
            return false;
        }
        const QString destination = QDir(directory).filePath(relativePath);
        const auto entry = data_->folders.find(folder.get());
        if (entry != data_->folders.end() && entry->second.folder.lock() == folder) {
            if (!copyFiles(entry->second.path, destination, {}, error)) return false;
        } else if (!QDir().mkpath(destination)) {
            if (error) *error = QStringLiteral("Cannot create project folder: %1").arg(relativePath);
            return false;
        }
        for (const auto& child : folder->children)
            if (!self(self, child, bucket)) return false;
        return true;
    };
    for (const auto& binding : project.folders)
        if (!stageFolder(stageFolder, binding.rootFolder, binding.parentFolder)) return false;

    return true;
}

}
