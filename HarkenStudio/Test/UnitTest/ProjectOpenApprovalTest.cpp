#include "Core/ProjectOpenApproval.h"
#include "Core/MigrationManager.h"
#include "Core/ProjectOpenProtocol.h"
#include "Core/ProjectTransaction.h"
#include "Core/IPC/IPCServer.h"
#include "Core/StartupCoordinator.h"
#include "Core/AudioEngine.h"
#include "Core/RuntimeSyncCoordinator.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "Dialogs/StartupLoadingWindow.h"
#include "MainWindow.h"
#include "Support/AITestSupport.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QAction>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QJsonArray>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

namespace {

void writeObject(const QString& path, const QJsonObject& object)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes = QJsonDocument(object).toJson();
    ASSERT_EQ(file.write(bytes), bytes.size());
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

QJsonObject readObject(const QString& path)
{
    return QJsonDocument::fromJson(readBytes(path)).object();
}

QString writeProject(const QTemporaryDir& directory,
                     int version = VNX::ProjectSchema::kProjectSchemaVersion)
{
    const QString projectPath = directory.filePath(QStringLiteral("Legacy/Legacy.hsp"));
    writeObject(projectPath, HarkenTest::projectRoot({
        {QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Legacy")},
        }},
        {QStringLiteral("paths"), QJsonObject{
            {QStringLiteral("audioFolder"), QStringLiteral("Layers")},
        }},
        {QStringLiteral("extensions"), QJsonObject{
            {QStringLiteral("authorNotes"), QJsonArray{17, true, QStringLiteral("Keep")}},
        }},
    }, version));
    const QString busId = QStringLiteral("bbbbbbbb-1111-2222-3333-444444444444");
    writeObject(directory.filePath(QStringLiteral("Legacy/Busses/Master.vp")),
        HarkenTest::projectDocument(QStringLiteral("project.bus"), busId, HarkenTest::kProjectId, {
            {QStringLiteral("type"), QStringLiteral("Bus")},
            {QStringLiteral("guid"), busId},
            {QStringLiteral("name"), QStringLiteral("Master")},
            {QStringLiteral("parentRef"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("root")},
            }},
            {QStringLiteral("componentData"), QJsonObject{}},
            {QStringLiteral("playableProperties"), QJsonObject{
                {QStringLiteral("presence"), QStringLiteral("0000000000000000")},
            }},
        }, version));
    return projectPath;
}

QMap<QString, QByteArray> treeBytes(const QString& directory)
{
    QMap<QString, QByteArray> result;
    QDirIterator files(directory, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        result.insert(QDir(directory).relativeFilePath(path), readBytes(path));
    }
    return result;
}

struct FutureUpgrade
{
    VNX::MigrationManager migrations;
    VNX::ProjectUpgradeManager manager{&migrations};

    FutureUpgrade()
    {
        VNX::ProjectMigrationStep step;
        step.id = QStringLiteral("test-current-to-next");
        step.fromSchema = VNX::ProjectSchema::kProjectSchemaVersion;
        step.toSchema = step.fromSchema + 1;
        step.writeSet = {QStringLiteral("<all-documents>")};
        step.transform = [](VNX::ProjectMigrationWorkspace& workspace, QStringList*, QString* error) {
            for (const QString& path : workspace.writeSet()) {
                QJsonObject document;
                if (!workspace.readObject(path, &document, error)) return false;
                QJsonObject header = HarkenTest::projectHeader(document);
                header.insert(QStringLiteral("schemaVersion"), VNX::ProjectSchema::kProjectSchemaVersion + 1);
                document.insert(QStringLiteral("harkenDocument"), header);
                if (!workspace.writeObject(path, document, error)) return false;
            }
            return true;
        };
        EXPECT_TRUE(migrations.registerMigration(std::move(step)));
        manager.setTargetSchemaForTesting(VNX::ProjectSchema::kProjectSchemaVersion + 1);
    }
};

struct ModalResponder
{
    QTimer timer;
    bool acceptUpgrade;
    int confirmations = 0;
    int failures = 0;
    int filePickers = 0;
    QString impact;
    QString details;
    QString failureTitle;
    QString failureText;

    explicit ModalResponder(bool accept) : acceptUpgrade(accept)
    {
        QObject::connect(&timer, &QTimer::timeout, [&]() {
            QWidget* modal = QApplication::activeModalWidget();
            if (auto* box = qobject_cast<QMessageBox*>(modal)) {
                if (box->objectName() == QStringLiteral("projectUpgradeConfirmation")) {
                    ++confirmations;
                    impact = box->informativeText();
                    details = box->detailedText();
                    EXPECT_EQ(box->defaultButton(), box->button(QMessageBox::Cancel));
                    box->button(acceptUpgrade ? QMessageBox::Ok : QMessageBox::Cancel)->click();
                } else {
                    ++failures;
                    failureTitle = box->windowTitle();
                    failureText = box->text();
                    box->done(QMessageBox::Ok);
                }
            } else if (auto* picker = qobject_cast<QFileDialog*>(modal)) {
                ++filePickers;
                picker->reject();
            }
        });
        timer.start(5);
    }
};

