#include "BucketFolderStore.h"

#include <QDir>
#include <QFileInfo>


namespace VNX::BucketFolderStore {

namespace {

std::shared_ptr<PhysicalFolder> scanFolder(const QFileInfo& folderInfo, PhysicalFolder* parent)
{
    auto folder = std::make_shared<PhysicalFolder>();
    folder->name = folderInfo.fileName();
    folder->parent = parent;

    const QFileInfoList children = QDir(folderInfo.absoluteFilePath()).entryInfoList(
        QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    folder->children.reserve(static_cast<size_t>(children.size()));
    for (const QFileInfo& childInfo : children)
        folder->children.push_back(scanFolder(childInfo, folder.get()));
    return folder;
}

bool isUsableFolderName(const QString& name)
{
    return !name.isEmpty()
        && name != QStringLiteral(".")
        && name != QStringLiteral("..")
        && !name.contains(QChar('/'))
        && !name.contains(QChar('\\'));
}


}

std::vector<ProjectFolderBinding> scanFolderTree(const QString& bucketRootDir,
                                                 const QString& bucketName)
{
    std::vector<ProjectFolderBinding> result;
    const QFileInfoList roots = QDir(bucketRootDir).entryInfoList(
        QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    result.reserve(static_cast<size_t>(roots.size()));
    for (const QFileInfo& rootInfo : roots) {
        ProjectFolderBinding binding;
        binding.parentFolder = bucketName;
        binding.rootFolder = scanFolder(rootInfo, nullptr);
        result.push_back(std::move(binding));
    }
    return result;
}

QString folderRelPath(const PhysicalFolder* folder)
{
    if (!folder)
        return {};

    QStringList segments;
    for (const PhysicalFolder* current = folder; current; current = current->parent) {
        if (!isUsableFolderName(current->name)) {
            return {};
        }
        segments.prepend(current->name);
    }
    return segments.join(QChar('/'));
}

QString pathComparisonKey(const QString& path)
{
    const QString cleaned = QDir::cleanPath(path);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return cleaned.toCaseFolded();
#else
    return cleaned;
#endif
}

}
