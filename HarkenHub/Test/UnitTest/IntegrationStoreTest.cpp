#include "Integrations/IntegrationStore.h"
#include "Integrations/EngineTargetStore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

namespace {

VNH::IntegrationRecord record(const QString& id, const QString& project,
                              const QString& target)
{
    VNH::IntegrationRecord value;
    value.integrationId = id;
    value.harkenProjectPath = project;
    value.engineTargetId = target;
    value.createdAt = QStringLiteral("2026-08-18T00:00:00Z");
    value.updatedAt = value.createdAt;
    return value;
}

}

TEST(IntegrationStoreTest, EnforcesOneToOneProjectRelationships)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNH::IntegrationStore store;
    ASSERT_TRUE(store.open(
        QDir(directory.path()).filePath(QStringLiteral("integrations.db"))));

    ASSERT_TRUE(store.add(record(QStringLiteral("one"),
                                 QStringLiteral("C:/Harken/A.hsp"),
                                 QStringLiteral("unity-a")), false))
        << store.lastError().toStdString();

    EXPECT_FALSE(store.add(record(QStringLiteral("two"),
                                 QStringLiteral("C:/Harken/A.hsp"),
                                 QStringLiteral("unity-b")), false));
    EXPECT_TRUE(store.lastError().contains(QStringLiteral("Harken project")));

    EXPECT_FALSE(store.add(record(QStringLiteral("three"),
                                 QStringLiteral("C:/Harken/B.hsp"),
                                 QStringLiteral("unity-a")), false));
    EXPECT_TRUE(store.lastError().contains(QStringLiteral("engine project")));

    ASSERT_TRUE(store.remove(QStringLiteral("one")));
    EXPECT_TRUE(store.add(record(QStringLiteral("four"),
                                QStringLiteral("C:/Harken/A.hsp"),
                                QStringLiteral("unity-a")), false));
}

TEST(IntegrationStoreTest, PersistsAndUpdatesSdkSourcePath)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNH::IntegrationStore store;
    ASSERT_TRUE(store.open(
        QDir(directory.path()).filePath(QStringLiteral("integrations.db"))));

    auto value = record(QStringLiteral("one"),
                        QStringLiteral("C:/Harken/A.hsp"),
                        QStringLiteral("unity-a"));
    value.sdkSourcePath = QStringLiteral("C:/SDK/one");
    ASSERT_TRUE(store.add(value, false));
    ASSERT_EQ(store.list().size(), 1u);
    EXPECT_EQ(store.list().front().sdkSourcePath,
              QStringLiteral("C:/SDK/one"));

    ASSERT_TRUE(store.updateSdkSourcePath(QStringLiteral("one"),
                                          QStringLiteral("C:/SDK/two")));
    ASSERT_EQ(store.list().size(), 1u);
    EXPECT_EQ(store.list().front().sdkSourcePath,
              QStringLiteral("C:/SDK/two"));
}

TEST(IntegrationStoreTest, MigratesExistingDatabaseForSdkSourcePath)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString databasePath =
        QDir(directory.path()).filePath(QStringLiteral("integrations.db"));
    const QString connectionName =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        ASSERT_TRUE(database.open());
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE integrations ("
            "integrationId TEXT PRIMARY KEY, "
            "harkenProjectPath TEXT NOT NULL, "
            "engineTargetId TEXT NOT NULL, "
            "createdAt TEXT NOT NULL, updatedAt TEXT NOT NULL, "
            "enabled INTEGER NOT NULL, UNIQUE(engineTargetId))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO integrations VALUES ("
            "'one', 'C:/Harken/A.hsp', 'unity-a', "
            "'2026-08-18T00:00:00Z', '2026-08-18T00:00:00Z', 1)")));
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    VNH::IntegrationStore store;
    ASSERT_TRUE(store.open(databasePath))
        << store.lastError().toStdString();
    ASSERT_EQ(store.list().size(), 1u);
    EXPECT_TRUE(store.list().front().sdkSourcePath.isEmpty());
    EXPECT_TRUE(store.updateSdkSourcePath(QStringLiteral("one"),
                                          QStringLiteral("C:/SDK")));
    EXPECT_EQ(store.list().front().sdkSourcePath,
              QStringLiteral("C:/SDK"));
}

TEST(IntegrationStoreTest, MigratesAndPersistsPerGameSdkLocationsIndependentlyOfRelationships)
{
    QTemporaryDir directory(QDir::current().filePath(QStringLiteral(".temp/sdk-store-XXXXXX")));
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("targets.db"));
    const QString connectionName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(path);
        ASSERT_TRUE(database.open());
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE engineTargets (targetId TEXT PRIMARY KEY, engineKind TEXT NOT NULL, "
            "canonicalRoot TEXT NOT NULL UNIQUE, descriptorPath TEXT, displayName TEXT NOT NULL, addedAt TEXT NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO engineTargets VALUES ('game', 'unity', 'D:/Game', '', 'Game', '2026-09-21')")));
    }
    QSqlDatabase::removeDatabase(connectionName);
    VNH::EngineTargetStore targets;
    ASSERT_TRUE(targets.open(path));
    ASSERT_EQ(targets.list().size(), 1);
    EXPECT_TRUE(targets.list().front().sdkInstallPath.isEmpty());
    const QString location = QStringLiteral("Assets/ThirdParty/HarkenSDK");
    ASSERT_TRUE(targets.updateSdkInstallPath(QStringLiteral("game"), location));
    VNH::IntegrationStore integrations;
    ASSERT_TRUE(integrations.open(directory.filePath(QStringLiteral("integrations.db"))));
    ASSERT_TRUE(integrations.add(record(QStringLiteral("link"), QStringLiteral("D:/Audio.hsp"), QStringLiteral("game")), false));
    ASSERT_TRUE(integrations.remove(QStringLiteral("link")));
    targets.close();
    ASSERT_TRUE(targets.open(path));
    EXPECT_EQ(targets.list().front().sdkInstallPath, location);
    EXPECT_FALSE(targets.updateSdkInstallPath(QStringLiteral("game"), QStringLiteral("../Outside")));
    EXPECT_EQ(targets.list().front().sdkInstallPath, location);
    EXPECT_FALSE(targets.updateSdkInstallPath(QStringLiteral("missing"), location));
    EXPECT_FALSE(targets.lastError().isEmpty());
}
