#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ProjectSerializer.h"
#include "AssetPipeline/PathUtils.h"
#include "ProjectTypes.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include "Support/ProjectPlatformTestUtils.h"

#include <memory>

namespace {

VNX::ProjectData projectWithMaster()
{
    VNX::ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "iOS"});
    auto master = std::make_shared<VNX::ProjectItem>(
        QStringLiteral("Master Audio Bus"), VNX::ProjectItem::Type::Bus);
    master->guid = QUuid(QStringLiteral("99999999-9999-4999-8999-999999999999"));
    master->objectID = project.generateObjectID();
    master->guid = QUuid::createUuid();
    master->attachDefaultComponents();
    project.busses.push_back({std::move(master), QStringLiteral("Busses")});
    return project;
}

}

TEST(PackGroupSerialization, LegacyGroupsDissolveWithoutChangingMemberTargeting)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto project = projectWithMaster();
    project.name = QStringLiteral("PackGroupProject");

    VNX::PackGroup combat;
    combat.name = QStringLiteral("Combat");
    combat.actionIds = {harkentest::StudioObjectId(9001u), harkentest::StudioObjectId(9002u), harkentest::StudioObjectId(9003u)};
    combat.allPlatforms = false;
    combat.platformIds = {HarkenTest::platformIdString(QStringLiteral("Windows")), HarkenTest::platformIdString(QStringLiteral("Android"))};
    project.packGroups.push_back(combat);

    VNX::PackGroup ambience;
    ambience.name = QStringLiteral("Ambience");
    ambience.actionIds = {harkentest::StudioObjectId(42u)};
    project.packGroups.push_back(ambience);

    VNX::PackGroup nowhere;
    nowhere.name = QStringLiteral("Nowhere");
    nowhere.allPlatforms = false;
    project.packGroups.push_back(nowhere);

    VNX::ProjectSerializer serializer;
    const QString path = dir.filePath(QStringLiteral("groups.hsp"));
    ASSERT_TRUE(serializer.saveProject(path, project)) << serializer.lastError().toStdString();
    ASSERT_TRUE(HarkenTest::writeRootBusForProject(path));

    VNX::ProjectData loaded;
    ASSERT_TRUE(serializer.loadProject(path, loaded)) << serializer.lastError().toStdString();

    EXPECT_TRUE(loaded.packGroups.empty());
    ASSERT_EQ(loaded.actionPackTargeting.size(), combat.actionIds.size());
    for (const auto id : combat.actionIds) {
        EXPECT_TRUE(loaded.includesActionPack(id, HarkenTest::platformIdString("Windows"), "English"));
        EXPECT_TRUE(loaded.includesActionPack(id, HarkenTest::platformIdString("Android"), "Japanese"));
        EXPECT_FALSE(loaded.includesActionPack(id, HarkenTest::platformIdString("iOS"), "English"));
    }
    EXPECT_TRUE(loaded.includesActionPack(ambience.actionIds.front(), HarkenTest::platformIdString("iOS"), "English"));
    loaded.busses = project.busses;
    ASSERT_TRUE(serializer.saveProject(path, loaded)) << serializer.lastError().toStdString();
    QFile saved(path);
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_FALSE(QJsonDocument::fromJson(saved.readAll()).object().value("content").toObject().contains("packGroups"));

}

TEST(PackGroupSerialization, ExplicitEmptyFilterDoesNotBecomeAllPlatforms)
{
    const VNX::PackGroup group = VNX::PackGroup::fromJson(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("None")},
        {QStringLiteral("actionIds"), QJsonArray{}},
        {QStringLiteral("allPlatforms"), false},
        {QStringLiteral("platformIds"), QJsonArray{}}});
    EXPECT_FALSE(group.allPlatforms);
    EXPECT_TRUE(group.platformIds.isEmpty());
    EXPECT_FALSE(group.includesPlatform(HarkenTest::platformIdString(QStringLiteral("Windows"))));
}

