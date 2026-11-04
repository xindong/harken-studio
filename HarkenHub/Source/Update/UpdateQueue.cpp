#include "Update/UpdateQueue.h"

#include "Transactions/HandoffArea.h"
#include "Platform/AclHardening.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>

#include <algorithm>

namespace VNH {

namespace {

constexpr qint64 kQueueMarkerMaxBytes = 4 * 1024;

QString studioResultPath(const QString& transactionId)
{
    return HandoffArea::rootDir() + QLatin1Char('/') + transactionId + QStringLiteral(".result");
}

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
        if (product == QStringLiteral("HarkenStudio"))
            entry.userId = fileOwnerId(info.filePath());
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

bool UpdateQueue::writeStudioResult(const QString& transactionId,
                                    const InstallOutcome& outcome,
                                    const QString& userId, QString* error)
{
    if (!HandoffArea::isValidTransactionId(transactionId)
        || (outcome.ok ? outcome.buildId.isEmpty() || !outcome.failureCode.isEmpty()
                       : outcome.failureCode.isEmpty() || outcome.failureCode == QStringLiteral("InstallBusy"))) {
        *error = QStringLiteral("Invalid Studio installation result");
        return false;
    }
    const QJsonObject result{
        {QStringLiteral("transactionId"), transactionId},
        {QStringLiteral("product"), QStringLiteral("HarkenStudio")},
        {QStringLiteral("state"), outcome.ok ? QStringLiteral("Installed") : QStringLiteral("Failed")},
        {QStringLiteral("buildId"), outcome.buildId},
        {QStringLiteral("installPath"), outcome.installPath},
        {QStringLiteral("failureCode"), outcome.failureCode},
        {QStringLiteral("failureDetail"), outcome.error},
    };
    if (!QDir().mkpath(HandoffArea::rootDir())) {
        *error = QStringLiteral("Cannot create the installation result directory");
        return false;
    }
    const QString path = studioResultPath(transactionId);
    QTemporaryFile file(path + QStringLiteral(".XXXXXX"));
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (!file.open() || file.write(bytes) != bytes.size() || !file.flush()) {
        *error = QStringLiteral("Cannot write installation result: %1 (%2)").arg(path, file.errorString());
        return false;
    }
    file.close();
    if (!grantFileReadAndDelete(file.fileName(), userId, error))
        return false;
    if (!file.rename(path)) {
        *error = QStringLiteral("Cannot commit installation result: %1 (%2)").arg(path, file.errorString());
        return false;
    }
    file.setAutoRemove(false);
    return true;
}

std::optional<InstallOutcome> UpdateQueue::studioResult(const QString& transactionId,
                                                        QString* error)
{
    error->clear();
    if (!HandoffArea::isValidTransactionId(transactionId)) {
        *error = QStringLiteral("Invalid transaction id");
        return std::nullopt;
    }
    const QString path = studioResultPath(transactionId);
    if (!QFileInfo::exists(path))
        return std::nullopt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read installation result: %1 (%2)").arg(path, file.errorString());
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto object = document.object();
    bool valid = file.error() == QFileDevice::NoError && document.isObject();
    for (const char* field : {"transactionId", "product", "state", "buildId", "installPath",
                             "failureCode", "failureDetail"})
        valid &= object.value(QLatin1String(field)).isString();
    const QString state = object.value(QStringLiteral("state")).toString();
    InstallOutcome outcome;
    outcome.ok = state == QStringLiteral("Installed");
    outcome.buildId = object.value(QStringLiteral("buildId")).toString();
    outcome.installPath = object.value(QStringLiteral("installPath")).toString();
    outcome.failureCode = object.value(QStringLiteral("failureCode")).toString();
    outcome.error = object.value(QStringLiteral("failureDetail")).toString();
    if (!valid || object.value(QStringLiteral("transactionId")).toString() != transactionId
        || object.value(QStringLiteral("product")).toString() != QStringLiteral("HarkenStudio")
        || (state != QStringLiteral("Installed") && state != QStringLiteral("Failed"))
        || (outcome.ok ? outcome.buildId.isEmpty() || !outcome.failureCode.isEmpty()
                       : outcome.failureCode.isEmpty() || outcome.failureCode == QStringLiteral("InstallBusy"))) {
        *error = QStringLiteral("Invalid installation result: %1").arg(path);
        return std::nullopt;
    }
    return outcome;
}

bool UpdateQueue::discardStudioResult(const QString& transactionId)
{
    if (!HandoffArea::isValidTransactionId(transactionId))
        return false;
    const QString path = studioResultPath(transactionId);
    if (!QFileInfo::exists(path) || QFile::remove(path))
        return true;
    qWarning().noquote() << "Cannot remove acknowledged installation result:" << path;
    return false;
}

}
