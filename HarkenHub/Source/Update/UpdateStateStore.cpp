#include "Update/UpdateStateStore.h"

#include "Platform/AtomicFile.h"
#include "Platform/PlatformPaths.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>

namespace VNH {

namespace {

QJsonObject loadRoot(bool* ok = nullptr)
{
    QFile file(UpdateStateStore::filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        if (ok)
            *ok = !file.exists();
        return QJsonObject();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (ok)
        *ok = file.error() == QFileDevice::NoError && doc.isObject();
    return doc.isObject() ? doc.object() : QJsonObject();
}

bool saveRoot(const QJsonObject& root)
{
    return writeAtomicFile(UpdateStateStore::filePath(),
                           QJsonDocument(root).toJson(QJsonDocument::Indented));
}

UpdateStateRecord readRecord(const QJsonObject& root, const QString& product)
{
    UpdateStateRecord record;
    record.product = product;
    if (!root.contains(product))
        return record;
    const QJsonObject entry = root[product].toObject();
    record.found = true;
    record.state = entry[QStringLiteral("state")].toString();
    record.version = entry[QStringLiteral("version")].toString();
    record.buildId = entry[QStringLiteral("buildId")].toString();
    record.downloadPath = entry[QStringLiteral("downloadPath")].toString();
    record.manifestEnvelopePath =
        entry[QStringLiteral("manifestEnvelopePath")].toString();
    record.expectedSha256 = entry[QStringLiteral("expectedSha256")].toString();
    record.packageSize = entry[QStringLiteral("packageSize")].toInteger(-1);
    record.downloadedBytes =
        entry[QStringLiteral("downloadedBytes")].toInteger(-1);
    record.downloadRestarted = entry[QStringLiteral("downloadRestarted")].toBool();
    record.releaseSequence =
        entry[QStringLiteral("releaseSequence")].toInteger(-1);
    record.transactionId = entry[QStringLiteral("transactionId")].toString();
    record.failureCode = entry[QStringLiteral("failureCode")].toString();
    record.updatedAt = entry[QStringLiteral("updatedAt")].toString();
    return record;
}

QJsonObject entryFor(const UpdateStateRecord& record, const QDateTime& at)
{
    QJsonObject entry;
    entry[QStringLiteral("state")] = record.state;
    entry[QStringLiteral("version")] = record.version;
    entry[QStringLiteral("buildId")] = record.buildId;
    entry[QStringLiteral("downloadPath")] = record.downloadPath;
    entry[QStringLiteral("manifestEnvelopePath")] = record.manifestEnvelopePath;
    entry[QStringLiteral("expectedSha256")] = record.expectedSha256;
    entry[QStringLiteral("packageSize")] = record.packageSize;
    entry[QStringLiteral("downloadedBytes")] = record.downloadedBytes;
    entry[QStringLiteral("downloadRestarted")] = record.downloadRestarted;
    entry[QStringLiteral("releaseSequence")] = record.releaseSequence;
    if (!record.transactionId.isEmpty())
        entry[QStringLiteral("transactionId")] = record.transactionId;
    if (!record.failureCode.isEmpty())
        entry[QStringLiteral("failureCode")] = record.failureCode;
    entry[QStringLiteral("updatedAt")] = at.toUTC().toString(Qt::ISODateWithMs);
    return entry;
}

UpdateStateStore::WriteResult writeRecord(const QString& product,
                                          const UpdateStateRecord* expected,
                                          const UpdateStateRecord* record,
                                          const QDateTime& at)
{
    using Result = UpdateStateStore::WriteResult;
    if (product.isEmpty() || (record && record->state.isEmpty()) ||
        !QDir().mkpath(QFileInfo(UpdateStateStore::filePath()).absolutePath()))
        return Result::Failed;
    QLockFile lock(UpdateStateStore::filePath() + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(1000))
        return Result::Failed;
    bool ok = false;
    QJsonObject root = loadRoot(&ok);
    if (!ok)
        return Result::Failed;
    if (expected && readRecord(root, product) != *expected)
        return Result::Conflict;
    if (record)
        root[product] = entryFor(*record, at);
    else if (root.contains(product))
        root.remove(product);
    else
        return Result::Saved;
    return saveRoot(root) ? Result::Saved : Result::Failed;
}

}

QString UpdateStateStore::filePath()
{
    return userDataDir() + QStringLiteral("/update-state.json");
}

bool UpdateStateRecord::needsDownloadRecovery() const
{
    return found &&
           (state == QStringLiteral("Verified") ||
            state == QStringLiteral("Downloaded") ||
            state == QStringLiteral("Verifying") ||
            (state == QStringLiteral("VerificationFailed") && !downloadPath.isEmpty()) ||
            state == QStringLiteral("AwaitingUserInstall") ||
            state == QStringLiteral("InstallFailed") ||
            state == QStringLiteral("AwaitingRestart") ||
            state == QStringLiteral("AwaitingUserApproval")) &&
           (!QFileInfo(downloadPath).isFile() ||
            !QFileInfo(manifestEnvelopePath).isFile());
}

UpdateStateRecord UpdateStateStore::load(const QString& product)
{
    return readRecord(loadRoot(), product);
}

bool UpdateStateStore::save(const UpdateStateRecord& record, const QDateTime& at)
{
    return writeRecord(record.product, nullptr, &record, at) == WriteResult::Saved;
}

bool UpdateStateStore::clear(const QString& product)
{
    return writeRecord(product, nullptr, nullptr, {}) == WriteResult::Saved;
}

UpdateStateStore::WriteResult UpdateStateStore::replace(
    const UpdateStateRecord& expected, const UpdateStateRecord& record,
    const QDateTime& at)
{
    if (expected.product != record.product)
        return WriteResult::Failed;
    return writeRecord(record.product, &expected, &record, at);
}

UpdateStateStore::WriteResult UpdateStateStore::clear(
    const UpdateStateRecord& expected)
{
    return writeRecord(expected.product, &expected, nullptr, {});
}

}
