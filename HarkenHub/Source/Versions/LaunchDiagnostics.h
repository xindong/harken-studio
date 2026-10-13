#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>

namespace VNH {

struct LaunchDiagnosticRecord
{
    QString buildId;
    QString userLaunchState;
    QString lastSelfTestAt;
    QString lastLaunchError;
};

class LaunchDiagnostics
{
public:
    static QString filePath();
    static bool record(const QString& buildId, const QString& userLaunchState,
                       const QString& lastLaunchError, const QDateTime& at);
    static QHash<QString, LaunchDiagnosticRecord> snapshot();
};

}
