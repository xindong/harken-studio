#include "Launch/ProjectCreateFlow.h"
#include "Platform/ProcessScan.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProgressDialog>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <memory>

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

class ProjectCreateFlowTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        parent = std::make_unique<QWidget>();
        parent->show();
        request.name = "Demo";
        request.location = dir.path();
        request.version.executablePath = QString::fromUtf8(HARKEN_HUB_STUDIO_FIXTURE);
        request.version.installPath = QFileInfo(request.version.executablePath).absolutePath();
        request.version.buildId = "run1.aaaa1111";
        request.version.capabilitiesVersion = 2;
        projectPath = dir.filePath("Demo/Demo.hsp");
        QObject::connect(&dialogs, &QTimer::timeout, [this]() {
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (auto* box = qobject_cast<QMessageBox*>(widget)) {
                    messages << box->text();
                    box->accept();
                }
            }
        });
        dialogs.start(10);
    }

    void TearDown() override
    {
        parent.reset();
        const auto process = pid();
        if (process > 0 && !VNH::executablePathForPid(process).isEmpty()) {
            VNH::terminateProcessById(process);
            spinUntil([process]() { return VNH::executablePathForPid(process).isEmpty(); });
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    qint64 pid() const
    {
        QFile file(projectPath + ".launch.json");
        if (!file.open(QIODevice::ReadOnly))
            return 0;
        return QJsonDocument::fromJson(file.readAll()).object().value("pid").toInteger();
    }

    void create(const QJsonObject& config = {})
    {
        QFile file(dir.filePath("Demo.scenario.json"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(config).toJson());
        file.close();
        const auto started = VNH::ProjectCreateFlow::create(request, parent.get(),
            [this](const VNH::ProjectCreateOutcome& result) { outcome = result; ++completions; });
        ASSERT_EQ(started.result, VNH::ProjectCreateOutcome::Result::Started);
        EXPECT_TRUE(parent->isEnabled());
    }

    QTemporaryDir dir;
    std::unique_ptr<QWidget> parent;
    VNH::ProjectCreateRequest request;
    VNH::ProjectCreateOutcome outcome;
    QString projectPath;
    QStringList messages;
    QTimer dialogs;
    int completions = 0;
};

TEST_F(ProjectCreateFlowTest, ReportsCreatedOnceAfterReadyAndExit)
{
    create({{"exitAfterResult", true}});
    EXPECT_EQ(VNH::ProjectCreateFlow::create(request, parent.get()).result,
              VNH::ProjectCreateOutcome::Result::Busy);
    ASSERT_TRUE(spinUntil([this]() { return completions > 0; }));
    EXPECT_EQ(outcome.result, VNH::ProjectCreateOutcome::Result::Created);
    EXPECT_EQ(outcome.projectPath, QFileInfo(projectPath).canonicalFilePath());
    ASSERT_TRUE(spinUntil([this]() { return VNH::executablePathForPid(pid()).isEmpty(); }));
    EXPECT_EQ(completions, 1);
    EXPECT_TRUE(messages.isEmpty());
}

TEST_F(ProjectCreateFlowTest, MissingProjectFileIsNotReportedAsCreated)
{
    create({{"missingProjectFile", true}});
    ASSERT_TRUE(spinUntil([this]() { return completions > 0; }));
    EXPECT_EQ(outcome.result, VNH::ProjectCreateOutcome::Result::Failed);
    EXPECT_TRUE(outcome.projectPath.isEmpty());
}

TEST_F(ProjectCreateFlowTest, ReportedFailureDoesNotTerminateStudio)
{
    create({{"launch", "failed"}});
    ASSERT_TRUE(spinUntil([this]() { return completions > 0; }));
    EXPECT_EQ(outcome.result, VNH::ProjectCreateOutcome::Result::Failed);
    EXPECT_FALSE(VNH::executablePathForPid(pid()).isEmpty());
}

TEST_F(ProjectCreateFlowTest, DisconnectLeavesCreationResultUnknownAndStudioRunning)
{
    create({{"launch", "disconnect"}});
    ASSERT_TRUE(spinUntil([this]() { return !messages.isEmpty(); }));
    ASSERT_EQ(completions, 0);
    EXPECT_FALSE(VNH::executablePathForPid(pid()).isEmpty());
    EXPECT_TRUE(messages.join('\n').contains("unknown"));
    EXPECT_TRUE(parent->isEnabled());
    ASSERT_EQ(VNH::ProjectCreateFlow::create(request, parent.get()).result,
              VNH::ProjectCreateOutcome::Result::Busy);

    ASSERT_TRUE(VNH::terminateProcessById(pid()));
    ASSERT_TRUE(spinUntil([this]() { return completions > 0; }));
    EXPECT_EQ(outcome.result, VNH::ProjectCreateOutcome::Result::Unknown);
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(messages.size(), 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    create({{"exitAfterResult", true}});
    ASSERT_TRUE(spinUntil([this]() { return completions > 1; }));
    EXPECT_EQ(outcome.result, VNH::ProjectCreateOutcome::Result::Created);
}

TEST_F(ProjectCreateFlowTest, HandshakeRejectionPreservesSpecificReason)
{
    request.version.buildId = "run2.bbbb2222";
    create({{"readyDelayMs", 3000}});
    ASSERT_TRUE(spinUntil([this]() { return !messages.isEmpty(); }));
    ASSERT_EQ(completions, 0);
    EXPECT_FALSE(VNH::executablePathForPid(pid()).isEmpty());
    EXPECT_TRUE(messages.join('\n').contains(
        "HarkenStudio reported build id run1.aaaa1111, expected run2.bbbb2222"));
    EXPECT_TRUE(messages.join('\n').contains("The project creation result is unknown."));
    ASSERT_EQ(VNH::ProjectCreateFlow::create(request, parent.get()).result,
              VNH::ProjectCreateOutcome::Result::Busy);

    ASSERT_TRUE(VNH::terminateProcessById(pid()));
    ASSERT_TRUE(spinUntil([this]() { return completions > 0; }));
    EXPECT_EQ(outcome.result, VNH::ProjectCreateOutcome::Result::Unknown);
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(messages.size(), 1);
}

TEST_F(ProjectCreateFlowTest, HandshakeTimeoutDoesNotTerminateStudio)
{
    create({{"helloDelayMs", 20000}, {"readyDelayMs", 20000}});
    ASSERT_TRUE(spinUntil([this]() { return !messages.isEmpty(); }, 18000));
    ASSERT_EQ(completions, 0);
    EXPECT_FALSE(VNH::executablePathForPid(pid()).isEmpty());
    ASSERT_EQ(VNH::ProjectCreateFlow::create(request, parent.get()).result,
              VNH::ProjectCreateOutcome::Result::Busy);

    ASSERT_TRUE(VNH::terminateProcessById(pid()));
    ASSERT_TRUE(spinUntil([this]() { return completions > 0; }));
    EXPECT_EQ(outcome.result, VNH::ProjectCreateOutcome::Result::Unknown);
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(messages.size(), 1);
}

TEST_F(ProjectCreateFlowTest, ClosingHubWindowLeavesStudioRunning)
{
    create({{"readyDelayMs", 3000}});
    ASSERT_TRUE(spinUntil([this]() { return pid() > 0; }));
    parent.reset();
    EXPECT_FALSE(VNH::executablePathForPid(pid()).isEmpty());
    EXPECT_EQ(completions, 0);
}

TEST_F(ProjectCreateFlowTest, WaitingPastTwoMinutesDoesNotTerminateStudio)
{
    create({{"readyDelayMs", 122000}});
    ASSERT_TRUE(spinUntil([this]() { return pid() > 0; }));
    QElapsedTimer elapsed;
    elapsed.start();
    ASSERT_TRUE(spinUntil([this, &elapsed]() {
        return completions > 0 || elapsed.elapsed() >= 120500;
    }, 125000));
    EXPECT_EQ(completions, 0);
    EXPECT_TRUE(parent->isEnabled());
    EXPECT_FALSE(VNH::executablePathForPid(pid()).isEmpty());
    ASSERT_TRUE(spinUntil([this]() { return completions > 0; }));
    EXPECT_EQ(outcome.result, VNH::ProjectCreateOutcome::Result::Created);
}

}
