#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ProjectSerializer.h"
#include "AssetPipeline/PathUtils.h"
#include "ProjectTypes.h"
#include "PackExportSelection.h"
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

TEST(PackGroupSerialization, LegacyGroupsDoNotRestoreLocalExportSelections)
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

    QFile legacy(path);
    ASSERT_TRUE(legacy.open(QIODevice::ReadOnly));
    auto document = QJsonDocument::fromJson(legacy.readAll()).object();
    legacy.close();
    auto content = document.value(QStringLiteral("content")).toObject();
    content.insert(QStringLiteral("packGroups"), QJsonArray{combat.toJson(), ambience.toJson()});
    content.insert(QStringLiteral("packExportSelectedPlatformIds"), QJsonArray{});
    content.insert(QStringLiteral("actionPackTargeting"), QJsonArray{
        VNX::ActionPackTargeting{combat.actionIds.front(), false, {}}.toJson()});
    document.insert(QStringLiteral("content"), content);
    ASSERT_TRUE(legacy.open(QIODevice::WriteOnly | QIODevice::Truncate));
    legacy.write(QJsonDocument(document).toJson());
    legacy.close();

    VNX::ProjectData loaded;
    ASSERT_TRUE(serializer.loadProject(path, loaded)) << serializer.lastError().toStdString();

    EXPECT_TRUE(loaded.packGroups.empty());
    auto& cache = VNX::PackExportSelectionStore::instance();
    cache.openProject(loaded);
    EXPECT_TRUE(cache.selection().actionTargeting.empty());
    EXPECT_EQ(cache.selection().selectedPlatformIds, loaded.platformIds());
    cache.closeProject();
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

TEST(PackGroupSerialization, ExportSelectionsPersistOnlyInDisposableCache)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto project = projectWithMaster();
    project.name = QStringLiteral("TargetingProject");
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};
    project.projectPath = dir.filePath(QStringLiteral("targeting.hsp"));
    auto action = std::make_shared<VNX::ProjectItem>(QStringLiteral("Voice"), VNX::ProjectItem::Type::Action);
    action->guid = QUuid::createUuid();
    action->objectID = project.generateObjectID();
    action->attachDefaultComponents();
    project.actions.push_back({action, QStringLiteral("Actions")});
    VNX::ProjectSerializer serializer;
    ASSERT_TRUE(serializer.saveProject(project.projectPath, project)) << serializer.lastError().toStdString();
    auto& cache = VNX::PackExportSelectionStore::instance();
    cache.openProject(project);
    auto selection = cache.selection();
    selection.selectedPlatformIds.clear();
    selection.actionTargeting = {{action->runtimeId(), false, {}, QStringLiteral("English")}};
    cache.setSelection(project, selection);
    cache.closeProject();
    cache.openProject(project);
    EXPECT_EQ(cache.selection(), selection);
    EXPECT_FALSE(cache.selection().includesActionPack(action->runtimeId(), project.platformIds().first(), "English"));
    EXPECT_TRUE(cache.selection().includesActionPack(action->runtimeId(), project.platformIds().first(), "Japanese"));
    QFile file(project.projectPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto original = file.readAll();
    const auto content = QJsonDocument::fromJson(original).object().value("content").toObject();
    for (const auto* key : {"packExportCurrentPlatformId", "packExportSelectedPlatformIds", "actionPackTargeting", "packGroups"})
        EXPECT_FALSE(content.contains(QLatin1String(key)));
    cache.closeProject();
    ASSERT_TRUE(QFile::remove(dir.filePath(".cache/pack-export-selection.json")));
    cache.openProject(project);
    EXPECT_EQ(cache.selection().selectedPlatformIds, project.platformIds());
    EXPECT_TRUE(cache.selection().actionTargeting.empty());
    file.seek(0);
    EXPECT_EQ(file.readAll(), original);
    cache.closeProject();
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
    project.packBuildOutputDir = dir.filePath(QStringLiteral("Exports/Packs"));
    project.buildExportProjectData = true;
    project.buildExportPackMetadata = true;
    project.buildExportWaveforms = true;
    project.buildWaveformOutputDir = dir.filePath(QStringLiteral("Exports/Waveforms"));

    VNX::ProjectSerializer serializer;
    const QString path = dir.filePath(QStringLiteral("paths.hsp"));
    ASSERT_TRUE(serializer.saveProject(path, project)) << serializer.lastError().toStdString();
    ASSERT_TRUE(HarkenTest::writeRootBusForProject(path));

    VNX::ProjectData loaded;
    ASSERT_TRUE(serializer.loadProject(path, loaded)) << serializer.lastError().toStdString();
    EXPECT_EQ(loaded.packBuildOutputDir, QStringLiteral("Exports/Packs"));
    EXPECT_EQ(loaded.buildExportProjectData, project.buildExportProjectData);
    EXPECT_EQ(loaded.buildExportPackMetadata, project.buildExportPackMetadata);
    EXPECT_EQ(VNX::PathUtils::packBuildOutputRoot(loaded.projectPath, loaded.packBuildOutputDir),
              dir.filePath(QStringLiteral("Exports/Packs")));
    EXPECT_EQ(loaded.buildExportWaveforms, project.buildExportWaveforms);
    EXPECT_EQ(loaded.buildWaveformOutputDir, QStringLiteral("Exports/Waveforms"));
    EXPECT_EQ(VNX::PathUtils::waveformBuildOutputRoot(loaded.projectPath, loaded.buildWaveformOutputDir),
              dir.filePath(QStringLiteral("Exports/Waveforms")));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonObject content = document.value(QStringLiteral("content")).toObject();
    content.remove(QStringLiteral("packBuildOutputDir"));
    content.remove(QStringLiteral("buildExportProjectData"));
    content.remove(QStringLiteral("buildExportPackMetadata"));
    content.remove(QStringLiteral("buildExportWaveforms"));
    content.remove(QStringLiteral("buildWaveformOutputDir"));
    document.insert(QStringLiteral("content"), content);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(document).toJson());
    file.close();
    ASSERT_TRUE(serializer.loadProject(path, loaded)) << serializer.lastError().toStdString();
    EXPECT_TRUE(loaded.packBuildOutputDir.isEmpty());
    EXPECT_FALSE(loaded.buildExportProjectData);
    EXPECT_FALSE(loaded.buildExportPackMetadata);
    EXPECT_FALSE(loaded.buildExportWaveforms);
    EXPECT_TRUE(loaded.buildWaveformOutputDir.isEmpty());
    EXPECT_EQ(VNX::PathUtils::waveformBuildOutputRoot(loaded.projectPath, loaded.buildWaveformOutputDir),
              dir.filePath(QStringLiteral("BuildPacks/Waveforms")));
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
