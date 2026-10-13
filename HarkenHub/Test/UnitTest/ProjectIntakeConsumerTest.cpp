#include "Projects/ProjectIntakeConsumer.h"

#include "Projects/ProjectStore.h"
#include "Projects/ProjectVersionSelection.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using VNH::ProjectIntakeConsumer;
using VNH::ProjectRecord;
using VNH::ProjectStore;

namespace {

class ProjectIntakeConsumerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
        ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("projects.db"))));
    }

    void TearDown() override { qunsetenv("HARKEN_HUB_USER_ROOT"); }

    QString writeProjectFile(const QString& name, bool withSavedVersion = true)
    {
        const QString projectId =
            QStringLiteral("11111111-2222-3333-4444-555555555555");
        QJsonObject root{
            {QStringLiteral("harkenDocument"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("project.root")},
                {QStringLiteral("id"), projectId},
                {QStringLiteral("projectId"), projectId},
                {QStringLiteral("schemaVersion"), 5},
                {QStringLiteral("savedStudioVersion"), QStringLiteral("1.4.0")},
            }},
            {QStringLiteral("content"), QJsonObject{
                {QStringLiteral("metadata"), QJsonObject{
                    {QStringLiteral("name"), name},
                }},
            }},
        };
        if (!withSavedVersion) {
            auto header = root.value(QStringLiteral("harkenDocument")).toObject();
            header.remove(QStringLiteral("savedStudioVersion"));
            root.insert(QStringLiteral("harkenDocument"), header);
        }
        const QString path = dir.filePath(name + QStringLiteral(".hsp"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return QString();
        file.write(QJsonDocument(root).toJson());
        file.close();
        return QFileInfo(path).canonicalFilePath();
    }

    void writeIntakeRecord(const QString& fileName, const QJsonObject& record)
    {
        const QString directory = ProjectIntakeConsumer::intakeDir();
        ASSERT_TRUE(QDir().mkpath(directory));
        QFile file(directory + QStringLiteral("/") + fileName);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
    }

    QStringList intakeFiles() const
    {
        return QDir(ProjectIntakeConsumer::intakeDir())
            .entryList({QStringLiteral("*.json")}, QDir::Files);
    }

    QTemporaryDir dir;
    ProjectStore store;
};

}

TEST_F(ProjectIntakeConsumerTest, RegistersUnknownProject)
{
    const QString projectPath = writeProjectFile(QStringLiteral("Alpha"));
    ASSERT_FALSE(projectPath.isEmpty());
    QJsonObject record;
    record[QStringLiteral("projectPath")] = projectPath;
    record[QStringLiteral("buildId")] = QStringLiteral("run1.aaaa1111");
    record[QStringLiteral("openedAt")] =
        QStringLiteral("2026-07-28T03:00:00.000Z");
    writeIntakeRecord(QStringLiteral("alpha.json"), record);

    ProjectIntakeConsumer consumer(&store);
    EXPECT_TRUE(consumer.consumeAll());

    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().canonicalPath, projectPath);
    EXPECT_EQ(records.front().displayName, QStringLiteral("Alpha"));
    EXPECT_EQ(records.front().detectedSchema, 5);
    EXPECT_EQ(records.front().lastKnownGoodStudioBuildId,
              QStringLiteral("run1.aaaa1111"));
    EXPECT_EQ(records.front().lastOpenedAt,
              QStringLiteral("2026-07-28T03:00:00.000Z"));
    EXPECT_TRUE(intakeFiles().isEmpty());
}

TEST_F(ProjectIntakeConsumerTest, UpdatesExistingProjectWithoutTouchingPreferred)
{
    const QString projectPath = writeProjectFile(QStringLiteral("Beta"));
    ASSERT_FALSE(projectPath.isEmpty());
    ProjectRecord seed;
    seed.canonicalPath = projectPath;
    seed.displayName = QStringLiteral("Beta");
    ASSERT_TRUE(store.addProject(seed));
    ASSERT_TRUE(store.setPreferredBuild(projectPath,
                                        QStringLiteral("run1.pinned")));

    QJsonObject record;
    record[QStringLiteral("projectPath")] = projectPath;
    record[QStringLiteral("buildId")] = QStringLiteral("run2.bbbb2222");
    record[QStringLiteral("openedAt")] =
        QStringLiteral("2026-07-28T04:00:00.000Z");
    writeIntakeRecord(QStringLiteral("beta.json"), record);

    ProjectIntakeConsumer consumer(&store);
    EXPECT_TRUE(consumer.consumeAll());

    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().preferredStudioBuildId,
              QStringLiteral("run1.pinned"));
    EXPECT_EQ(records.front().lastKnownGoodStudioBuildId,
              QStringLiteral("run2.bbbb2222"));
    EXPECT_EQ(records.front().lastOpenedAt,
              QStringLiteral("2026-07-28T04:00:00.000Z"));
}

