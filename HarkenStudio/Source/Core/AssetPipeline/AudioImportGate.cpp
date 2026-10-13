
#include "AudioImportGate.h"
#include "AudioFormatCatalog.h"

#include "../ItemTypeCatalog.h"
#include "../ProjectManager.h"
#include "../ProjectTypes.h"

#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>

namespace VNX {

const QStringList& AudioImportGate::acceptedExtensions()
{
    return AudioFormatCatalog::extensions();
}

bool AudioImportGate::mimeHasAudio(const QMimeData* mimeData)
{
    if (!mimeData || !mimeData->hasUrls())
        return false;

    for (const QUrl& url : mimeData->urls())
    {
        const QString ext = QFileInfo(url.toLocalFile()).suffix().toLower();
        if (acceptedExtensions().contains(ext))
            return true;
    }
    return false;
}

QStringList AudioImportGate::extractAudioFiles(const QMimeData* mimeData)
{
    QStringList result;
    if (!mimeData || !mimeData->hasUrls())
        return result;

    const QStringList filters = AudioFormatCatalog::filePatterns();

    for (const QUrl& url : mimeData->urls())
    {
        const QString localPath = url.toLocalFile();
        if (localPath.isEmpty())
            continue;

        const QFileInfo info(localPath);
        if (info.isDir())
        {
            QDirIterator it(localPath, filters, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
            {
                it.next();
                result << it.filePath();
            }
        }
        else if (acceptedExtensions().contains(info.suffix().toLower()))
        {
            result << localPath;
        }
    }

    result.removeDuplicates();
    return result;
}

bool AudioImportGate::canDropOnTarget(const ProjectItem* target)
{
    return ProjectManager::canAcceptImportedAudioParent(target);
}

bool AudioImportGate::bucketAcceptsAudioDrop(const QString& bucketFolderName)
{
    return ItemTypeCatalog::bucketAcceptsAudioImport(bucketFolderName);
}

ImportPlacement AudioImportGate::resolveImportParent(ProjectItem* context)
{
    if (!context)
        return { ImportPlacementKind::RootSound, nullptr };

    // A Sound target means "insert beside it": land in its parent, or at
    // root level when the Sound itself is a root object.
    if (context->type == ProjectItem::Type::Sound)
    {
        if (ProjectItem* parent = context->parent)
            return { ImportPlacementKind::Parent, parent };
        return { ImportPlacementKind::RootSound, nullptr };
    }

    if (ProjectManager::canAcceptImportedAudioParent(context))
        return { ImportPlacementKind::Parent, context };

    return { ImportPlacementKind::Rejected, nullptr };
}

}
