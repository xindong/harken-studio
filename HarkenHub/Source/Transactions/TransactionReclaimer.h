#pragma once

#include <QDateTime>
#include <QStringList>

namespace harken::hub {

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
