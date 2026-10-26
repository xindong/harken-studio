#include "Core/ProjectOpenApproval.h"
#include "Core/ProjectInstance.h"
#include "Core/ProjectManager.h"
#include "Core/FileWatcher.h"
#include "Core/MigrationManager.h"
#include "Core/ProjectOpenProtocol.h"
#include "Core/ProjectTransaction.h"
#include "Core/IPC/IPCServer.h"
#include "Core/UndoCommands.h"
#include "MainWindow.h"
#include "Support/AITestSupport.h"
#include "Support/ProjectDocumentTestUtils.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QDirIterator>
#include <QFileDialog>
#include <QJsonArray>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

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

struct ModalResponder
{
    QTimer timer;
    bool acceptUpgrade;

    explicit ModalResponder(bool accept) : acceptUpgrade(accept)
    {
        QObject::connect(&timer, &QTimer::timeout, [&]() {
            QWidget* modal = QApplication::activeModalWidget();
            if (auto* box = qobject_cast<QMessageBox*>(modal)) {
                if (box->objectName() == QStringLiteral("projectUpgradeConfirmation"))
                    box->button(acceptUpgrade ? QMessageBox::Ok : QMessageBox::Cancel)->click();
                else
                    box->done(QMessageBox::Ok);
            }
        });
        timer.start(5);
    }
};

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

TEST(StudioProjectOpen, FailedSaveAsPreservesOriginalProjectSession)
{
    VNX::TestSupport::ensureApp();
    if (runInChild()) return;
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QTemporaryDir directory{QStringLiteral(HARKEN_TEMP_DIR "/save-as-failure-XXXXXX")};
    ASSERT_TRUE(directory.isValid());
    const QString path = writeProject(directory);
    VNX::MainWindow window;
    ASSERT_TRUE(window.loadProject(path));
    auto& projects = VNX::ProjectManager::instance();
    auto* project = projects.currentProject();
    ASSERT_NE(project, nullptr);
    const QString originalName = project->name;
    const QString sourcePath = directory.filePath(QStringLiteral("Legacy/AudioSources/original.wav"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(sourcePath).absolutePath()));
    QFile source(sourcePath);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.close();
    projects.startWatchingCurrentProject();
    ASSERT_EQ(projects.fileWatcher()->sourceFiles(), QStringList{sourcePath});
    auto* pending = projects.createItem(nullptr, QStringLiteral("PendingAction"),
                                        VNX::ProjectItem::Type::Action);
    ASSERT_NE(pending, nullptr);
    const auto originalFiles = treeBytes(QFileInfo(path).absolutePath());
    auto& undo = VNX::UndoRedoManager::instance();
    ASSERT_FALSE(undo.isClean());
    const int undoCount = undo.undoStack()->count();
    std::weak_ptr<VNX::ProjectInstance> originalInstance;
    {
        auto acquisition = VNX::ProjectInstance::acquire(path);
        ASSERT_EQ(acquisition.status, VNX::ProjectInstance::Status::Acquired);
        originalInstance = acquisition.instance;
    }
    const QString destination = directory.filePath(QStringLiteral("Destination/Copy.hsp"));
    const QString obstruction = directory.filePath(QStringLiteral("Destination/Actions"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(destination).absolutePath()));
    QFile blocker(obstruction);
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    ASSERT_EQ(blocker.write("keep"), 4);
    blocker.close();
    std::weak_ptr<VNX::ProjectInstance> targetInstance;
    QStringList failures;
    QObject::connect(&projects, &VNX::ProjectManager::errorOccurred, &window,
                     [&](const QString& error) {
        failures.append(error);
        EXPECT_FALSE(projects.fileWatcher()->isWatching());
        auto acquisition = VNX::ProjectInstance::acquire(destination);
        EXPECT_EQ(acquisition.status, VNX::ProjectInstance::Status::Acquired);
        targetInstance = acquisition.instance;
    });
    QTimer responder;
    QObject::connect(&responder, &QTimer::timeout, [&] {
        if (auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
            picker->selectFile(destination);
            QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
        } else if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            box->done(QMessageBox::Ok);
        }
    });
    responder.start(5);
    ASSERT_TRUE(QMetaObject::invokeMethod(&window, "onSaveProjectAs", Qt::DirectConnection));
    responder.stop();
    ASSERT_EQ(failures.size(), 1);
    EXPECT_EQ(projects.currentProject(), project);
    EXPECT_EQ(project->projectPath, path);
    EXPECT_EQ(project->name, originalName);
    EXPECT_EQ(projects.findItemByGuid(pending->guid), pending);
    EXPECT_FALSE(undo.isClean());
    EXPECT_EQ(undo.undoStack()->count(), undoCount);
    EXPECT_TRUE(projects.fileWatcher()->isWatching());
    EXPECT_EQ(projects.fileWatcher()->sourceFiles(), QStringList{sourcePath});
    EXPECT_FALSE(originalInstance.expired());
    EXPECT_TRUE(targetInstance.expired());
    EXPECT_EQ(treeBytes(QFileInfo(path).absolutePath()), originalFiles);
    EXPECT_FALSE(QFileInfo::exists(destination));
    EXPECT_EQ(readBytes(obstruction), QByteArray("keep"));
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(destination));
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
    EXPECT_EQ(readBytes(path), original);
    EXPECT_FALSE(VNX::ProjectTransaction::recoveryRequired(path));
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
