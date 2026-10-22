#pragma once

#include <QStringList>

namespace VNH {

struct RunningProcess
{
    QString executable;
    QStringList arguments;
};

bool runningUnrealEditors(QList<RunningProcess>* processes);

QStringList runningExecutablesUnder(const QString& directory);

QStringList runningExecutablesUnder(const QString& directory,
                                    qint64 excludePid);

QString executablePathForPid(qint64 pid);

bool isProcessRunning(qint64 pid);

bool terminateProcessById(qint64 pid);

}
