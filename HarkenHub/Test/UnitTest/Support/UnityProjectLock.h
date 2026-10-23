#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

namespace HarkenHubTest {

int runUnityProjectLock(const QStringList& arguments);

class UnityProjectLock
{
public:
    ~UnityProjectLock()
    {
        if (!unlock()) {
            process_.kill();
            process_.waitForFinished(5000);
        }
    }

    bool lock(const QString& project, const QString& mode = {})
    {
        const QString path = QDir(project).filePath(QStringLiteral("Temp/UnityLockfile"));
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        process_.start(QCoreApplication::applicationFilePath(),
                       {QStringLiteral("--unity-project-lock"), path, mode});
        return process_.waitForStarted(5000) && process_.waitForReadyRead(5000)
            && process_.readLine().trimmed() == "locked";
    }

    bool unlock()
    {
        if (process_.state() == QProcess::NotRunning) return true;
        process_.write("\n");
        return process_.waitForFinished(5000) && process_.exitCode() == 0
            && process_.exitStatus() == QProcess::NormalExit;
    }

private:
    QProcess process_;
};

}
