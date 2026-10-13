#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>

namespace VNH {

struct UninstallOutcome
{
    bool ok = false;
    QString error;
    QString failureCode;
    QString buildId;
};

class UninstallExecutor
{
public:
    using ProcessProbe = std::function<QStringList(const QString&)>;

    static UninstallOutcome run(const QString& buildId, const QDateTime& now);
    static UninstallOutcome run(const QString& buildId, const QDateTime& now,
                                const ProcessProbe& probe);
};

}
