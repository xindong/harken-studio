#pragma once

#include <QString>
#include <QStringList>

namespace VNH {

struct ProjectProbeResult
{
    bool ok = false;
    QString error;
    QString errorCode;
    QStringList candidates;
    QString canonicalPath;
    QString displayName;
    QString projectId;
    QString savedStudioVersion;
    QString metadataWarning;
    int schemaVersion = 0;
};

class ProjectProbe
{
public:
    static ProjectProbeResult probe(const QString& inputPath);
};

}
