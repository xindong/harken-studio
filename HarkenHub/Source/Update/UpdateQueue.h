#pragma once

#include "Transactions/InstallExecutor.h"

#include <QString>
#include <QStringList>

#include <optional>
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
    static bool writeStudioResult(const QString& transactionId,
                                  const InstallOutcome& outcome,
                                  const QString& userId, QString* error);
    static QStringList retryStudioResults();
    static std::optional<InstallOutcome> studioResult(const QString& transactionId,
                                                      QString* error);
    static bool discardStudioResult(const QString& transactionId);
};

}
