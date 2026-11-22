#pragma once

#include <QString>
#include <QStringList>

namespace harken::hub {

struct ProjectProbeResult
{
    bool ok = false;
    QString error;
    QString errorCode;
    QStringList candidates;
    QString canonicalPath;
    QString displayName;
    QString savedStudioVersion;
    QString metadataWarning;
};

class ProjectProbe
{
public:
    static ProjectProbeResult probe(const QString& inputPath);
};

}
