#include "Integrations/EngineTargetStore.h"
#include "Integrations/Unity/UnityInstallation.h"

#include <QAtomicInt>
#include <QCoreApplication>
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
    return QStringLiteral("HarkenHubEngineTargets%1")
        .arg(counter.fetchAndAddRelaxed(1));
}

QString kindName(EngineKind kind)
{
    return kind == EngineKind::Unity ? QStringLiteral("unity")
                                     : QStringLiteral("unreal");
}

EngineKind parseKind(const QString& value)
{
    return value == QStringLiteral("unreal") ? EngineKind::Unreal
                                              : EngineKind::Unity;
}

}

EngineTargetStore::~EngineTargetStore()
{
    close();
}

bool EngineTargetStore::open(const QString& databasePath)
{
    close();
    const QFileInfo info(databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        lastError_ = hubTranslate(QStringLiteral("Cannot create directory: %1"))
                         .arg(info.absolutePath());
        return false;
    }
    connectionName_ = nextConnectionName();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                 connectionName_);
    db.setDatabaseName(databasePath);
    if (!db.open()) {
        lastError_ = db.lastError().text();
        QSqlDatabase::removeDatabase(connectionName_);
        connectionName_.clear();
        return false;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS engineTargets ("
            "targetId TEXT PRIMARY KEY,"
            "engineKind TEXT NOT NULL,"
            "canonicalRoot TEXT NOT NULL UNIQUE,"
            "descriptorPath TEXT,"
            "displayName TEXT NOT NULL,"
            "addedAt TEXT NOT NULL,"
            "sdkInstallPath TEXT NOT NULL DEFAULT '')"))) {
        lastError_ = query.lastError().text();
        close();
        return false;
    }
    if (!query.exec(QStringLiteral("PRAGMA table_info(engineTargets)"))) {
        lastError_ = query.lastError().text();
        close();
        return false;
    }
    bool hasInstallPath = false;
    while (query.next())
        hasInstallPath |= query.value(1).toString() == QStringLiteral("sdkInstallPath");
    if (!hasInstallPath && !query.exec(QStringLiteral(
            "ALTER TABLE engineTargets ADD COLUMN sdkInstallPath TEXT NOT NULL DEFAULT ''"))) {
        lastError_ = query.lastError().text();
        close();
        return false;
    }
    lastError_.clear();
    return true;
}

void EngineTargetStore::close()
{
    if (connectionName_.isEmpty())
        return;
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(connectionName_);
    connectionName_.clear();
}

bool EngineTargetStore::add(const EngineTargetRecord& record)
{
    if (connectionName_.isEmpty() || record.targetId.isEmpty()
        || record.canonicalRoot.isEmpty() || record.displayName.isEmpty()) {
        lastError_ = hubTranslate(QStringLiteral("Engine target is incomplete"));
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral(
        "INSERT INTO engineTargets (targetId, engineKind, canonicalRoot, "
        "descriptorPath, displayName, addedAt, sdkInstallPath) VALUES (?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(record.targetId);
    query.addBindValue(kindName(record.engineKind));
    query.addBindValue(record.canonicalRoot);
    query.addBindValue(record.descriptorPath);
    query.addBindValue(record.displayName);
    query.addBindValue(record.addedAt);
    query.addBindValue(record.sdkInstallPath.isNull() ? QStringLiteral("") : record.sdkInstallPath);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    return true;
}

bool EngineTargetStore::remove(const QString& targetId)
{
    if (connectionName_.isEmpty())
        return false;
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral("DELETE FROM engineTargets WHERE targetId = ?"));
    query.addBindValue(targetId);
    return query.exec() && query.numRowsAffected() > 0;
}

std::vector<EngineTargetRecord> EngineTargetStore::list() const
{
    std::vector<EngineTargetRecord> records;
    if (connectionName_.isEmpty()) {
        if (lastError_.isEmpty())
            lastError_ = hubTranslate(QStringLiteral("The game project database is not available"));
        return records;
    }
    lastError_.clear();
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    if (!query.exec(QStringLiteral(
            "SELECT targetId, engineKind, canonicalRoot, descriptorPath, "
            "displayName, addedAt, sdkInstallPath FROM engineTargets "
            "ORDER BY displayName COLLATE NOCASE"))) {
        lastError_ = query.lastError().text();
        return records;
    }
    while (query.next()) {
        EngineTargetRecord record;
        record.targetId = query.value(0).toString();
        record.engineKind = parseKind(query.value(1).toString());
        record.canonicalRoot = query.value(2).toString();
        record.descriptorPath = query.value(3).toString();
        record.displayName = query.value(4).toString();
        record.addedAt = query.value(5).toString();
        record.sdkInstallPath = query.value(6).toString();
        records.push_back(std::move(record));
    }
    return records;
}

bool EngineTargetStore::updateSdkInstallPath(const QString& targetId,
                                            const QString& relativePath)
{
    lastError_.clear();
    if (connectionName_.isEmpty() || !VNX::UnityInstallation::safeRelativePath(relativePath)) {
        lastError_ = hubTranslate(QStringLiteral("Cannot save the Integration Package installation location"));
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral("UPDATE engineTargets SET sdkInstallPath = ? WHERE targetId = ?"));
    query.addBindValue(relativePath);
    query.addBindValue(targetId);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    if (query.numRowsAffected() != 1) {
        lastError_ = hubTranslate(QStringLiteral("The game project record no longer exists"));
        return false;
    }
    return true;
}

}
