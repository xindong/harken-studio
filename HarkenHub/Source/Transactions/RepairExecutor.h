#pragma once

#include <QDateTime>
#include <QString>

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
    static RepairOutcome run(const QString& buildId, const QDateTime& now);
};

}
