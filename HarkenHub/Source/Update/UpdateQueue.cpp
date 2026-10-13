#include "Update/UpdateQueue.h"

#include "Transactions/HandoffArea.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace VNH {

namespace {

constexpr qint64 kQueueMarkerMaxBytes = 4 * 1024;

bool writeMarker(const QString& transactionId, const QJsonObject& body,
                 QString* error)
{
    if (!HandoffArea::isValidTransactionId(transactionId)) {
        *error = QStringLiteral("Invalid transaction id");
        return false;
    }
    const QString path = HandoffArea::queuePath(transactionId);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        *error = QStringLiteral(
                     "Queue marker already exists or is not writable: %1")
                     .arg(path);
        return false;
    }
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (file.write(json) != json.size()) {
        file.close();
        QFile::remove(path);
        *error = QStringLiteral("Write failed: %1").arg(path);
        return false;
    }
    return true;
}

QJsonObject markerBody(const QString& product)
{
    QJsonObject body;
    body[QStringLiteral("product")] = product;
    body[QStringLiteral("queuedAt")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    return body;
}

}

bool UpdateQueue::markStudioInstall(const QString& transactionId,
                                    QString* error)
{
    return writeMarker(transactionId, markerBody(QStringLiteral("HarkenStudio")),
                       error);
}

bool UpdateQueue::markHubInstall(const QString& transactionId,
                                      const QString& restartMode,
                                      const QString& userId,
                                      QString* error)
{
    if (restartMode != QStringLiteral("gui") &&
        restartMode != QStringLiteral("background")) {
        *error = QStringLiteral("Invalid restart mode: %1").arg(restartMode);
        return false;
    }
    QJsonObject body = markerBody(QStringLiteral("HarkenHub"));
    body[QStringLiteral("restart")] = restartMode;
    if (!userId.isEmpty())
        body[QStringLiteral("user")] = userId;
    return writeMarker(transactionId, body, error);
}

std::vector<UpdateQueueEntry> UpdateQueue::pendingEntries()
{
    std::vector<UpdateQueueEntry> entries;
    const QDir handoff(HandoffArea::rootDir());
    if (!handoff.exists())
        return entries;

    const auto files = handoff.entryInfoList({QStringLiteral("*.queue")},
                                             QDir::Files | QDir::NoSymLinks,
                                             QDir::Time | QDir::Reversed);
    for (const QFileInfo& info : files) {
        const QString transactionId = info.completeBaseName();
        if (!HandoffArea::isValidTransactionId(transactionId))
            continue;
        if (info.size() <= 0 || info.size() > kQueueMarkerMaxBytes)
            continue;
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject())
            continue;
        const QJsonObject body = doc.object();
        const QString product = body[QStringLiteral("product")].toString();
        if (product != QStringLiteral("HarkenStudio") &&
            product != QStringLiteral("HarkenHub"))
            continue;
        UpdateQueueEntry entry;
        entry.transactionId = transactionId;
        entry.product = product;
        entry.restartMode = body[QStringLiteral("restart")].toString();
        entry.userId = body[QStringLiteral("user")].toString();
        entry.queuedAt = body[QStringLiteral("queuedAt")].toString();
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool UpdateQueue::hasPendingHubUpdate()
{
    const auto entries = pendingEntries();
    return std::any_of(entries.begin(), entries.end(),
                       [](const UpdateQueueEntry& entry) {
                           return entry.product ==
                                  QStringLiteral("HarkenHub");
                       });
}

}
