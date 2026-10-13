#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace VNH {

class ProjectStore;

class BuildPinRegistry
{
public:
    static QString pinsDir();
    static QString currentUserPinFile();
    static bool writeCurrentUserPins(const QStringList& buildIds,
                                     QString* error);
    static bool refreshFromProjects(const ProjectStore& store, QString* error);
    static QSet<QString> machinePinnedBuildIds();
};

}
