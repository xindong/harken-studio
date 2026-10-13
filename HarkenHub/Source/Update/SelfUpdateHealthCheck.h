#pragma once

#include <QString>

#include <functional>

namespace VNH {

struct HealthReport
{
    bool ok = false;
    QString error;
    QString version;
    QString buildId;
};

class SelfUpdateHealthCheck
{
public:
    static QString newChannelName();
    static HealthReport awaitReport(
        const QString& channelName,
        const std::function<bool(const QString&)>& launchReporter,
        int timeoutMs);
    static HealthReport runInstalledCheck(const QString& hubExePath,
                                          int timeoutMs);
};

}