TEST_F(ProjectIntakeConsumerTest, EmptyBuildIdOnlyUpdatesLastOpened)
{
    const QString projectPath = writeProjectFile(QStringLiteral("Gamma"));
    ASSERT_FALSE(projectPath.isEmpty());
    ProjectRecord seed;
    seed.canonicalPath = projectPath;
    seed.displayName = QStringLiteral("Gamma");
    seed.lastKnownGoodStudioBuildId = QStringLiteral("run1.seed");
    ASSERT_TRUE(store.addProject(seed));

    QJsonObject record;
    record[QStringLiteral("projectPath")] = projectPath;
    record[QStringLiteral("openedAt")] =
        QStringLiteral("2026-07-28T05:00:00.000Z");
    writeIntakeRecord(QStringLiteral("gamma.json"), record);

    ProjectIntakeConsumer consumer(&store);
    EXPECT_TRUE(consumer.consumeAll());

    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().lastKnownGoodStudioBuildId,
              QStringLiteral("run1.seed"));
    EXPECT_EQ(records.front().lastOpenedAt,
              QStringLiteral("2026-07-28T05:00:00.000Z"));
}

TEST_F(ProjectIntakeConsumerTest, DiscardsRecordForMissingProject)
{
    QJsonObject record;
    record[QStringLiteral("projectPath")] =
        dir.filePath(QStringLiteral("Ghost.hsp"));
    record[QStringLiteral("buildId")] = QStringLiteral("run1.aaaa1111");
    writeIntakeRecord(QStringLiteral("ghost.json"), record);

    ProjectIntakeConsumer consumer(&store);
    EXPECT_FALSE(consumer.consumeAll());
    EXPECT_TRUE(store.listProjects().empty());
    EXPECT_TRUE(intakeFiles().isEmpty());
}

TEST_F(ProjectIntakeConsumerTest, DiscardsRecordWithoutAValidObservationTime)
{
    const QString projectPath = writeProjectFile(QStringLiteral("Delta"));
    ASSERT_FALSE(projectPath.isEmpty());
    QJsonObject record;
    record[QStringLiteral("projectPath")] = projectPath;
    record[QStringLiteral("buildId")] = QStringLiteral("../evil");
    record[QStringLiteral("openedAt")] = QStringLiteral("not-a-date");
    writeIntakeRecord(QStringLiteral("delta.json"), record);

    ProjectIntakeConsumer consumer(&store);
    EXPECT_FALSE(consumer.consumeAll());
    EXPECT_TRUE(store.listProjects().empty());
    EXPECT_TRUE(intakeFiles().isEmpty());
}

TEST_F(ProjectIntakeConsumerTest, KeepsRecordWhileStoreUnavailable)
{
    const QString projectPath = writeProjectFile(QStringLiteral("Epsilon"));
    ASSERT_FALSE(projectPath.isEmpty());
    QJsonObject record;
    record[QStringLiteral("projectPath")] = projectPath;
    record[QStringLiteral("buildId")] = QStringLiteral("run1.aaaa1111");
    record[QStringLiteral("openedAt")] =
        QStringLiteral("2026-07-28T06:00:00.000Z");
    writeIntakeRecord(QStringLiteral("epsilon.json"), record);

    ProjectStore closedStore;
    ProjectIntakeConsumer blockedConsumer(&closedStore);
    EXPECT_FALSE(blockedConsumer.consumeAll());
    EXPECT_EQ(intakeFiles().size(), 1);
    EXPECT_TRUE(store.listProjects().empty());

    ProjectIntakeConsumer consumer(&store);
    EXPECT_TRUE(consumer.consumeAll());
    EXPECT_TRUE(intakeFiles().isEmpty());
    ASSERT_EQ(store.listProjects().size(), 1u);
}

