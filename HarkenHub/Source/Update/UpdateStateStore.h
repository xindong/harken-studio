#pragma once

#include <QDateTime>
#include <QString>

namespace VNH {

struct UpdateStateRecord
{
    bool found = false;
    QString product;
    QString state;
    QString version;
    QString buildId;
    QString downloadPath;
    QString manifestEnvelopePath;
    QString expectedSha256;
    qint64 packageSize = -1;
    qint64 downloadedBytes = -1;
    qint64 releaseSequence = -1;
    QString transactionId;
    QString failureCode;
    QString updatedAt;

    bool needsDownloadRecovery() const;
};

class UpdateStateStore
{
public:
    static QString filePath();
    static UpdateStateRecord load(const QString& product);
    static bool save(const UpdateStateRecord& record, const QDateTime& at);
    static bool clear(const QString& product);
};

}
