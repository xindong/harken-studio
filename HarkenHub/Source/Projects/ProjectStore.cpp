#include "Projects/ProjectStore.h"

#include <QAtomicInt>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace VNH {

namespace {

QString nextConnectionName()
{
    static QAtomicInt counter(0);
    return QStringLiteral("HarkenHubProjects%1").arg(counter.fetchAndAddRelaxed(1));
}

}

ProjectStore::~ProjectStore()
{
    close();
}

bool ProjectStore::open(const QString& databasePath)
{
    close();

    const QFileInfo info(databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        lastError_ = QStringLiteral("Cannot create directory: %1").arg(info.absolutePath());
        return false;
    }

    connectionName_ = nextConnectionName();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    db.setDatabaseName(databasePath);
    if (!db.open()) {
        lastError_ = db.lastError().text();
        QSqlDatabase::removeDatabase(connectionName_);
        connectionName_.clear();
        return false;
    }

    QSqlQuery query(db);
    const bool created = query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS projects ("
        "canonicalPath TEXT PRIMARY KEY,"
        "projectId TEXT,"
        "displayName TEXT,"
        "detectedSchema INTEGER,"
        "preferredStudioBuildId TEXT,"
        "lastKnownGoodStudioBuildId TEXT,"
        "lastOpenedAt TEXT,"
        "lastPreflightAt TEXT)"));
    if (!created) {
        lastError_ = query.lastError().text();
        close();
        return false;
    }

    return true;
}

void ProjectStore::close()
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

bool ProjectStore::isOpen() const
{
    return !connectionName_.isEmpty();
}

bool ProjectStore::addProject(const ProjectRecord& record)
{
    if (!isOpen()) {
        lastError_ = QStringLiteral("Store is not open");
        return false;
    }
    if (contains(record.canonicalPath)) {
        lastError_ = QStringLiteral("Project is already in the list");
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO projects (canonicalPath, projectId, displayName, detectedSchema,"
        " preferredStudioBuildId, lastKnownGoodStudioBuildId, lastOpenedAt, lastPreflightAt)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(record.canonicalPath);
    query.addBindValue(record.projectId);
    query.addBindValue(record.displayName);
    query.addBindValue(record.detectedSchema);
    query.addBindValue(record.preferredStudioBuildId);
    query.addBindValue(record.lastKnownGoodStudioBuildId);
    query.addBindValue(record.lastOpenedAt);
    query.addBindValue(record.lastPreflightAt);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    return true;
}

bool ProjectStore::removeProject(const QString& canonicalPath)
{
    if (!isOpen()) {
        lastError_ = QStringLiteral("Store is not open");
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM projects WHERE canonicalPath = ?"));
    query.addBindValue(canonicalPath);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool ProjectStore::contains(const QString& canonicalPath) const
{
    if (connectionName_.isEmpty())
        return false;
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT 1 FROM projects WHERE canonicalPath = ?"));
    query.addBindValue(canonicalPath);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    return query.next();
}

bool ProjectStore::setPreferredBuild(const QString& canonicalPath, const QString& buildId)
{
    if (!isOpen()) {
        lastError_ = QStringLiteral("Store is not open");
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "UPDATE projects SET preferredStudioBuildId = ? WHERE canonicalPath = ?"));
    query.addBindValue(buildId);
    query.addBindValue(canonicalPath);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool ProjectStore::setLastKnownGood(const QString& canonicalPath,
                                    const QString& buildId, const QString& at)
{
    if (!isOpen()) {
        lastError_ = QStringLiteral("Store is not open");
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "UPDATE projects SET lastKnownGoodStudioBuildId = ?, lastOpenedAt = ?"
        " WHERE canonicalPath = ?"));
    query.addBindValue(buildId);
    query.addBindValue(at);
    query.addBindValue(canonicalPath);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool ProjectStore::recordSuccessfulOpen(const QString& canonicalPath,
                                        const QString& projectId,
                                        const QString& displayName,
                                        int detectedSchema,
                                        const QString& buildId,
                                        const QString& at)
{
    if (!isOpen()) {
        lastError_ = QStringLiteral("Store is not open");
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "UPDATE projects SET projectId = ?, displayName = ?, detectedSchema = ?,"
        " lastKnownGoodStudioBuildId = ?, lastOpenedAt = ?"
        " WHERE canonicalPath = ?"));
    query.addBindValue(projectId);
    query.addBindValue(displayName);
    query.addBindValue(detectedSchema);
    query.addBindValue(buildId);
    query.addBindValue(at);
    query.addBindValue(canonicalPath);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool ProjectStore::setLastOpened(const QString& canonicalPath,
                                 const QString& at)
{
    if (!isOpen()) {
        lastError_ = QStringLiteral("Store is not open");
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "UPDATE projects SET lastOpenedAt = ? WHERE canonicalPath = ?"));
    query.addBindValue(at);
    query.addBindValue(canonicalPath);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool ProjectStore::setLastPreflight(const QString& canonicalPath,
                                    const QString& at)
{
    if (!isOpen()) {
        lastError_ = QStringLiteral("Store is not open");
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "UPDATE projects SET lastPreflightAt = ? WHERE canonicalPath = ?"));
    query.addBindValue(at);
    query.addBindValue(canonicalPath);
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

std::vector<ProjectRecord> ProjectStore::listProjects() const
{
    std::vector<ProjectRecord> records;
    if (connectionName_.isEmpty())
        return records;

    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT canonicalPath, projectId, displayName, detectedSchema,"
            " preferredStudioBuildId, lastKnownGoodStudioBuildId, lastOpenedAt,"
            " lastPreflightAt FROM projects ORDER BY displayName COLLATE NOCASE"))) {
        lastError_ = query.lastError().text();
        return records;
    }

    while (query.next()) {
        ProjectRecord record;
        record.canonicalPath = query.value(0).toString();
        record.projectId = query.value(1).toString();
        record.displayName = query.value(2).toString();
        record.detectedSchema = query.value(3).toInt();
        record.preferredStudioBuildId = query.value(4).toString();
        record.lastKnownGoodStudioBuildId = query.value(5).toString();
        record.lastOpenedAt = query.value(6).toString();
        record.lastPreflightAt = query.value(7).toString();
        records.push_back(std::move(record));
    }
    return records;
}

}
