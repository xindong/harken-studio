#pragma once

#include <QString>

namespace VNX {

enum class ProjectVolumeKind
{
    CertifiedInternal,
    RecoverableLocal,
    Remote,
};

struct ProjectVolumeInfo
{
    ProjectVolumeKind kind = ProjectVolumeKind::Remote;
    QString rootPath;
    QString fileSystem;
    qint64 availableBytes = -1;
};

class ProjectVolume
{
public:
    static ProjectVolumeInfo classify(const QString& path);
};

}