VNX::ProjectOpenApprovalBinding bindingFor(const VNX::ProjectUpgradeAnalysis& analysis)
{
    const QString requestId = QStringLiteral("bound-open-request");
    return {requestId, VNX::projectOpenApprovalDigest(analysis, requestId), VNX::currentStudioBuildId()};
}

bool runInChild()
{
    const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
    const QString name = QString::fromLatin1(test->test_suite_name())
        + QLatin1Char('.') + QString::fromLatin1(test->name());
    if (qEnvironmentVariable("HARKEN_PROJECT_OPEN_TEST_CHILD") == name)
        return false;
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("HARKEN_PROJECT_OPEN_TEST_CHILD"), name);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(),
        {QStringLiteral("--gtest_filter=") + name, QStringLiteral("--gtest_color=no")});
    EXPECT_TRUE(child.waitForStarted(5000));
    if (!child.waitForFinished(20000)) {
        child.kill();
        child.waitForFinished();
        ADD_FAILURE() << child.readAll().toStdString();
    }
    const QByteArray output = child.readAll();
    EXPECT_EQ(child.exitStatus(), QProcess::NormalExit) << output.toStdString();
    EXPECT_EQ(child.exitCode(), 0) << output.toStdString();
    return true;
}

bool startProject(const QString& path, VNX::HubClient::Arguments arguments, ModalResponder& response)
{
    VNX::StartupLoadingWindow loading;
    VNX::StartupCoordinator coordinator(*qobject_cast<QApplication*>(qApp), loading,
                                        std::move(arguments), path);
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (loading.progressValue() == 100 || response.failures
            || (!response.acceptUpgrade && response.confirmations))
            loop.quit();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    coordinator.start();
    poll.start(5);
    timeout.start(10000);
    loop.exec();
    return loading.progressValue() == 100 && VNX::ProjectManager::instance().currentProject();
}

void expectCurrent(const QString& path)
{
    EXPECT_EQ(HarkenTest::projectHeader(readObject(path)).value(QStringLiteral("schemaVersion")).toInt(),
              VNX::ProjectSchema::kProjectSchemaVersion);
    EXPECT_EQ(HarkenTest::projectContent(readObject(path)).value(QStringLiteral("extensions")).toObject()
                  .value(QStringLiteral("authorNotes")), QJsonArray({17, true, QStringLiteral("Keep")}));
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(path));
}

TEST(ProjectOpenApproval, LocalConfirmationBindsPathsBuildAndMigrationPlan)
{
    VNX::TestSupport::ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = writeProject(directory);
    FutureUpgrade future;
    const auto analysis = future.manager.analyze(path);
    ASSERT_TRUE(analysis.ok);
    ModalResponder response(true);
    const auto decision = VNX::confirmProjectOpen(analysis, nullptr);
    ASSERT_TRUE(decision.proceed);
    ASSERT_TRUE(decision.upgradeApproval);
    EXPECT_TRUE(decision.upgradeApproval(analysis));
    EXPECT_EQ(response.confirmations, 1);
    EXPECT_TRUE(response.impact.contains(QDir::toNativeSeparators(directory.path())));
    EXPECT_TRUE(response.details.contains(QStringLiteral("Legacy.hsp")));
    EXPECT_TRUE(response.details.contains(QStringLiteral("Busses/Master.vp")));
    const QList<std::function<void(VNX::ProjectUpgradeAnalysis&)>> changes{
        [](auto& value) { value.ok = false; },
        [](auto& value) { value.upgradeRequired = false; },
        [](auto& value) { value.sourceFingerprint.append('x'); },
        [](auto& value) { value.sourceTreeFingerprint.append('x'); },
        [](auto& value) { value.projectFilePath += QStringLiteral(".renamed"); },
        [](auto& value) { value.probe.projectId += QStringLiteral("-changed"); },
        [](auto& value) { ++value.targetSchema; },
        [](auto& value) { value.migrationPlan.steps.front().id += QStringLiteral("-changed"); },
        [](auto& value) { value.migrationPlan.writeSet.append(QStringLiteral("Layers/Other.vp")); },
        [](auto& value) { ++value.requiredProjectBytes; },
        [](auto& value) { value.recoveryRoot += QStringLiteral("/other"); },
    };
    for (const auto& change : changes) {
        auto changed = analysis;
        change(changed);
        EXPECT_FALSE(decision.upgradeApproval(changed));
    }
    auto wrongBuild = bindingFor(analysis);
    wrongBuild.expectedBuildId += QStringLiteral("-other-build");
    EXPECT_FALSE(VNX::confirmProjectOpen(analysis, nullptr, wrongBuild).proceed);
    EXPECT_EQ(response.confirmations, 1);
}

