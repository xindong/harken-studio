#pragma once

#include <QJsonArray>
#include <QString>

#include <vector>

namespace VNH {

struct TransactionSummary
{
    QString transactionId;
    QString product;
    QString targetBuildId;
    QString lastState;
    QString failureCode;
    QString failureDetail;
    QString firstAt;
    QString lastAt;
    int entryCount = 0;
};

class TransactionDiagnostics
{
public:
    static std::vector<TransactionSummary> summarize();
    static QString failureMessage(const QString& transactionId, int exitCode = 2);
    static QJsonArray toJson(const std::vector<TransactionSummary>& summaries);
};

}
