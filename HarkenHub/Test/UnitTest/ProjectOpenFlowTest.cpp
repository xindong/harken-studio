#include "Launch/ProjectOpenFlow.h"
#include "Platform/ProcessScan.h"
#include "Projects/ProjectStore.h"
#include "Versions/LocalBuildRegistry.h"
#include "Versions/LocalBuildRefreshCoordinator.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QWidget>
#include <algorithm>

namespace {

bool spinUntil(const std::function<bool()>& predicate, int timeoutMs = 10000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return predicate();
}

class ProjectOpenFlowTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        qputenv("HARKEN_HUB_USER_ROOT", dir.path().toUtf8());
        qputenv("HARKEN_HUB_MACHINE_ROOT", dir.filePath("machine").toUtf8());
        ASSERT_TRUE(store.open(dir.filePath("projects.db")));
        VNH::LocalBuildRecord local;
        QString error;
        ASSERT_TRUE(VNH::LocalBuildRegistry::probe(QString::fromUtf8(HARKEN_HUB_STUDIO_FIXTURE), &local, &error)) << error.toStdString();
        ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(local, &error));
        versions = VNH::VersionScanner::scanLocalBuilds();
        ASSERT_EQ(versions.size(), 1u);
        projectPath = dir.filePath("Demo.hsp");
        writeJson(projectPath, {{"harkenDocument", QJsonObject{
            {"type", "project.root"}, {"id", "11111111-2222-3333-4444-555555555555"},
            {"projectId", "11111111-2222-3333-4444-555555555555"},
            {"schemaVersion", 2}, {"savedStudioVersion", "1.4.0"}}}});
        QString canonical;
        ASSERT_TRUE(VNH::ProjectOpenFlow::ensureRegistered(&store, projectPath, &canonical, &error));
        projectPath = canonical;
        writeJson(projectPath + ".scenario.json", {});
    }

    void TearDown() override
    {
        const auto pid = launchLog().value("pid").toInteger();
        if (pid > 0) {
            VNH::terminateProcessById(pid);
            spinUntil([pid]() { return VNH::executablePathForPid(pid).isEmpty(); });
        }
        QThreadPool::globalInstance()->waitForDone(5000);
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
    }

    void writeJson(const QString& path, const QJsonObject& json)
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(json).toJson());
    }

    QJsonObject launchLog() const
    {
        QFile file(projectPath + ".launch.json");
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    QTemporaryDir dir;
    VNH::ProjectStore store;
    std::vector<VNH::InstalledVersion> versions;
    QString projectPath;
    QWidget parent;
};

TEST_F(ProjectOpenFlowTest, DispatchDoesNotClaimProjectSuccessOrOwnTheStudioLifetime)
{
    const auto before = store.listProjects().front();
    const auto result = VNH::ProjectOpenFlow::open(&store, projectPath, versions, &parent);
    ASSERT_EQ(result.result, VNH::ProjectOpenOutcome::Result::Started);
    ASSERT_TRUE(spinUntil([this]() { return !launchLog().isEmpty(); }));
    parent.close();
    QCoreApplication::processEvents();
    const auto pid = launchLog().value("pid").toInteger();
    EXPECT_FALSE(VNH::executablePathForPid(pid).isEmpty());
    const auto after = store.listProjects().front();
    EXPECT_EQ(after.lastKnownGoodStudioBuildId, before.lastKnownGoodStudioBuildId);
    EXPECT_EQ(after.lastOpenedAt, before.lastOpenedAt);
    EXPECT_EQ(after.detectedSchema, before.detectedSchema);
    EXPECT_EQ(after.preferredStudioBuildId, before.preferredStudioBuildId);
}

TEST_F(ProjectOpenFlowTest, UnavailableExecutableReturnsAnErrorWithoutAStartupSession)
{
    versions.front().available = false;
    versions.front().availabilityError = QStringLiteral("Unavailable test installation");
    const auto result = VNH::ProjectOpenFlow::open(
        &store, projectPath, versions, &parent, versions.front().buildId);
    EXPECT_EQ(result.result, VNH::ProjectOpenOutcome::Result::Blocked);
    EXPECT_EQ(result.error, versions.front().availabilityError);
    EXPECT_TRUE(launchLog().isEmpty());
    EXPECT_TRUE(store.listProjects().front().lastOpenedAt.isEmpty());
}