TEST(ProjectOpenApproval, CancelAndInvalidIncomingApprovalsLeaveOriginalFiles)
{
    VNX::TestSupport::ensureApp();
    QTemporaryDir directory;
    const QString path = writeProject(directory);
    const auto before = treeBytes(directory.path());
    FutureUpgrade future;
    const auto analysis = future.manager.analyze(path);
    ASSERT_TRUE(analysis.ok);
    ModalResponder response(false);
    const auto cancelled = VNX::confirmProjectOpen(analysis, nullptr);
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_FALSE(cancelled.proceed);
    EXPECT_FALSE(cancelled.upgradeApproval);
    EXPECT_EQ(response.confirmations, 1);
    const auto binding = bindingFor(analysis);
    const auto approved = VNX::confirmProjectOpen(analysis, nullptr, binding);
    EXPECT_TRUE(approved.proceed);
    EXPECT_EQ(response.confirmations, 1);
    for (const auto& invalid : {
             VNX::ProjectOpenApprovalBinding{},
             VNX::ProjectOpenApprovalBinding{QStringLiteral("other-request"), binding.digest, {}},
             VNX::ProjectOpenApprovalBinding{binding.requestId, QString(64, QLatin1Char('f')), {}},
         }) {
        const auto rejected = VNX::confirmProjectOpen(analysis, nullptr, invalid);
        EXPECT_FALSE(rejected.proceed);
        EXPECT_FALSE(rejected.upgradeApproval);
        EXPECT_EQ(rejected.issue, VNX::ProjectIssue::ProjectChangedDuringUpgrade);
    }
    EXPECT_EQ(response.confirmations, 1);
    EXPECT_EQ(treeBytes(directory.path()), before);
}

