#pragma once

#include <QString>

#include <vector>

namespace VNH {

struct UpdateQueueEntry
{
    QString transactionId;
    QString product;
    QString restartMode;
    QString userId;
    QString queuedAt;
};

class UpdateQueue
{
public:
    static bool markStudioInstall(const QString& transactionId,
                                  QString* error);
    static bool markHubInstall(const QString& transactionId,
                                    const QString& restartMode,
                                    const QString& userId,
                                    QString* error);
    static std::vector<UpdateQueueEntry> pendingEntries();
    static bool hasPendingHubUpdate();
};

}
