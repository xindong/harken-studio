#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace VNH {

inline constexpr int kInstallMachineDataUnavailableExitCode = 3;

struct InstallOutcome
{
    bool ok = false;
    QString error;
    QString failureCode;
    QString buildId;
    QString installPath;

    int exitCode() const;
};

class InstallExecutor
{
public:
    static QString lockFilePath();
    static bool isTransactionActive();
    static InstallOutcome run(const QString& transactionId,
                              const QByteArray& manifestPublicKey,
                              const QDateTime& now,
                              bool explicitInstall = false);
};

}