TEST(ProjectOpenApproval, ChangedFilesAndPendingJournalDoNotConsumeApproval)
{
    VNX::TestSupport::ensureApp();
    QTemporaryDir directory;
    const QString path = writeProject(directory);
    FutureUpgrade future;
    const auto analysis = future.manager.analyze(path);
    ASSERT_TRUE(analysis.ok);
    const auto approved = VNX::confirmProjectOpen(analysis, nullptr, bindingFor(analysis));
    ASSERT_TRUE(approved.upgradeApproval);
    QJsonObject changed = readObject(path);
    QJsonObject content = HarkenTest::projectContent(changed);
    content.insert(QStringLiteral("extensions"), QJsonObject{{QStringLiteral("authorEdit"), true}});
    changed.insert(QStringLiteral("content"), content);
    writeObject(path, changed);
    auto before = treeBytes(directory.path());
    EXPECT_FALSE(approved.upgradeApproval(future.manager.analyze(path)));
    const auto rejected = future.manager.execute(analysis);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.issue, VNX::ProjectIssue::ProjectChangedDuringUpgrade);
    EXPECT_EQ(treeBytes(directory.path()), before);
    QString error;
    auto transaction = VNX::ProjectTransaction::begin(path, {QStringLiteral("Legacy.hsp")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    transaction.reset();
    ASSERT_TRUE(VNX::ProjectTransaction::recoveryRequired(path));
    before = treeBytes(directory.path());
    const auto blocked = future.manager.analyzeForProjectOpen(path);
    EXPECT_FALSE(blocked.ok);
    EXPECT_FALSE(VNX::confirmProjectOpen(blocked, nullptr, bindingFor(analysis)).proceed);
    EXPECT_FALSE(future.manager.execute(blocked).ok);
    EXPECT_TRUE(VNX::ProjectTransaction::recoveryRequired(path));
    EXPECT_EQ(treeBytes(directory.path()), before);
    EXPECT_TRUE(VNX::ProjectTransaction::recover(path, &error)) << error.toStdString();
}

TEST(ProjectOpenApproval, LegacyPropertyModelsCannotBeApprovedOrRecovered)
{
    VNX::TestSupport::ensureApp();
    for (bool missingMarker : {false, true}) {
        SCOPED_TRACE(missingMarker);
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const QString path = writeProject(directory);
        QJsonObject document = readObject(path);
        QJsonObject header = HarkenTest::projectHeader(document);
        if (missingMarker)
            header.remove(QStringLiteral("propertyModel"));
        else
            header.insert(QStringLiteral("propertyModel"), QStringLiteral("fieldGroups"));
        document.insert(QStringLiteral("harkenDocument"), header);
        writeObject(path, document);
        const auto analysis = VNX::ProjectUpgradeManager().analyzeForProjectOpen(path);
        ASSERT_FALSE(analysis.ok);
        EXPECT_TRUE(analysis.blockingErrors.join(QLatin1Char(';')).contains(QStringLiteral("property model")));
        ModalResponder response(true);
        EXPECT_FALSE(VNX::confirmProjectOpen(analysis, nullptr).proceed);
        EXPECT_FALSE(VNX::confirmProjectOpen(analysis, nullptr, bindingFor(analysis)).proceed);
        EXPECT_EQ(response.confirmations, 0);
        bool consumed = false;
        const auto approval = [&](const VNX::ProjectUpgradeAnalysis&) {
            consumed = true;
            return true;
        };
        auto before = treeBytes(directory.path());
        EXPECT_EQ(VNX::ProjectManager::prepareProject(path, true, {}, approval).project, nullptr);
        EXPECT_FALSE(consumed);
        EXPECT_EQ(treeBytes(directory.path()), before);
        QString error;
        auto transaction = VNX::ProjectTransaction::begin(path, {QStringLiteral("Legacy.hsp")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        transaction.reset();
        ASSERT_TRUE(VNX::ProjectTransaction::recoveryRequired(path));
        before = treeBytes(directory.path());
        VNX::ProjectSerializer serializer;
        VNX::ProjectData project;
        EXPECT_FALSE(serializer.loadProject(path, project, true, {}, approval));
        EXPECT_FALSE(consumed);
        EXPECT_EQ(treeBytes(directory.path()), before);
        EXPECT_TRUE(VNX::ProjectTransaction::recoveryRequired(path));
        EXPECT_TRUE(VNX::ProjectTransaction::recover(path, &error)) << error.toStdString();
    }
}

TEST(StudioProjectOpen, HubApprovalCannotAuthorizeLegacyPropertyModel)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = writeProject(directory);
    FutureUpgrade future;
    const auto analysis = future.manager.analyzeForProjectOpen(path);
    ASSERT_TRUE(analysis.ok);
    const auto binding = bindingFor(analysis);
    QJsonObject document = readObject(path);
    QJsonObject header = HarkenTest::projectHeader(document);
    header.insert(QStringLiteral("propertyModel"), QStringLiteral("fieldGroups"));
    document.insert(QStringLiteral("harkenDocument"), header);
    writeObject(path, document);
    const auto before = treeBytes(directory.path());
    VNX::HubClient::Arguments arguments;
    arguments.present = true;
    arguments.serverName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    arguments.sessionToken = QStringLiteral("disconnected");
    arguments.launchIntent = QStringLiteral("UpgradeApprovedByHub");
    arguments.projectOpenRequestId = binding.requestId;
    arguments.projectOpenApproval = binding.digest;
    arguments.expectedBuildId = binding.expectedBuildId;
    ModalResponder response(true);
    EXPECT_FALSE(startProject(path, arguments, response));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 1);
    EXPECT_EQ(treeBytes(directory.path()), before);
}

TEST(StudioProjectOpen, StartupOpensCurrentGenerationWithoutConfirmation)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory);
    ModalResponder response(true);
    EXPECT_TRUE(startProject(path, {}, response));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 0);
    expectCurrent(path);
}

TEST(StudioProjectOpen, StartupRejectsRetiredGenerationWithoutChangingFiles)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory, 1);
    const auto before = treeBytes(directory.path());
    ModalResponder response(false);
    EXPECT_FALSE(startProject(path, {}, response));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 1);
    EXPECT_TRUE(response.failureText.contains(QStringLiteral("no longer supports")));
    EXPECT_EQ(treeBytes(directory.path()), before);
    EXPECT_EQ(VNX::ProjectManager::instance().currentProject(), nullptr);
}

TEST(StudioProjectOpen, HubApprovalCannotAuthorizeARetiredGeneration)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory);
    FutureUpgrade future;
    const auto analysis = future.manager.analyzeForProjectOpen(path);
    ASSERT_TRUE(analysis.ok);
    const auto binding = bindingFor(analysis);
    writeProject(directory, 2);
    const auto before = treeBytes(directory.path());
    VNX::HubClient::Arguments arguments;
    arguments.present = true;
    arguments.serverName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    arguments.sessionToken = QStringLiteral("disconnected");
    arguments.launchIntent = QStringLiteral("UpgradeApprovedByHub");
    arguments.projectOpenRequestId = binding.requestId;
    arguments.projectOpenApproval = binding.digest;
    arguments.expectedBuildId = binding.expectedBuildId;
    ModalResponder response(false);
    EXPECT_FALSE(startProject(path, arguments, response));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 1);
    EXPECT_TRUE(response.failureText.contains(QStringLiteral("no longer supports")));
    EXPECT_EQ(treeBytes(directory.path()), before);
}