TEST(PackGroupSerialization, ActionPackTargetingRoundTripPreservesExplicitFilters)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto project = projectWithMaster();
    project.name = QStringLiteral("TargetingProject");
    project.actionPackTargeting.push_back(
        {harkentest::StudioObjectId(7001u), false, {HarkenTest::platformIdString(QStringLiteral("Windows")), HarkenTest::platformIdString(QStringLiteral("iOS"))}});
    project.actionPackTargeting.push_back({harkentest::StudioObjectId(7002u), true, {}});
    project.actionPackTargeting.push_back({harkentest::StudioObjectId(7003u), false, {}});
    project.actionPackTargeting.push_back({harkentest::StudioObjectId(7001u), false,
        {HarkenTest::platformIdString("Android")}, QStringLiteral("English")});
    project.actionPackTargeting.push_back({harkentest::StudioObjectId(7003u), true, {}, QStringLiteral("Japanese")});

    VNX::ProjectSerializer serializer;
    const QString path = dir.filePath(QStringLiteral("targeting.hsp"));
    ASSERT_TRUE(serializer.saveProject(path, project)) << serializer.lastError().toStdString();
    ASSERT_TRUE(HarkenTest::writeRootBusForProject(path));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const QJsonObject content = root.value(QStringLiteral("content")).toObject();
    EXPECT_EQ(content.value(QStringLiteral("actionPackTargeting")).toArray().size(), 5);

    VNX::ProjectData loaded;
    ASSERT_TRUE(serializer.loadProject(path, loaded)) << serializer.lastError().toStdString();

    ASSERT_EQ(loaded.actionPackTargeting.size(), 5u);
    EXPECT_EQ(loaded.actionPackTargeting, project.actionPackTargeting);
    EXPECT_FALSE(loaded.includesActionPack(harkentest::StudioObjectId(7001u), HarkenTest::platformIdString("Windows"), "English"));
    EXPECT_TRUE(loaded.includesActionPack(harkentest::StudioObjectId(7001u), HarkenTest::platformIdString("Windows"), "Japanese"));
    EXPECT_TRUE(loaded.includesActionPack(harkentest::StudioObjectId(7003u), HarkenTest::platformIdString("Windows"), "Japanese"));
    EXPECT_FALSE(loaded.includesActionPack(harkentest::StudioObjectId(7003u), HarkenTest::platformIdString("Windows"), "English"));
    EXPECT_EQ(loaded.actionPackTargeting[0].stableId, harkentest::StudioObjectId(7001u));
    EXPECT_FALSE(loaded.actionPackTargeting[0].allPlatforms);
    EXPECT_EQ(loaded.actionPackTargeting[0].platformIds,
              (QStringList{HarkenTest::platformIdString(QStringLiteral("Windows")), HarkenTest::platformIdString(QStringLiteral("iOS"))}));
    EXPECT_EQ(loaded.actionPackTargeting[2].stableId, harkentest::StudioObjectId(7003u));
    EXPECT_FALSE(loaded.actionPackTargeting[2].allPlatforms);
    EXPECT_TRUE(loaded.actionPackTargeting[2].platformIds.isEmpty());
    EXPECT_FALSE(loaded.actionPackTargeting[2].includesPlatform(HarkenTest::platformIdString(QStringLiteral("Windows"))));
}

TEST(PackGroupSerialization, MissingPackGroupsKeyLoadsAsEmpty)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto project = projectWithMaster();
    project.name = QStringLiteral("EmptyPackGroupsProject");

    VNX::ProjectSerializer serializer;
    const QString path = dir.filePath(QStringLiteral("empty-pack-groups.hsp"));
    ASSERT_TRUE(serializer.saveProject(path, project)) << serializer.lastError().toStdString();
    ASSERT_TRUE(HarkenTest::writeRootBusForProject(path));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const QJsonObject content = root.value(QStringLiteral("content")).toObject();
    EXPECT_FALSE(content.contains(QStringLiteral("packGroups")));
    EXPECT_EQ(HarkenTest::projectHeader(root).value(QStringLiteral("propertyModel")).toString(),
              QStringLiteral("fieldGroupsV1"));

    VNX::ProjectData loaded;
    ASSERT_TRUE(serializer.loadProject(path, loaded)) << serializer.lastError().toStdString();
    EXPECT_TRUE(loaded.packGroups.empty());
}

TEST(PackGroupSerialization, PackBuildDirectoryRoundTripsWithoutLegacyDeploymentSettings)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto project = projectWithMaster();
    project.name = QStringLiteral("PackPathsProject");
    project.packBuildOutputDir = QStringLiteral("Exports/Packs");

    VNX::ProjectSerializer serializer;
    const QString path = dir.filePath(QStringLiteral("paths.hsp"));
    ASSERT_TRUE(serializer.saveProject(path, project)) << serializer.lastError().toStdString();
    ASSERT_TRUE(HarkenTest::writeRootBusForProject(path));

    VNX::ProjectData loaded;
    ASSERT_TRUE(serializer.loadProject(path, loaded)) << serializer.lastError().toStdString();
    EXPECT_EQ(loaded.packBuildOutputDir, project.packBuildOutputDir);
    EXPECT_EQ(VNX::PathUtils::packBuildOutputRoot(loaded.projectPath, loaded.packBuildOutputDir),
              dir.filePath(QStringLiteral("Exports/Packs")));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonObject content = document.value(QStringLiteral("content")).toObject();
    content.remove(QStringLiteral("packBuildOutputDir"));
    EXPECT_FALSE(content.contains(QStringLiteral("unityIntegration")));
    EXPECT_FALSE(content.contains(QStringLiteral("unityPackDeployDir")));
    EXPECT_FALSE(content.contains(QStringLiteral("packOutputDir")));
    content.insert(QStringLiteral("unityIntegration"), QJsonObject{
        {QStringLiteral("projectPath"), dir.filePath(QStringLiteral("Game"))},
        {QStringLiteral("autoDeploy"), true}});
    content.insert(QStringLiteral("packOutputDir"), QStringLiteral("Assets/Audio/Packs"));
    content.insert(QStringLiteral("packExportPlatformOutputRoots"),
                   QJsonObject{{HarkenTest::platformIdString(QStringLiteral("Windows")), QStringLiteral("OldPacks/Windows")}});
    document.insert(QStringLiteral("content"), content);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(document).toJson());
    file.close();

    VNX::ProjectData legacy;
    const auto before = QFileInfo(path).size();
    EXPECT_FALSE(serializer.loadProject(path, legacy));
    EXPECT_TRUE(serializer.lastError().contains(QStringLiteral("Legacy platform field")));
    EXPECT_EQ(QFileInfo(path).size(), before);
}
