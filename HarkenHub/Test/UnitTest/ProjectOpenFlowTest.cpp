#include "Launch/ProjectOpenFlow.h"
#include "Launch/ProjectOpenAnalyzer.h"
#include "Platform/ProcessScan.h"
#include "Projects/ProjectStore.h"
#include "Versions/LocalBuildRegistry.h"
#include "Versions/LocalBuildRefreshCoordinator.h"

#include <gtest/gtest.h>
#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProgressDialog>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTranslator>
#include <QScopeGuard>
#include <QTimer>
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
        parent.show();
        QObject::connect(&dialogs, &QTimer::timeout, &parent, [this]() {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(widget);
                if (!box || !box->isVisible())
                    continue;
                const auto buttons = box->buttons();
                const bool upgrade = std::any_of(buttons.begin(), buttons.end(), [](const auto* button) {
                    return button->text() == QStringLiteral("Upgrade and Open");
                });
                if (upgrade) {
                    ++confirmations;
                    if (beforeConfirmation)
                        beforeConfirmation();
                    for (auto* button : box->buttons())
                        if (box->buttonRole(button) == (approve ? QMessageBox::AcceptRole : QMessageBox::RejectRole)) {
                            button->click();
                            break;
                        }
                } else {
                    ++notifications;
                    notificationText = box->text();
                    box->accept();
                }
                return;
            }
        });
        dialogs.start(10);
    }

    void TearDown() override
    {
        const auto pid = launchLog().value("pid").toInteger();
        if (pid > 0) {
            VNH::terminateProcessById(pid);
            spinUntil([pid]() { return VNH::executablePathForPid(pid).isEmpty(); });
        }
        spinUntil([this]() { return !started || completed; }, 3000);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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

    void open(const QJsonObject& config = {})
    {
        writeJson(projectPath + ".scenario.json", config);
        const auto result = VNH::ProjectOpenFlow::open(&store, projectPath, versions, &parent, {},
            [this](const VNH::ProjectOpenOutcome& value) { ++completionCount; completed = true; outcome = value.result; });
        ASSERT_EQ(result.result, VNH::ProjectOpenOutcome::Result::Started);
        started = true;
    }

    bool hasApproval() const
    {
        return launchLog().value("arguments").toArray().contains(QStringLiteral("--project-open-approval"));
    }

    QTemporaryDir dir;
    VNH::ProjectStore store;
    std::vector<VNH::InstalledVersion> versions;
    QString projectPath;
    QWidget parent;
    QTimer dialogs;
    bool started = false;
    bool completed = false;
    bool approve = false;
    int confirmations = 0;
    int notifications = 0;
    QString notificationText;
    int completionCount = 0;
    std::function<void()> beforeConfirmation;
    VNH::ProjectOpenOutcome::Result outcome = VNH::ProjectOpenOutcome::Result::Unknown;
};

TEST_F(ProjectOpenFlowTest, CurrentProjectLaunchesWithoutUpgradeApproval)
{
    open();
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Launched);
    EXPECT_EQ(confirmations, 0);
    EXPECT_FALSE(hasApproval());
    EXPECT_EQ(store.listProjects().front().lastKnownGoodStudioBuildId, versions.front().buildId);
    EXPECT_TRUE(store.listProjects().front().preferredStudioBuildId.isEmpty());
}