TEST(StudioProjectOpen, DisconnectedHubOpensCurrentGeneration)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory);
    VNX::HubClient::Arguments arguments;
    arguments.present = true;
    arguments.serverName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    arguments.sessionToken = QStringLiteral("disconnected");
    arguments.launchIntent = QStringLiteral("DirectOpenOnly");
    ModalResponder response(true);
    EXPECT_TRUE(startProject(path, arguments, response));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 0);
    expectCurrent(path);
}

TEST(StudioProjectOpen, InvalidHubApprovalShowsFailureWithoutMigration)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory);
    const auto before = treeBytes(directory.path());
    VNX::HubClient::Arguments arguments;
    arguments.launchIntent = QStringLiteral("UpgradeApprovedByHub");
    arguments.projectOpenRequestId = QStringLiteral("request");
    arguments.projectOpenApproval = QString(64, QLatin1Char('f'));
    ModalResponder response(false);
    EXPECT_FALSE(startProject(path, arguments, response));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 1);
    EXPECT_EQ(treeBytes(directory.path()), before);
}

TEST(StudioProjectOpen, EditorOpensCurrentGenerationWithoutConfirmation)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory);
    VNX::MainWindow window;
    ModalResponder response(true);
    EXPECT_TRUE(window.loadProject(path));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 0);
    expectCurrent(path);
}

enum class PendingOpenOutcome { Completed, Canceled, TimedOut, Reentered, ReenteredCreate };

