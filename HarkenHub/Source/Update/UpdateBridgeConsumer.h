#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>

namespace VNH {

struct BridgeConsumeReport
{
    int installed = 0;
    int failed = 0;
    int deferred = 0;
    QStringList details;
};

class UpdateBridgeConsumer
{
public:
    struct Hooks
    {
        std::function<int(const QString& installerPath,
                          const QString& logPath)>
            runHubInstaller;
        std::function<bool(const QStringList& arguments,
                           const QString& userId, QString* error)>
            restartHub;
        std::function<QStringList()> runningHubProcesses;
        std::function<void(qint64 milliseconds)> wait;
        qint64 hubQuietTimeoutMs = 60000;
    };

    static Hooks defaultHooks();
    static BridgeConsumeReport run(const QByteArray& manifestPublicKey,
                                   const QDateTime& now, const Hooks& hooks);
};

}