TEST_F(ProjectOpenFlowTest, ForwardingCompletesWithoutChangingSuccessfulOpenHistory)
{
    const auto registered = store.listProjects().front();
    ASSERT_TRUE(store.recordSuccessfulOpen(projectPath, registered.projectId, registered.displayName,
        registered.detectedSchema, "previous-build", "2026-09-07T00:00:00Z"));
    ASSERT_TRUE(store.setPreferredBuild(projectPath, versions.front().buildId));
    const auto before = store.listProjects().front();

    open({{"launch", "forwarded"}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    const auto pid = launchLog().value("pid").toInteger();
    ASSERT_GT(pid, 0);
    ASSERT_TRUE(spinUntil([pid]() { return VNH::executablePathForPid(pid).isEmpty(); }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Activated);
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(notifications, 0);
    const auto after = store.listProjects().front();
    EXPECT_EQ(after.lastKnownGoodStudioBuildId, before.lastKnownGoodStudioBuildId);
    EXPECT_EQ(after.lastOpenedAt, before.lastOpenedAt);
    EXPECT_EQ(after.detectedSchema, before.detectedSchema);
    EXPECT_EQ(after.preferredStudioBuildId, before.preferredStudioBuildId);
}

TEST_F(ProjectOpenFlowTest, HistoricalStudioOpensWithoutCallingTheNewAnalyzer)
{
    versions.front().capabilitiesVersion = 1;
    open({{"legacy", true}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Launched);
    EXPECT_FALSE(QFileInfo::exists(projectPath + ".unexpected-analysis"));
    EXPECT_FALSE(hasApproval());
    EXPECT_EQ(notifications, 0);
    EXPECT_EQ(store.listProjects().front().lastKnownGoodStudioBuildId, versions.front().buildId);
}

TEST_F(ProjectOpenFlowTest, LegacyWithoutMonitoringStartsAndKeepsOpenResultUnknown)
{
    versions.front().capabilitiesVersion = 0;
    open({{"legacy", true}});
    ASSERT_TRUE(spinUntil([this]() { return !launchLog().isEmpty(); }));
    EXPECT_FALSE(QFileInfo::exists(projectPath + ".unexpected-analysis"));
    EXPECT_FALSE(launchLog().value("arguments").toArray().contains("--hub-session"));
    EXPECT_FALSE(completed);
    EXPECT_EQ(notifications, 0);
    EXPECT_TRUE(store.listProjects().front().lastOpenedAt.isEmpty());
    VNH::terminateProcessById(launchLog().value("pid").toInteger());
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Unknown);
}

TEST_F(ProjectOpenFlowTest, InvalidAnalysisFallsBackWithoutApproval)
{
    open({{"analysis", "invalid"}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Launched);
    EXPECT_FALSE(hasApproval());
    EXPECT_EQ(confirmations, 0);
}

TEST_F(ProjectOpenFlowTest, SuccessfulOpenKeepsAnalyzedSchemaWhenTheFileDisappears)
{
    const auto before = store.listProjects().front();
    ASSERT_TRUE(store.recordSuccessfulOpen(projectPath, before.projectId, before.displayName,
        1, "previous-build", "2026-09-07T00:00:00Z"));
    approve = true;
    open({{"upgrade", true}, {"readyDelayMs", 500}});
    ASSERT_TRUE(spinUntil([this]() { return !launchLog().isEmpty(); }));
    ASSERT_TRUE(QFile::remove(projectPath));
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Launched);
    const auto after = store.listProjects().front();
    EXPECT_EQ(after.detectedSchema, 2);
    EXPECT_EQ(after.projectId, before.projectId);
    EXPECT_EQ(after.displayName, before.displayName);
    EXPECT_EQ(after.lastKnownGoodStudioBuildId, versions.front().buildId);
}

TEST_F(ProjectOpenFlowTest, SuccessfulFallbackKeepsHistoryWhenFinalMetadataIsInvalid)
{
    const auto before = store.listProjects().front();
    open({{"analysis", "invalid"}, {"readyDelayMs", 500}});
    ASSERT_TRUE(spinUntil([this]() { return !launchLog().isEmpty(); }));
    writeJson(projectPath, {});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Launched);
    const auto after = store.listProjects().front();
    EXPECT_EQ(after.detectedSchema, before.detectedSchema);
    EXPECT_EQ(after.projectId, before.projectId);
    EXPECT_EQ(after.displayName, before.displayName);
}

TEST_F(ProjectOpenFlowTest, FailedAnalysisLeavesFinalDecisionToStudio)
{
    open({{"analysis", "failed"}, {"launch", "failed"}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Failed);
    EXPECT_FALSE(launchLog().isEmpty());
    EXPECT_FALSE(hasApproval());
    EXPECT_TRUE(store.listProjects().front().lastOpenedAt.isEmpty());
}

TEST_F(ProjectOpenFlowTest, NonWritableUpgradeFilesBlockBeforeApprovalAndLaunch)
{
    const QString files = projectPath + QStringLiteral("\n") + dir.filePath("Layers/Main.vp");
    open({{"analysis", "failed"}, {"upgrade", true},
          {"errorCode", "PROJECT_UPGRADE_FILES_NOT_WRITABLE"}, {"technicalDetails", files}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Failed);
    EXPECT_TRUE(launchLog().isEmpty());
    EXPECT_EQ(confirmations, 0);
    EXPECT_EQ(notifications, 1);
    EXPECT_TRUE(notificationText.contains(files));
    EXPECT_TRUE(notificationText.contains("write protection"));
    EXPECT_TRUE(store.listProjects().front().lastOpenedAt.isEmpty());
}

TEST_F(ProjectOpenFlowTest, WriteAccessChangesAfterApprovalKeepPathsInHubLanguage)
{
    approve = true;
    QTranslator translator;
    ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
    const auto removeTranslator = qScopeGuard([&]() {
        QCoreApplication::removeTranslator(&translator);
    });
    beforeConfirmation = [&]() { QCoreApplication::installTranslator(&translator); };
    const QString files = projectPath + QStringLiteral("\n") + dir.filePath("Layer %2.vp");
    const QString failure = QStringLiteral("Studio-only English failure");
    open({{"upgrade", true}, {"launch", "failed"},
          {"errorCode", "PROJECT_UPGRADE_FILES_NOT_WRITABLE"}, {"errorMessage", failure},
          {"failureDetails", files}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Failed);
    EXPECT_EQ(confirmations, 1);
    const auto reason = QCoreApplication::translate("HarkenHub", "Some files needed for the project upgrade are read-only or not writable.");
    EXPECT_NE(reason, QStringLiteral("Some files needed for the project upgrade are read-only or not writable."));
    EXPECT_TRUE(notificationText.contains(reason));
    EXPECT_TRUE(notificationText.contains(files));
    EXPECT_FALSE(notificationText.contains(failure));
    EXPECT_TRUE(store.listProjects().front().lastOpenedAt.isEmpty());
}

TEST_F(ProjectOpenFlowTest, UpgradeCancelDoesNotLaunch)
{
    open({{"upgrade", true}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Declined);
    EXPECT_EQ(confirmations, 1);
    EXPECT_TRUE(launchLog().isEmpty());
}

TEST_F(ProjectOpenFlowTest, ConfirmedUpgradeCarriesOnlyThisRequestApproval)
{
    approve = true;
    open({{"upgrade", true}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Launched);
    EXPECT_EQ(confirmations, 1);
    EXPECT_TRUE(hasApproval());
    EXPECT_TRUE(launchLog().value("arguments").toArray().contains(QStringLiteral("UpgradeApprovedByHub")));
}

TEST_F(ProjectOpenFlowTest, StudioCancellationIsNotFailureOrSuccessfulHistory)
{
    open({{"analysis", "invalid"}, {"launch", "cancel"}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Declined);
    EXPECT_EQ(notifications, 0);
    EXPECT_TRUE(store.listProjects().front().lastOpenedAt.isEmpty());
}

TEST_F(ProjectOpenFlowTest, DisconnectLeavesProcessRunningAndResultUnknown)
{
    open({{"launch", "disconnect"}});
    ASSERT_TRUE(spinUntil([this]() { return notifications > 0; }));
    EXPECT_FALSE(completed);
    EXPECT_FALSE(VNH::executablePathForPid(launchLog().value("pid").toInteger()).isEmpty());
    EXPECT_TRUE(parent.isEnabled());
    EXPECT_TRUE(store.listProjects().front().lastOpenedAt.isEmpty());
    VNH::terminateProcessById(launchLog().value("pid").toInteger());
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Unknown);
}

TEST_F(ProjectOpenFlowTest, FinalResultAndProcessExitKeepOutcomeConsistentWithHistory)
{
    open({{"readyDelayMs", 950}, {"exitAfterResult", true}});
    ASSERT_TRUE(spinUntil([this]() { return !launchLog().isEmpty(); }));
    QElapsedTimer handshake;
    handshake.start();
    spinUntil([&]() { return handshake.elapsed() >= 100; });
    QThread::msleep(1100);
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    QCoreApplication::processEvents();
    EXPECT_EQ(completionCount, 1);
    ASSERT_TRUE(outcome == VNH::ProjectOpenOutcome::Result::Launched
                || outcome == VNH::ProjectOpenOutcome::Result::Failed);
    EXPECT_EQ(!store.listProjects().front().lastOpenedAt.isEmpty(),
              outcome == VNH::ProjectOpenOutcome::Result::Launched);
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

TEST_F(ProjectOpenFlowTest, AnalyzerTimeoutNeverProducesApproval)
{
    writeJson(projectPath + ".scenario.json", {{"analysisDelayMs", 3000}, {"upgrade", true}});
    const auto report = VNH::ProjectOpenAnalyzer::run(versions.front(), projectPath, "request", 100);
    EXPECT_FALSE(report.reportValid);
    EXPECT_EQ(report.errorCode, QStringLiteral("ANALYZER_TIMEOUT"));
    EXPECT_TRUE(report.approvalDigest.isEmpty());
}

TEST_F(ProjectOpenFlowTest, RebuiltExecutableCannotUseThePreviousApproval)
{
    const auto program = dir.filePath(QFileInfo(QString::fromUtf8(HARKEN_HUB_STUDIO_FIXTURE)).fileName());
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(HARKEN_HUB_STUDIO_FIXTURE), program));
#ifdef Q_OS_WIN
    const auto libraries = QDir(QCoreApplication::applicationDirPath()).entryList(
        {QStringLiteral("Qt6Core*.dll"), QStringLiteral("Qt6Network*.dll")}, QDir::Files);
    for (const auto& name : libraries)
        ASSERT_TRUE(QFile::copy(QDir(QCoreApplication::applicationDirPath()).filePath(name), dir.filePath(name)));
#endif
    VNH::LocalBuildRecord local;
    QString error;
    ASSERT_TRUE(VNH::LocalBuildRegistry::probe(program, &local, &error)) << error.toStdString();
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(local, &error));
    versions = VNH::VersionScanner::scanLocalBuilds();
    versions.erase(std::remove_if(versions.begin(), versions.end(), [&](const auto& candidate) {
        return candidate.executablePath != program;
    }), versions.end());
    ASSERT_EQ(versions.size(), 1u);
    approve = true;
    beforeConfirmation = [program]() {
        QFile file(program);
        ASSERT_TRUE(file.open(QIODevice::Append));
        file.write("changed after analysis");
    };
    open({{"upgrade", true}});
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Failed);
    EXPECT_EQ(confirmations, 1);
    EXPECT_TRUE(launchLog().isEmpty());
    EXPECT_TRUE(store.listProjects().front().lastOpenedAt.isEmpty());
}

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

TEST_F(ProjectOpenFlowTest, WaitingPastTwoMinutesDoesNotTerminateStudio)
{
    open({{"readyDelayMs", 122000}});
    ASSERT_TRUE(spinUntil([this]() { return !launchLog().isEmpty(); }));
    QElapsedTimer elapsed;
    elapsed.start();
    ASSERT_TRUE(spinUntil([this, &elapsed]() {
        return completed || elapsed.elapsed() >= 120500;
    }, 125000));
    EXPECT_FALSE(completed);
    EXPECT_TRUE(parent.isEnabled());
    EXPECT_FALSE(VNH::executablePathForPid(launchLog().value("pid").toInteger()).isEmpty());
    EXPECT_TRUE(store.listProjects().front().lastOpenedAt.isEmpty());
    ASSERT_TRUE(spinUntil([this]() { return completed; }));
    EXPECT_EQ(outcome, VNH::ProjectOpenOutcome::Result::Launched);
}

}