void checkPendingProjectOpen(PendingOpenOutcome outcome)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    auto& engine = VNX::AudioEngine::instance();
    auto& projects = VNX::ProjectManager::instance();
    engine.shutdown();
    HarkenEditor::Bridge::EngineSessionConfig engineConfig;
    engineConfig.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
    ASSERT_TRUE(engine.initialize(engineConfig));
    const auto reset = qScopeGuard([&]() {
        HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
        engine.shutdown();
        if (projects.currentProject()) projects.closeProject();
    });
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("SavedBusProject");
    config.location = directory.path();
    auto saved = VNX::ProjectManager::prepareProject(config);
    ASSERT_NE(saved.project, nullptr);
    const QString savedPath = saved.project->projectPath;
    const QByteArray savedBytes = readBytes(savedPath);
    QString competingPath;
    if (outcome == PendingOpenOutcome::Reentered) {
        config.name = QStringLiteral("CompetingProject");
        auto competing = VNX::ProjectManager::prepareProject(config);
        ASSERT_NE(competing.project, nullptr);
        competingPath = competing.project->projectPath;
    }
    VNX::MainWindow window;
    config.name = QStringLiteral("Bootstrap");
    ASSERT_TRUE(projects.createProject(config));
    ASSERT_NE(projects.currentProject(), nullptr);
    ASSERT_TRUE(window.loadProject(projects.currentProject()->projectPath));
    ASSERT_TRUE(projects.saveProject());
    ASSERT_TRUE(VNX::TestSupport::spinUntil([&]() {
        QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
        auto& coordinator = engine.runtimeSyncCoordinator();
        return coordinator.barrier(coordinator.currentAuthoringRevision()).state
            == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready;
    }, 5000));
    HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(
        +[](const harkenstudiobuild::ContainerWorldInstallViewV1&) noexcept {
            harkenstudiobuild::WorldInstallResult result;
            result.status = harkenstudiobuild::WorldInstallStatus::Backpressure;
            return result;
        });
    bool released = false;
    bool canceled = false;
    bool reentered = false;
    QJsonObject nestedResult;
    VNX::IPCServer server(nullptr);
    server.setProjectManager(&projects);
    server.setMainWindow(&window);
    QTimer release;
    QObject::connect(&release, &QTimer::timeout, [&]() {
        if (!projects.isClosing()) return;
        if (outcome == PendingOpenOutcome::Completed
            || outcome == PendingOpenOutcome::Reentered
            || outcome == PendingOpenOutcome::ReenteredCreate) {
            release.stop();
            released = true;
            HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
            if (outcome == PendingOpenOutcome::Reentered
                || outcome == PendingOpenOutcome::ReenteredCreate) {
                reentered = true;
                QTimer::singleShot(0, &engine, [&]() {
                    QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
                });
                if (outcome == PendingOpenOutcome::Reentered) {
                    nestedResult = server.dispatchCommand(QStringLiteral("open_project"), {
                        {QStringLiteral("path"), competingPath},
                    });
                } else {
                    nestedResult = server.dispatchCommand(QStringLiteral("create_project"), {
                        {QStringLiteral("name"), QStringLiteral("CompetingCreatedProject")},
                        {QStringLiteral("location"), directory.path()},
                    });
                }
            } else {
                QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
            }
        } else if (outcome == PendingOpenOutcome::Canceled) {
            for (auto* progress : window.findChildren<QProgressDialog*>()) {
                for (auto* button : progress->findChildren<QPushButton*>()) {
                    if (button->text() == QStringLiteral("Cancel")) {
                        canceled = true;
                        button->click();
                        return;
                    }
                }
            }
        }
    });
    if (outcome != PendingOpenOutcome::TimedOut)
        release.start(5);
    int opened = 0;
    QObject::connect(&projects, &VNX::ProjectManager::projectOpened, &window,
                     [&](VNX::ProjectData*) { ++opened; });
    ModalResponder response(true);
    QElapsedTimer elapsed;
    elapsed.start();
    const auto result = server.dispatchCommand(QStringLiteral("open_project"), {
        {QStringLiteral("path"), savedPath},
    });
    release.stop();
    EXPECT_EQ(readBytes(savedPath), savedBytes);
    if (outcome == PendingOpenOutcome::Canceled
        || outcome == PendingOpenOutcome::TimedOut) {
        EXPECT_LT(elapsed.elapsed(), 15000);
        EXPECT_EQ(canceled, outcome == PendingOpenOutcome::Canceled);
        EXPECT_FALSE(released);
        EXPECT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(result.value(QStringLiteral("code")).toString(), QStringLiteral("OPEN_PROJECT_FAILED"));
        EXPECT_EQ(opened, 0);
        EXPECT_EQ(projects.currentProject(), nullptr);
        EXPECT_TRUE(projects.isClosing());
        EXPECT_TRUE(projects.hasPendingDestruction());
        EXPECT_EQ(response.failures, outcome == PendingOpenOutcome::TimedOut ? 1 : 0);
        HarkenEditor::Bridge::setCurrentWorldInstallBackendForTesting(nullptr);
        ASSERT_TRUE(VNX::TestSupport::spinUntil([&]() {
            QMetaObject::invokeMethod(&engine, "onRuntimeServiceTimer", Qt::DirectConnection);
            return !projects.isClosing();
        }, 5000));
        EXPECT_FALSE(projects.hasPendingDestruction());
        EXPECT_EQ(projects.currentProject(), nullptr);
        EXPECT_EQ(opened, 0);
        const auto retry = server.dispatchCommand(QStringLiteral("open_project"), {
            {QStringLiteral("path"), savedPath},
        });
        EXPECT_EQ(retry.value(QStringLiteral("status")).toString(), QStringLiteral("ok"))
            << QJsonDocument(retry).toJson().toStdString();
        ASSERT_NE(projects.currentProject(), nullptr);
        EXPECT_EQ(projects.currentProject()->projectPath, savedPath);
        EXPECT_EQ(opened, 1);
        return;
    }
    EXPECT_TRUE(released);
    if (outcome == PendingOpenOutcome::Reentered
        || outcome == PendingOpenOutcome::ReenteredCreate) {
        EXPECT_TRUE(reentered);
        EXPECT_EQ(nestedResult.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(nestedResult.value(QStringLiteral("code")).toString(),
                  outcome == PendingOpenOutcome::Reentered
                      ? QStringLiteral("OPEN_PROJECT_FAILED") : QStringLiteral("CREATE_PROJECT_FAILED"));
        EXPECT_FALSE(QDir(directory.filePath(QStringLiteral("CompetingCreatedProject"))).exists());
    }
    EXPECT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"))
        << QJsonDocument(result).toJson().toStdString();
    EXPECT_EQ(result.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("path")).toString(), savedPath);
    ASSERT_NE(projects.currentProject(), nullptr);
    EXPECT_EQ(projects.currentProject()->projectPath, savedPath);
    EXPECT_EQ(opened, 1);
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 0);
    EXPECT_FALSE(projects.hasPendingDestruction());
    EXPECT_TRUE(window.windowTitle().contains(QFileInfo(savedPath).baseName()));
}

TEST(StudioProjectOpen, IpcWaitsForPendingCloseBeforeReportingSavedProjectOpened)
{
    checkPendingProjectOpen(PendingOpenOutcome::Completed);
}

TEST(StudioProjectOpen, IpcPendingCloseTimesOutWithoutOpeningLaterAndCanRetry)
{
    checkPendingProjectOpen(PendingOpenOutcome::TimedOut);
}

TEST(StudioProjectOpen, IpcPendingCloseCanBeCanceledWithoutOpeningLaterAndCanRetry)
{
    checkPendingProjectOpen(PendingOpenOutcome::Canceled);
}

TEST(StudioProjectOpen, IpcReentrantOpenDoesNotReplaceOrMisreportThePendingProject)
{
    checkPendingProjectOpen(PendingOpenOutcome::Reentered);
}

TEST(StudioProjectOpen, IpcReentrantCreateDoesNotReplaceThePendingProject)
{
    checkPendingProjectOpen(PendingOpenOutcome::ReenteredCreate);
}

