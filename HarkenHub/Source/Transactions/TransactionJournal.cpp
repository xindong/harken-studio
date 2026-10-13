#include "Transactions/TransactionJournal.h"

#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace VNH {

QString TransactionJournal::filePath()
{
    return machineDataDir() + QStringLiteral("/transactions.log");
}

bool TransactionJournal::append(const QString& transactionId,
                                const QString& product,
                                const QString& targetBuildId,
                                const QString& state, const QString& failureCode,
                                const QDateTime& at, const QString& failureDetail)
{
    QJsonObject entry;
    entry[QStringLiteral("transactionId")] = transactionId;
    entry[QStringLiteral("product")] = product;
    entry[QStringLiteral("targetBuildId")] = targetBuildId;
    entry[QStringLiteral("state")] = state;
    if (!failureCode.isEmpty())
        entry[QStringLiteral("failureCode")] = failureCode;
    if (!failureDetail.isEmpty())
        entry[QStringLiteral("failureDetail")] = failureDetail;
    entry[QStringLiteral("at")] = at.toUTC().toString(Qt::ISODateWithMs);

    const QString path = filePath();
    if (!QDir().mkpath(QFileInfo(path).dir().path()))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    const QByteArray line =
        QJsonDocument(entry).toJson(QJsonDocument::Compact) + QByteArrayLiteral("\n");
    return file.write(line) == line.size();
}

QHash<QString, QString> TransactionJournal::lastStateByTransaction()
{
    QHash<QString, QString> states;
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return states;
    while (!file.atEnd()) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readLine());
        if (!doc.isObject())
            continue;
        const QJsonObject entry = doc.object();
        const QString transactionId =
            entry[QStringLiteral("transactionId")].toString();
        const QString state = entry[QStringLiteral("state")].toString();
        if (!transactionId.isEmpty() && !state.isEmpty())
            states.insert(transactionId, state);
    }
    return states;
}

}
