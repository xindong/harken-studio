
#pragma once

#include <QStringList>

class QMimeData;

namespace VNX {

struct ProjectItem;

enum class ImportPlacementKind
{
    RootSound,
    Parent,
    Rejected
};

struct ImportPlacement
{
    ImportPlacementKind kind;
    ProjectItem* parent = nullptr;
};

class AudioImportGate
{
public:
    static const QStringList& acceptedExtensions();
    static bool mimeHasAudio(const QMimeData* mimeData);
    static QStringList extractAudioFiles(const QMimeData* mimeData);

    static bool canDropOnTarget(const ProjectItem* target);
    static bool bucketAcceptsAudioDrop(const QString& bucketFolderName);
    static ImportPlacement resolveImportParent(ProjectItem* context);
};

}
