#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>

namespace VNH {

class TransactionJournal
{
public:
    static QString filePath();
    static bool append(const QString& transactionId, const QString& product,
                       const QString& targetBuildId, const QString& state,
                       const QString& failureCode, const QDateTime& at,
                       const QString& failureDetail = QString());
    static QHash<QString, QString> lastStateByTransaction();
};

}