TEST(StudioProjectOpen, RetiredGenerationKeepsTheOpenProjectAndOriginalFiles)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory, 2);
    const QByteArray original = readBytes(path);
    VNX::MainWindow window;
    VNX::ProjectConfig config;
    config.name = QStringLiteral("Existing");
    config.location = directory.path();
    ASSERT_TRUE(VNX::ProjectManager::instance().createProject(config));
    const QString existing = VNX::ProjectManager::instance().currentProject()->projectPath;
    ModalResponder response(false);
    EXPECT_FALSE(window.loadProject(path));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 1);
    EXPECT_TRUE(response.failureText.contains(QStringLiteral("no longer supports")));
    ASSERT_NE(VNX::ProjectManager::instance().currentProject(), nullptr);
    EXPECT_EQ(VNX::ProjectManager::instance().currentProject()->projectPath, existing);
    EXPECT_EQ(readBytes(path), original);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(path));
}

TEST(StudioProjectOpen, FileMenusUseHubPanels)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString hubPath = directory.filePath(QStringLiteral("InvalidHub.exe"));
    QFile hubFile(hubPath);
    ASSERT_TRUE(hubFile.open(QIODevice::WriteOnly));
    hubFile.close();
    const QByteArray previous = qgetenv("HARKEN_HUB_INSTALL_DIR");
    const auto restore = qScopeGuard([&previous]() {
        if (previous.isNull())
            qunsetenv("HARKEN_HUB_INSTALL_DIR");
        else
            qputenv("HARKEN_HUB_INSTALL_DIR", previous);
    });
    qputenv("HARKEN_HUB_INSTALL_DIR", hubPath.toUtf8());
    VNX::MainWindow window;
    ModalResponder response(false);
    for (const QString& name : {
             QStringLiteral("fileMenu_openProject"), QStringLiteral("fileMenu_newProject")}) {
        QAction* action = window.findChild<QAction*>(name);
        ASSERT_NE(action, nullptr);
        action->trigger();
        EXPECT_EQ(response.failureTitle, QStringLiteral("Harken Hub"));
        EXPECT_EQ(response.failureText, QStringLiteral("Harken Hub did not start."));
    }
    EXPECT_EQ(response.filePickers, 0);
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 2);
    EXPECT_EQ(VNX::ProjectManager::instance().currentProject(), nullptr);
}

TEST(StudioProjectOpen, CurrentGenerationOpenRetainsTransactionRecovery)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory, VNX::ProjectSchema::kProjectSchemaVersion);
    const QByteArray original = readBytes(path);
    QJsonObject changed = readObject(path);
    QJsonObject content = HarkenTest::projectContent(changed);
    content.insert(QStringLiteral("extensions"), QJsonObject{{QStringLiteral("interruptedEdit"), true}});
    changed.insert(QStringLiteral("content"), content);
    const QString staging = directory.filePath(QStringLiteral("staging"));
    writeObject(QDir(staging).filePath(QStringLiteral("Legacy.hsp")), changed);
    QString error;
    auto transaction = VNX::ProjectTransaction::begin(path, {QStringLiteral("Legacy.hsp")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->replaceFromDirectory(staging, &error)) << error.toStdString();
    transaction.reset();
    ASSERT_TRUE(VNX::ProjectTransaction::recoveryRequired(path));
    VNX::MainWindow window;
    ModalResponder response(false);
    EXPECT_TRUE(window.loadProject(path));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 0);
    EXPECT_EQ(readBytes(path), original);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(path));
}

TEST(StudioProjectOpen, IpcWithoutWindowPreservesSchemaFailureCodesWithoutWriting)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    for (int version : {1, 2, VNX::ProjectSchema::kProjectSchemaVersion + 1}) {
        QTemporaryDir directory;
        const QString path = writeProject(directory, version);
        const auto original = treeBytes(directory.path());
        VNX::IPCServer server(nullptr);
        server.setProjectManager(&VNX::ProjectManager::instance());
        const auto result = server.dispatchCommand(QStringLiteral("open_project"), {
            {QStringLiteral("path"), path},
        });
        EXPECT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(result.value(QStringLiteral("code")).toString(),
                  version < VNX::ProjectSchema::kProjectSchemaVersion
                      ? QStringLiteral("PROJECT_SCHEMA_UNSUPPORTED")
                      : QStringLiteral("PROJECT_REQUIRES_NEWER_STUDIO"));
        EXPECT_TRUE(result.value(QStringLiteral("message")).toString().contains(
            version < VNX::ProjectSchema::kProjectSchemaVersion
                ? QStringLiteral("no longer supports") : QStringLiteral("newer project format")));
        EXPECT_EQ(treeBytes(directory.path()), original);
        EXPECT_EQ(VNX::ProjectManager::instance().currentProject(), nullptr);
    }
}

