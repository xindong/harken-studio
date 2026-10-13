#include "MigrationManager.h"
#include "ProjectSerializer.h"
#include "ProjectOpenProtocol.h"
#include "ProjectUpgradeManager.h"
#include "ProjectTypes.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using VNX::MigrationManager;
using VNX::ProjectData;

TEST(MigrationVersionGate, CurrentVersionIsAccepted)
{
    const QJsonObject root = HarkenTest::projectRoot({});
    ProjectData project;
    EXPECT_TRUE(MigrationManager::instance().validateSchema(root, project));
}

TEST(MigrationVersionGate, NewerVersionIsRejected)
{
    const QJsonObject root = HarkenTest::projectRoot(
        {}, MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION + 1);
    ProjectData project;
    EXPECT_FALSE(MigrationManager::instance().validateSchema(root, project));
}

TEST(MigrationVersionGate, NewerVersionLeavesRootUnmodified)
{
    QJsonObject root = HarkenTest::projectRoot(
        QJsonObject{{QStringLiteral("payload"), QStringLiteral("untouched")}},
        MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION + 1);
    const QJsonObject before = root;
    ProjectData project;
    MigrationManager::instance().validateSchema(root, project);
    EXPECT_TRUE(root == before);
}

TEST(MigrationVersionGate, OlderVersionIsRejected)
{
    const QJsonObject root = HarkenTest::projectRoot({}, 0);
    ProjectData project;
    EXPECT_FALSE(MigrationManager::instance().validateSchema(root, project));
}

TEST(MigrationVersionGate, LoadProjectRejectsNewerFileWithExplicitError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QJsonObject root = HarkenTest::projectRoot(
        QJsonObject{{QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Future")},
        }}},
        MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION + 1);
    QJsonObject header = HarkenTest::projectHeader(root);
    header.insert(QStringLiteral("savedStudioVersion"), VNX::currentStudioVersion());
    root.insert(QStringLiteral("harkenDocument"), header);

    const QString path = dir.filePath(QStringLiteral("Future.hsp"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();

    VNX::ProjectSerializer serializer;
    ProjectData project;
    EXPECT_FALSE(serializer.loadProject(path, project));
    EXPECT_TRUE(serializer.lastError().contains(
        VNX::projectIssueCode(VNX::ProjectIssue::ProjectRequiresNewerStudio)));
    EXPECT_TRUE(serializer.lastError().contains(
        QString::number(MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION + 1)));
    const VNX::ProjectUpgradeAnalysis analysis =
        VNX::ProjectUpgradeManager().analyzeForProjectOpen(path);
    EXPECT_FALSE(analysis.ok);
    EXPECT_EQ(analysis.issue, VNX::ProjectIssue::ProjectRequiresNewerStudio);
    EXPECT_FALSE(analysis.upgradeRequired);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QJsonDocument::fromJson(file.readAll()).object(), root);
}
