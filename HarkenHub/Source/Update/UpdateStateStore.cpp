#include "Update/UpdateStateStore.h"

#include "Platform/PlatformPaths.h"
#include "Update/UpdateFile.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace VNH {

namespace {

QJsonObject loadRoot()
{
    QFile file(UpdateStateStore::filePath());
    if (!file.open(QIODevice::ReadOnly))
        return QJsonObject();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

bool saveRoot(const QJsonObject& root)
{
    return writeUpdateFile(UpdateStateStore::filePath(),
                           QJsonDocument(root).toJson(QJsonDocument::Indented));
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
    UpdateStateRecord record;
    record.product = product;
    const QJsonObject root = loadRoot();
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
    record.releaseSequence =
        entry[QStringLiteral("releaseSequence")].toInteger(-1);
    record.transactionId = entry[QStringLiteral("transactionId")].toString();
    record.failureCode = entry[QStringLiteral("failureCode")].toString();
    record.updatedAt = entry[QStringLiteral("updatedAt")].toString();
    return record;
}

bool UpdateStateStore::save(const UpdateStateRecord& record, const QDateTime& at)
{
    if (record.product.isEmpty() || record.state.isEmpty())
        return false;
    QJsonObject root = loadRoot();
    QJsonObject entry;
    entry[QStringLiteral("state")] = record.state;
    entry[QStringLiteral("version")] = record.version;
    entry[QStringLiteral("buildId")] = record.buildId;
    entry[QStringLiteral("downloadPath")] = record.downloadPath;
    entry[QStringLiteral("manifestEnvelopePath")] = record.manifestEnvelopePath;
    entry[QStringLiteral("expectedSha256")] = record.expectedSha256;
    entry[QStringLiteral("packageSize")] = record.packageSize;
    entry[QStringLiteral("downloadedBytes")] = record.downloadedBytes;
    entry[QStringLiteral("releaseSequence")] = record.releaseSequence;
    if (!record.transactionId.isEmpty())
        entry[QStringLiteral("transactionId")] = record.transactionId;
    if (!record.failureCode.isEmpty())
        entry[QStringLiteral("failureCode")] = record.failureCode;
    entry[QStringLiteral("updatedAt")] = at.toUTC().toString(Qt::ISODateWithMs);
    root[record.product] = entry;
    return saveRoot(root);
}

bool UpdateStateStore::clear(const QString& product)
{
    QJsonObject root = loadRoot();
    if (!root.contains(product))
        return true;
    root.remove(product);
    return saveRoot(root);
}

}
