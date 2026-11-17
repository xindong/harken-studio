#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QHash>

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

struct RepairSource
{
    bool ok = false;
    QString error;
    QString packagePath;
    QString manifestHash;
    QString failureCode = QStringLiteral("RepairSourceRejected");
    QHash<QString, QString> files;
};

class RepairExecutor
{
public:
    static RepairSource verifySource(const QString& buildId, const QDateTime& now);

    using ProcessProbe = std::function<QStringList(const QString&)>;

    static RepairOutcome run(const QString& buildId, const QDateTime& now,
                             const QString& transactionId = {}, const QString& reviewedSnapshot = {}, const QString& reviewedManifestHash = {});
    static RepairOutcome run(const QString& buildId, const QDateTime& now,
                             const ProcessProbe& probe, const QString& transactionId = {}, const QString& reviewedSnapshot = {}, const QString& reviewedManifestHash = {});
};

}
