#pragma once

#include <QDateTime>
#include <QStringList>

namespace VNH {

struct ReclaimReport
{
    bool locked = false;
    QStringList reclaimedTransactionIds;
    QStringList journalWriteFailures;
    QStringList installRecoveryErrors;
};

class TransactionReclaimer
{
public:
    static ReclaimReport reclaimAtStartup(const QDateTime& now);
};

}
