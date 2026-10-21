#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>

namespace VNH {

struct RepairOutcome
{
    bool ok = false;
    QString error;
    QString failureCode;
    QString buildId;
    QString healthState;
};

class RepairExecutor
{
public:
    using ProcessProbe = std::function<QStringList(const QString&)>;

    static RepairOutcome run(const QString& buildId, const QDateTime& now,
                             const QString& transactionId = {});
    static RepairOutcome run(const QString& buildId, const QDateTime& now,
                             const ProcessProbe& probe, const QString& transactionId = {});
};

}