TEST(StudioProjectOpen, IpcWithWindowRejectsRetiredGenerationWithoutWriting)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory, 2);
    const auto original = treeBytes(directory.path());
    VNX::MainWindow window;
    VNX::IPCServer server(nullptr);
    server.setProjectManager(&VNX::ProjectManager::instance());
    server.setMainWindow(&window);
    ModalResponder response(false);
    const auto result = server.dispatchCommand(QStringLiteral("open_project"), {
        {QStringLiteral("path"), path},
    });
    EXPECT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(result.value(QStringLiteral("code")).toString(), QStringLiteral("PROJECT_SCHEMA_UNSUPPORTED"));
    EXPECT_TRUE(result.value(QStringLiteral("message")).toString().contains(QStringLiteral("no longer supports")));
    EXPECT_EQ(response.confirmations, 0);
    EXPECT_EQ(response.failures, 1);
    EXPECT_TRUE(response.failureText.contains(QStringLiteral("no longer supports")));
    EXPECT_EQ(treeBytes(directory.path()), original);
    EXPECT_EQ(VNX::ProjectManager::instance().currentProject(), nullptr);
}

TEST(StudioProjectOpen, IpcPreservesRecoveryLockFailureWithAndWithoutWindow)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    for (bool withWindow : {false, true}) {
        SCOPED_TRACE(withWindow);
        QTemporaryDir directory;
        const QString path = writeProject(directory);
        QString error;
        auto transaction = VNX::ProjectTransaction::begin(
            path, {QStringLiteral("Legacy.hsp")}, &error);
        ASSERT_NE(transaction, nullptr) << error.toStdString();
        const auto rollback = qScopeGuard([&]() { transaction->rollback(); });
        ASSERT_TRUE(VNX::ProjectTransaction::recoveryRequired(path));
        ASSERT_EQ(VNX::ProjectUpgradeManager().analyzeForProjectOpen(path).issue,
                  VNX::ProjectIssue::ProjectUpgradeRecoveryRequired);
        const auto original = treeBytes(directory.path());
        std::unique_ptr<VNX::MainWindow> window;
        if (withWindow)
            window = std::make_unique<VNX::MainWindow>();
        VNX::IPCServer server(nullptr);
        server.setProjectManager(&VNX::ProjectManager::instance());
        server.setMainWindow(window.get());
        ModalResponder response(false);
        const auto result = server.dispatchCommand(QStringLiteral("open_project"), {
            {QStringLiteral("path"), path},
        });
        EXPECT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(result.value(QStringLiteral("code")).toString(), QStringLiteral("PROJECT_IN_USE"));
        EXPECT_TRUE(result.value(QStringLiteral("message")).toString().contains(
            QStringLiteral("using this project")));
        EXPECT_EQ(response.confirmations, 0);
        EXPECT_EQ(response.failures, withWindow ? 1 : 0);
        EXPECT_EQ(treeBytes(directory.path()), original);
        EXPECT_TRUE(VNX::ProjectTransaction::recoveryRequired(path));
        EXPECT_EQ(VNX::ProjectManager::instance().currentProject(), nullptr);
    }
}

TEST(StudioProjectOpen, IpcWithoutWindowRecoversInterruptedCurrentProject)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QTemporaryDir directory;
    const QString path = writeProject(directory);
    const QByteArray original = readBytes(path);
    const QString staging = directory.filePath(QStringLiteral("staging"));
    QJsonObject changed = readObject(path);
    QJsonObject content = HarkenTest::projectContent(changed);
    content.insert(QStringLiteral("extensions"), QJsonObject{{QStringLiteral("interruptedWrite"), true}});
    changed.insert(QStringLiteral("content"), content);
    writeObject(QDir(staging).filePath(QStringLiteral("Legacy.hsp")), changed);
    QString error;
    auto transaction = VNX::ProjectTransaction::begin(path, {QStringLiteral("Legacy.hsp")}, &error);
    ASSERT_NE(transaction, nullptr) << error.toStdString();
    ASSERT_TRUE(transaction->replaceFromDirectory(staging, &error)) << error.toStdString();
    transaction.reset();
    ASSERT_TRUE(VNX::ProjectTransaction::recoveryRequired(path));
    ASSERT_FALSE(VNX::ProjectUpgradeManager().analyzeForProjectOpen(path).ok);
    VNX::IPCServer server(nullptr);
    server.setProjectManager(&VNX::ProjectManager::instance());
    const auto result = server.dispatchCommand(QStringLiteral("open_project"), {
        {QStringLiteral("path"), path},
    });
    EXPECT_EQ(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"))
        << QJsonDocument(result).toJson().toStdString();
    EXPECT_EQ(readBytes(path), original);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(path));
    EXPECT_NE(VNX::ProjectManager::instance().currentProject(), nullptr);
}

}