TEST_F(ProjectOpenFlowTest, HashMismatchSchedulesAnActualBackgroundSelfTest)
{
    auto records = VNH::LocalBuildRegistry::load();
    ASSERT_EQ(records.size(), 1u);
    const auto verifiedFingerprint = records.front().executableFingerprint;
    records.front().executableFingerprint = QString(64, QLatin1Char('0'));
    QString error;
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(records.front(), &error));
    versions = VNH::VersionScanner::scanLocalBuilds();
    ASSERT_TRUE(versions.front().available);
    const auto project = store.listProjects().front();
    EXPECT_EQ(VNH::LaunchPreflight::evaluate(project, versions, {}).verdict,
              VNH::LaunchVerdict::Blocked);
    ASSERT_FALSE(VNH::LocalBuildRegistry::load().front().lastProbeError.isEmpty());

    VNH::LocalBuildRefreshCoordinator coordinator;
    coordinator.start();
    ASSERT_TRUE(spinUntil([&]() {
        const auto refreshed = VNH::LocalBuildRegistry::load();
        return refreshed.size() == 1 && refreshed.front().lastProbeError.isEmpty()
            && refreshed.front().executableFingerprint == verifiedFingerprint;
    }));
    versions = VNH::VersionScanner::scanLocalBuilds();
    EXPECT_EQ(VNH::LaunchPreflight::evaluate(project, versions, {}).verdict,
              VNH::LaunchVerdict::Ready);
}

#if defined(Q_OS_WIN)
TEST_F(ProjectOpenFlowTest, BackgroundRefreshRecoversAfterRegistryStorageIsReleased)
{
    auto records = VNH::LocalBuildRegistry::load();
    ASSERT_EQ(records.size(), 1u);
    const auto verifiedFingerprint = records.front().executableFingerprint;
    records.front().executableFingerprint.clear();
    QString error;
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(records.front(), &error));

    QFile reader(VNH::LocalBuildRegistry::filePath());
    ASSERT_TRUE(reader.open(QIODevice::ReadOnly));
    const auto original = reader.readAll();
    VNH::LocalBuildRefreshCoordinator coordinator;
    bool refreshStarted = false;
    QObject::connect(&coordinator, &VNH::LocalBuildRefreshCoordinator::buildsChanged,
                     &coordinator, [&]() { refreshStarted = true; });
    coordinator.start();
    ASSERT_TRUE(spinUntil([&]() { return refreshStarted; }));
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
    ASSERT_TRUE(reader.seek(0));
    EXPECT_EQ(reader.readAll(), original);
    reader.close();

    ASSERT_TRUE(spinUntil([&]() {
        const auto refreshed = VNH::LocalBuildRegistry::load();
        return refreshed.size() == 1 && refreshed.front().lastProbeError.isEmpty()
            && refreshed.front().executableFingerprint == verifiedFingerprint;
    }));
}
#endif

TEST_F(ProjectOpenFlowTest, BackgroundRecheckExposesTheSelfTestFailure)
{
    VNH::LocalBuildRecord old;
    old.executablePath = dir.filePath(QStringLiteral("BrokenStudio.exe"));
    QFile binary(old.executablePath);
    ASSERT_TRUE(binary.open(QIODevice::WriteOnly));
    binary.write("not an executable");
    binary.close();
    old.capabilitiesVersion = 1;
    QString error;
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(old, &error));
    VNH::LocalBuildRefreshCoordinator refresh;
    refresh.start();
    ASSERT_TRUE(spinUntil([&]() {
        const auto records = VNH::LocalBuildRegistry::load();
        return std::any_of(records.begin(), records.end(), [&](const auto& value) {
            return value.executablePath == old.executablePath && !value.lastProbeError.isEmpty();
        });
    }));
    const auto records = VNH::VersionScanner::scanLocalBuilds();
    const auto broken = std::find_if(records.begin(), records.end(), [&](const auto& value) {
        return value.executablePath == old.executablePath;
    });
    ASSERT_NE(broken, records.end());
    EXPECT_FALSE(broken->available);
    EXPECT_FALSE(broken->availabilityError.isEmpty());
}

}
