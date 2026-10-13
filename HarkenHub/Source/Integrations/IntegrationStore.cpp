#include "Integrations/IntegrationStore.h"

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace VNH {
namespace {
QString hubTranslate(const QString& source)
{
    const QByteArray utf8 = source.toUtf8();
    return QCoreApplication::translate("HarkenHub", utf8.constData());
}

QString nextConnectionName()
{
    static QAtomicInt counter(0);
    return QStringLiteral("HarkenHubIntegrations%1")
        .arg(counter.fetchAndAddRelaxed(1));
}
}

IntegrationStore::~IntegrationStore()
{
    close();
}

bool IntegrationStore::open(const QString& databasePath)
{
    close();
    const QFileInfo info(databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        lastError_ = hubTranslate(QStringLiteral("Cannot create integration store directory"));
        return false;
    }
    connectionName_ = nextConnectionName();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                 connectionName_);
    db.setDatabaseName(databasePath);
    if (!db.open()) {
        lastError_ = db.lastError().text();
        connectionName_.clear();
        return false;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS integrations ("
            "integrationId TEXT PRIMARY KEY, "
            "harkenProjectPath TEXT NOT NULL, "
            "engineTargetId TEXT NOT NULL, "
            "sdkSourcePath TEXT NOT NULL DEFAULT '', "
            "createdAt TEXT NOT NULL, updatedAt TEXT NOT NULL, "
            "enabled INTEGER NOT NULL, "
            "UNIQUE(engineTargetId))"))) {
        lastError_ = query.lastError().text();
        close();
        return false;
    }
    bool hasSdkSourcePath = false;
    if (!query.exec(QStringLiteral("PRAGMA table_info(integrations)"))) {
        lastError_ = query.lastError().text();
        close();
        return false;
    }
    while (query.next()) {
        if (query.value(1).toString() == QStringLiteral("sdkSourcePath")) {
            hasSdkSourcePath = true;
            break;
        }
    }
    if (!hasSdkSourcePath
        && !query.exec(QStringLiteral(
            "ALTER TABLE integrations ADD COLUMN "
            "sdkSourcePath TEXT NOT NULL DEFAULT ''"))) {
        lastError_ = query.lastError().text();
        close();
        return false;
    }
    return true;
}

void IntegrationStore::close()
{
    if (connectionName_.isEmpty()) return;
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(connectionName_);
    connectionName_.clear();
}

bool IntegrationStore::add(const IntegrationRecord& record, bool replaceTarget)
{
    if (connectionName_.isEmpty() || record.integrationId.isEmpty()
        || record.harkenProjectPath.isEmpty() || record.engineTargetId.isEmpty()) {
        lastError_ = hubTranslate(QStringLiteral("Integration is incomplete"));
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    if (!db.transaction()) {
        lastError_ = db.lastError().text();
        return false;
    }
    if (replaceTarget) {
        QSqlQuery remove(db);
        remove.prepare(QStringLiteral(
            "DELETE FROM integrations WHERE engineTargetId = ?"));
        remove.addBindValue(record.engineTargetId);
        if (!remove.exec()) {
            lastError_ = remove.lastError().text();
            db.rollback();
            return false;
        }
    }
    QSqlQuery duplicate(db);
    duplicate.prepare(QStringLiteral(
        "SELECT harkenProjectPath, engineTargetId FROM integrations "
        "WHERE harkenProjectPath = ? OR engineTargetId = ? LIMIT 1"));
    duplicate.addBindValue(record.harkenProjectPath);
    duplicate.addBindValue(record.engineTargetId);
    if (!duplicate.exec()) {
        lastError_ = duplicate.lastError().text();
        db.rollback();
        return false;
    }
    if (duplicate.next()) {
        lastError_ = duplicate.value(0).toString() == record.harkenProjectPath
            ? hubTranslate(QStringLiteral("The Harken project already has an integration"))
            : hubTranslate(QStringLiteral("The engine project already has an integration"));
        db.rollback();
        return false;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO integrations (integrationId, harkenProjectPath, "
        "engineTargetId, sdkSourcePath, createdAt, updatedAt, enabled) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(record.integrationId);
    query.addBindValue(record.harkenProjectPath);
    query.addBindValue(record.engineTargetId);
    query.addBindValue(record.sdkSourcePath.isNull()
                           ? QStringLiteral("")
                           : record.sdkSourcePath);
    query.addBindValue(record.createdAt);
    query.addBindValue(record.updatedAt);
    query.addBindValue(record.enabled ? 1 : 0);
    if (!query.exec() || !db.commit()) {
        lastError_ = query.lastError().text();
        db.rollback();
        return false;
    }
    lastError_.clear();
    return true;
}

bool IntegrationStore::updateSdkSourcePath(
    const QString& integrationId, const QString& sdkSourcePath)
{
    if (connectionName_.isEmpty() || integrationId.isEmpty()) {
        lastError_ = hubTranslate(QStringLiteral("Integration is unavailable"));
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral(
        "UPDATE integrations SET sdkSourcePath = ?, updatedAt = ? "
        "WHERE integrationId = ?"));
    query.addBindValue(sdkSourcePath);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    query.addBindValue(integrationId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        lastError_ = query.lastError().text();
        if (lastError_.isEmpty())
            lastError_ = hubTranslate(QStringLiteral("Integration is unavailable"));
        return false;
    }
    lastError_.clear();
    return true;
}

bool IntegrationStore::remove(const QString& integrationId)
{
    if (connectionName_.isEmpty() || integrationId.isEmpty()) {
        lastError_ = hubTranslate(QStringLiteral("Integration is unavailable"));
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral("DELETE FROM integrations WHERE integrationId = ?"));
    query.addBindValue(integrationId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        lastError_ = query.lastError().text();
        if (lastError_.isEmpty())
            lastError_ = hubTranslate(QStringLiteral("Integration is unavailable"));
        return false;
    }
    lastError_.clear();
    return true;
}

std::vector<IntegrationRecord> IntegrationStore::list() const
{
    std::vector<IntegrationRecord> records;
    if (connectionName_.isEmpty()) {
        if (lastError_.isEmpty())
            lastError_ = QStringLiteral("The integration database is not available");
        return records;
    }
    lastError_.clear();
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    if (!query.exec(QStringLiteral(
            "SELECT integrationId, harkenProjectPath, engineTargetId, "
            "sdkSourcePath, createdAt, updatedAt, enabled "
            "FROM integrations ORDER BY createdAt"))) {
        lastError_ = query.lastError().text();
        return records;
    }
    while (query.next()) {
        records.push_back({query.value(0).toString(), query.value(1).toString(),
                           query.value(2).toString(), query.value(3).toString(),
                           query.value(4).toString(), query.value(5).toString(),
                           query.value(6).toInt() != 0});
    }
    return records;
}

}