TEST_F(ProjectIntakeConsumerTest, RemovesMalformedRecordFiles)
{
    const QString directory = ProjectIntakeConsumer::intakeDir();
    ASSERT_TRUE(QDir().mkpath(directory));
    QFile file(directory + QStringLiteral("/broken.json"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{not json");
    file.close();

    ProjectIntakeConsumer consumer(&store);
    EXPECT_FALSE(consumer.consumeAll());
    EXPECT_TRUE(intakeFiles().isEmpty());
}

TEST_F(ProjectIntakeConsumerTest, OldObservationCannotOverwriteNewerSuccessOrPreference)
{
    const QString path = writeProjectFile(QStringLiteral("Recent"));
    ProjectRecord seed;
    seed.canonicalPath = path;
    seed.preferredStudioBuildId = QStringLiteral("user-selection");
    seed.lastKnownGoodStudioBuildId = QStringLiteral("newer-success");
    seed.lastOpenedAt = QStringLiteral("2026-09-08T10:00:00.000Z");
    ASSERT_TRUE(store.addProject(seed));
    writeIntakeRecord(QStringLiteral("old.json"), {
        {QStringLiteral("projectPath"), path},
        {QStringLiteral("buildId"), QStringLiteral("old-success")},
        {QStringLiteral("openedAt"), QStringLiteral("2026-09-07T10:00:00.000Z")},
    });
    ProjectIntakeConsumer consumer(&store);
    EXPECT_FALSE(consumer.consumeAll());
    EXPECT_EQ(store.listProjects().front().lastKnownGoodStudioBuildId, seed.lastKnownGoodStudioBuildId);
    EXPECT_EQ(store.listProjects().front().lastOpenedAt, seed.lastOpenedAt);
    EXPECT_EQ(store.listProjects().front().preferredStudioBuildId, seed.preferredStudioBuildId);
    EXPECT_TRUE(intakeFiles().isEmpty());
}

TEST_F(ProjectIntakeConsumerTest, AmbiguousRuntimeBuildRequiresAnInstallationChoice)
{
    const QString path = writeProjectFile(QStringLiteral("SharedBuild"), false);
    ASSERT_FALSE(path.isEmpty());
    writeIntakeRecord(QStringLiteral("shared.json"), {
        {QStringLiteral("projectPath"), path},
        {QStringLiteral("buildId"), QStringLiteral("shared-build")},
        {QStringLiteral("openedAt"), QStringLiteral("2026-09-08T10:00:00.000Z")},
    });
    ProjectIntakeConsumer consumer(&store);
    ASSERT_TRUE(consumer.consumeAll());
    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 1u);
    const auto probe = VNH::ProjectProbe::probe(path);
    ASSERT_TRUE(probe.ok);
    ASSERT_TRUE(probe.savedStudioVersion.isEmpty());
    VNH::InstalledVersion managed;
    managed.buildId = "shared-build";
    managed.executablePath = dir.filePath("managed/HarkenStudio.exe");
    auto local = managed;
    local.buildId = "local-user-path-id";
    local.runtimeBuildId = managed.buildId;
    local.source = VNH::VersionSource::LocalBuild;
    local.executablePath = dir.filePath("local/HarkenStudio.exe");

    for (const auto& versions : {std::vector{managed, local}, std::vector{local, managed}}) {
        const auto selection = VNH::ProjectVersionSelection::resolve(records.front(), probe, versions);
        EXPECT_EQ(selection.errorCode, QStringLiteral("VersionSelectionRequired"));
        EXPECT_TRUE(selection.buildId.isEmpty());
        const auto explicitChoice = VNH::ProjectVersionSelection::resolve(
            records.front(), probe, versions, local.buildId);
        EXPECT_TRUE(explicitChoice.errorCode.isEmpty());
        EXPECT_EQ(explicitChoice.buildId, local.buildId);
    }

    managed.available = false;
    const auto unique = VNH::ProjectVersionSelection::resolve(records.front(), probe, {managed, local});
    EXPECT_TRUE(unique.errorCode.isEmpty());
    EXPECT_EQ(unique.buildId, local.buildId);
    EXPECT_EQ(store.listProjects().front().lastKnownGoodStudioBuildId, managed.buildId);
    EXPECT_TRUE(store.listProjects().front().preferredStudioBuildId.isEmpty());
}

TEST_F(ProjectIntakeConsumerTest, RecordedLocalInstallationRemainsSpecificAfterReload)
{
    const QString path = writeProjectFile(QStringLiteral("SpecificBuild"), false);
    ASSERT_FALSE(path.isEmpty());
    ProjectRecord record;
    record.canonicalPath = path;
    ASSERT_TRUE(store.addProject(record));
    VNH::InstalledVersion managed;
    managed.buildId = "shared-build";
    auto local = managed;
    local.buildId = "local-user-path-id";
    local.runtimeBuildId = managed.buildId;
    local.source = VNH::VersionSource::LocalBuild;
    ASSERT_TRUE(store.setLastKnownGood(path, local.buildId, "2026-09-08T10:00:00.000Z"));
    const auto records = store.listProjects();
    ASSERT_EQ(records.size(), 1u);
    const auto selection = VNH::ProjectVersionSelection::resolve(
        records.front(), VNH::ProjectProbe::probe(path), {managed, local});
    EXPECT_TRUE(selection.errorCode.isEmpty());
    EXPECT_EQ(selection.buildId, local.buildId);
}
