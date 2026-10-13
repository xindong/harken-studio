#include "Integrations/ProjectIntegrationExecutor.h"
#include "Integrations/EngineTargetProbe.h"
#include "Integrations/IntegrationProgress.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QLockFile>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

bool writeFile(const QString& path, const QByteArray& data)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(data) == data.size();
}

bool writeUnrealHeaders(const QString& source)
{
    const QDir include(QDir(source).filePath("Source/HarkenCore/Include/harken"));
    return writeFile(include.filePath("Harken.h"), "#include <harken/ObjectId.h>\n")
        && writeFile(include.filePath("ObjectId.h"), "typedef struct { unsigned char bytes[16]; } VnObjectId;\n");
}

}

class UnrealMigrationTest : public ::testing::Test
{
protected:
    using Executor = VNH::ProjectIntegrationExecutor;
    using Operation = VNH::IntegrationOperation;
    QTemporaryDir directory{QDir::current().filePath(".temp/unreal-migration-XXXXXX")};
    VNH::EngineTargetRecord target;
    QString source;
    QString original;
    QString destination;

    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        const QString root = directory.filePath("Game");
        source = directory.filePath("SDK");
        original = QDir(root).filePath("Plugins/Harken");
        destination = QDir(root).filePath("Plugins/Audio/Harken");
        ASSERT_TRUE(writeFile(QDir(root).filePath("Game.uproject"), "{}"));
        ASSERT_TRUE(writeFile(QDir(source).filePath("Harken.uplugin"), R"({"VersionName":"1.0"})"));
        ASSERT_TRUE(writeFile(QDir(source).filePath("Runtime.bin"), "runtime"));
        ASSERT_TRUE(writeUnrealHeaders(source));
        ASSERT_TRUE(VNH::EngineTargetProbe::resolve(root, &target));
        const auto install = Executor::planUnreal(target, source, Operation::Install);
        ASSERT_TRUE(install.valid) << install.error.toStdString();
        ASSERT_TRUE(Executor::executeUnreal(target, install, Operation::Install, true).success);
    }

    QByteArray read(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }

    void restoreJournal(const VNH::SdkMigrationPlan& plan)
    {
        ASSERT_TRUE(writeFile(Executor::unrealMigrationJournalPath(target),
            read(QDir(QFileInfo(plan.backupRoot).absolutePath()).filePath("migration.json"))));
    }
};

TEST_F(UnrealMigrationTest, IncompleteSourcesCannotModifyNewOrInstalledProjects)
{
    const QString header = QDir(source).filePath("Source/HarkenCore/Include/harken/ObjectId.h");
    const QByteArray contents = read(header);
    const QByteArray ownership = read(QDir(original).filePath(".harken-integration-state.json"));
    const QString custom = QDir(original).filePath("custom.txt");
    ASSERT_TRUE(writeFile(custom, "keep"));
    VNH::EngineTargetRecord freshTarget;
    const QString freshRoot = directory.filePath("NewGame");
    ASSERT_TRUE(writeFile(QDir(freshRoot).filePath("NewGame.uproject"), "{}"));
    ASSERT_TRUE(VNH::EngineTargetProbe::resolve(freshRoot, &freshTarget));

    for (const bool empty : {false, true}) {
        const auto approvedInstall = Executor::planUnreal(freshTarget, source, Operation::Install);
        const auto approvedUpdate = Executor::planUnreal(target, source, Operation::Update);
        ASSERT_TRUE(approvedInstall.valid && approvedUpdate.valid);
        if (empty) {
            ASSERT_TRUE(writeFile(header, {}));
        } else {
            ASSERT_TRUE(QFile::remove(header));
        }
        const auto install = Executor::planUnreal(freshTarget, source, Operation::Install);
        EXPECT_FALSE(install.valid);
        EXPECT_FALSE(install.error.isEmpty());
        for (const auto operation : {Operation::Update, Operation::Repair}) {
            const auto plan = Executor::planUnreal(target, source, operation);
            EXPECT_FALSE(plan.valid);
            EXPECT_FALSE(plan.error.isEmpty());
            EXPECT_FALSE(Executor::executeUnreal(target, plan, operation, true).success);
        }
        EXPECT_FALSE(Executor::executeUnreal(freshTarget, approvedInstall, Operation::Install, true).success);
        EXPECT_FALSE(Executor::executeUnreal(target, approvedUpdate, Operation::Update, true).success);
        EXPECT_FALSE(QFileInfo::exists(QDir(freshRoot).filePath("Plugins")));
        EXPECT_EQ(read(QDir(original).filePath(".harken-integration-state.json")), ownership);
        EXPECT_EQ(read(QDir(original).filePath("Source/HarkenCore/Include/harken/ObjectId.h")), contents);
        EXPECT_EQ(read(custom), "keep");
        ASSERT_TRUE(writeFile(header, contents));
    }
    ASSERT_TRUE(QFile::remove(QDir(original).filePath("Source/HarkenCore/Include/harken/ObjectId.h")));
    const auto repair = Executor::planUnreal(target, source, Operation::Repair);
    ASSERT_TRUE(repair.valid) << repair.error.toStdString();
    ASSERT_TRUE(Executor::executeUnreal(target, repair, Operation::Repair, true).success);
    EXPECT_EQ(read(QDir(original).filePath("Source/HarkenCore/Include/harken/ObjectId.h")), contents);
    EXPECT_EQ(read(custom), "keep");
}

TEST_F(UnrealMigrationTest, PreservesEntireInstallationAndMaintainsItAtTheNewLocation)
{
    ASSERT_TRUE(writeFile(QDir(original).filePath("Runtime.bin"), "locally modified"));
    ASSERT_TRUE(writeFile(QDir(original).filePath(".custom/notes.txt"), "custom"));
    ASSERT_TRUE(QDir().mkpath(QDir(original).filePath("EmptyDirectory")));
    const QByteArray ownership = read(QDir(original).filePath(".harken-integration-state.json"));
    const auto plan = Executor::planUnrealMigration(target, destination);
    ASSERT_TRUE(plan.valid) << plan.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(plan.backupRoot));
    EXPECT_FALSE(Executor::hasPendingUnrealMigration(target));
    EXPECT_FALSE(Executor::executeUnrealMigration(target, plan, false).success);
    const auto moved = Executor::executeUnrealMigration(target, plan, true);
    ASSERT_TRUE(moved.success) << moved.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(original));
    EXPECT_TRUE(QDir(QDir(destination).filePath("EmptyDirectory")).exists());
    EXPECT_EQ(read(QDir(destination).filePath("Runtime.bin")), "locally modified");
    EXPECT_EQ(read(QDir(destination).filePath(".harken-integration-state.json")), ownership);
    EXPECT_EQ(read(target.descriptorPath), "{}");
    EXPECT_EQ(moved.installedPath, "Plugins/Audio/Harken");
    const auto observation = VNH::EngineTargetProbe::observe(target);
    EXPECT_EQ(observation.sdkInstallPath, moved.installedPath);
    EXPECT_TRUE(observation.canChangeSdk && observation.canMoveSdk && observation.canRemoveSdk);
    auto inspection = Executor::inspectUnreal(target);
    EXPECT_TRUE(inspection.valid);
    EXPECT_EQ(inspection.modifiedManagedFiles, QStringList{"Runtime.bin"});
    ASSERT_TRUE(writeFile(QDir(source).filePath("Runtime.bin"), "updated"));
    const auto update = Executor::planUnreal(target, source, Operation::Update);
    ASSERT_EQ(update.targetRoot, destination);
    ASSERT_TRUE(Executor::executeUnreal(target, update, Operation::Update, true).success);
    EXPECT_EQ(read(QDir(destination).filePath(".custom/notes.txt")), "custom");
    EXPECT_EQ(read(QDir(destination).filePath("Runtime.bin")), "updated");
    const auto remove = Executor::planUnreal(target, {}, Operation::Remove);
    ASSERT_EQ(remove.targetRoot, destination);
    ASSERT_TRUE(Executor::executeUnreal(target, remove, Operation::Remove, true).success);
    EXPECT_FALSE(QFileInfo::exists(QDir(destination).filePath("Harken.uplugin")));
    EXPECT_EQ(read(QDir(destination).filePath(".custom/notes.txt")), "custom");
    target.sdkInstallPath = moved.installedPath;
    const auto reinstall = Executor::planUnreal(target, source, Operation::Install);
    ASSERT_TRUE(reinstall.valid);
    EXPECT_EQ(reinstall.targetRoot, destination);
    EXPECT_TRUE(Executor::executeUnreal(target, reinstall, Operation::Install, true).success);
}

TEST_F(UnrealMigrationTest, RejectsUnsafeDestinationsDuplicatesAndStaleApproval)
{
    ASSERT_TRUE(writeFile(QDir(target.canonicalRoot).filePath("Plugins/Other/Other.uplugin"), "{}"));
    ASSERT_TRUE(writeFile(QDir(target.canonicalRoot).filePath("Plugins/Occupied/notes.txt"), "keep"));
    for (const QString& path : {QString("Plugins"), QString("Source/Harken"), QString("../Outside"),
            QString("Plugins/../Outside"), original, original + "/Nested", QString("Plugins/Other/Nested"),
            QString("Plugins/Occupied"), directory.filePath("Outside")}) {
        const auto plan = Executor::planUnrealMigration(target, path);
        EXPECT_FALSE(plan.valid) << path.toStdString();
        EXPECT_FALSE(plan.error.isEmpty());
    }
    const auto approved = Executor::planUnrealMigration(target, destination);
    ASSERT_TRUE(approved.valid);
    ASSERT_TRUE(writeFile(QDir(original).filePath("new-file.txt"), "new"));
    EXPECT_FALSE(Executor::executeUnrealMigration(target, approved, true).success);
    EXPECT_FALSE(QFileInfo::exists(approved.backupRoot));
    EXPECT_FALSE(Executor::hasPendingUnrealMigration(target));
    ASSERT_TRUE(writeFile(QDir(destination).filePath("Harken.uplugin"), "{}"));
    EXPECT_FALSE(Executor::inspectUnreal(target).valid);
    EXPECT_FALSE(Executor::planUnreal(target, source, Operation::Update).valid);
    EXPECT_FALSE(Executor::planUnrealMigration(target, "Plugins/Elsewhere").valid);
    const auto observation = VNH::EngineTargetProbe::observe(target);
    EXPECT_FALSE(observation.canChangeSdk || observation.canMoveSdk || observation.canRemoveSdk);
    EXPECT_TRUE(observation.error.contains("Multiple Harken plugins"));
}

TEST_F(UnrealMigrationTest, RecoversBeforeAndAfterRenameAndFromVerifiedBackup)
{
    const auto plan = Executor::planUnrealMigration(target, destination);
    ASSERT_TRUE(plan.valid);
    ASSERT_TRUE(Executor::executeUnrealMigration(target, plan, true).success);
    restoreJournal(plan);
    EXPECT_FALSE(Executor::planUnreal(target, source, Operation::Update).valid);
    EXPECT_FALSE(Executor::planUnreal(target, {}, Operation::Remove).valid);
    const auto blocked = VNH::EngineTargetProbe::observe(target);
    EXPECT_TRUE(blocked.canMoveSdk);
    EXPECT_FALSE(blocked.canChangeSdk || blocked.canRemoveSdk);
    auto recover = Executor::planUnrealMigration(target, {}, true);
    ASSERT_TRUE(recover.valid) << recover.error.toStdString();
    ASSERT_TRUE(Executor::executeUnrealMigration(target, recover, true).success);
    EXPECT_TRUE(QFileInfo::exists(QDir(original).filePath("Harken.uplugin")));
    EXPECT_FALSE(QFileInfo::exists(destination));
    EXPECT_FALSE(Executor::hasPendingUnrealMigration(target));
    restoreJournal(plan);
    recover = Executor::planUnrealMigration(target, {}, true);
    ASSERT_TRUE(recover.valid);
    ASSERT_TRUE(Executor::executeUnrealMigration(target, recover, true).success);
    restoreJournal(plan);
    ASSERT_TRUE(QDir(original).removeRecursively());
    const QString other = QDir(target.canonicalRoot).filePath("Plugins/Other/Harken.uplugin");
    ASSERT_TRUE(writeFile(other, "{}"));
    EXPECT_FALSE(Executor::planUnrealMigration(target, {}, true).valid);
    ASSERT_TRUE(QFile::remove(other));
    recover = Executor::planUnrealMigration(target, {}, true);
    ASSERT_TRUE(recover.valid);
    const auto restored = Executor::executeUnrealMigration(target, recover, true);
    ASSERT_TRUE(restored.success) << restored.error.toStdString();
    EXPECT_EQ(restored.installedPath, "Plugins/Harken");
    EXPECT_EQ(read(QDir(original).filePath("Runtime.bin")), "runtime");
    EXPECT_TRUE(Executor::inspectUnreal(target).valid);
}

TEST_F(UnrealMigrationTest, RecoveryRejectsModifiedFilesCorruptBackupsAndEscapingRecords)
{
    const auto plan = Executor::planUnrealMigration(target, destination);
    ASSERT_TRUE(Executor::executeUnrealMigration(target, plan, true).success);
    restoreJournal(plan);
    const auto reviewed = Executor::planUnrealMigration(target, {}, true);
    ASSERT_TRUE(reviewed.valid);
    ASSERT_TRUE(writeFile(QDir(destination).filePath("Runtime.bin"), "newer user changes"));
    EXPECT_FALSE(Executor::executeUnrealMigration(target, reviewed, true).success);
    EXPECT_FALSE(Executor::planUnrealMigration(target, {}, true).valid);
    EXPECT_EQ(read(QDir(destination).filePath("Runtime.bin")), "newer user changes");
    ASSERT_TRUE(writeFile(QDir(destination).filePath("Runtime.bin"), "runtime"));
    ASSERT_TRUE(writeFile(QDir(plan.backupRoot).filePath("Runtime.bin"), "corrupt"));
    EXPECT_FALSE(Executor::planUnrealMigration(target, {}, true).valid);
    ASSERT_TRUE(writeFile(QDir(plan.backupRoot).filePath("Runtime.bin"), "runtime"));
    auto journal = QJsonDocument::fromJson(read(Executor::unrealMigrationJournalPath(target))).object();
    journal.insert("source", "../Outside");
    ASSERT_TRUE(writeFile(Executor::unrealMigrationJournalPath(target), QJsonDocument(journal).toJson()));
    EXPECT_FALSE(Executor::planUnrealMigration(target, {}, true).valid);
    EXPECT_FALSE(QFileInfo::exists(original));
    EXPECT_TRUE(Executor::hasPendingUnrealMigration(target));
}

TEST_F(UnrealMigrationTest, ChecksProjectLockAndAllowsStoppingReadOnlyScans)
{
    const auto plan = Executor::planUnrealMigration(target, destination);
    ASSERT_TRUE(plan.valid);
    QLockFile lock(QDir(target.canonicalRoot).filePath(".harken-sdk-operation.lock"));
    ASSERT_TRUE(lock.tryLock());
    EXPECT_FALSE(Executor::executeUnrealMigration(target, plan, true).success);
    EXPECT_FALSE(QFileInfo::exists(plan.backupRoot));
    lock.unlock();
    std::atomic<quint64> inspected{0};
    std::atomic<bool> cancelled{true};
    VNH::IntegrationProgress progress(inspected, cancelled);
    EXPECT_THROW(Executor::planUnrealMigration(target, destination), VNH::IntegrationScanStopped);
    EXPECT_FALSE(Executor::hasPendingUnrealMigration(target));
    EXPECT_TRUE(QFileInfo::exists(original));
}

#ifdef Q_OS_WIN
TEST_F(UnrealMigrationTest, RejectsDirectoryJunctionsAtSourceDestinationAndBackup)
{
    const QString external = directory.filePath("External");
    ASSERT_TRUE(QDir().mkpath(external));
    for (const QString& link : {QDir(target.canonicalRoot).filePath("Plugins/Linked"),
                               QDir(original).filePath("LinkedContent"),
                               QDir(target.canonicalRoot).filePath("Saved/HarkenHub/Backups")}) {
        ASSERT_TRUE(QDir().mkpath(QFileInfo(link).absolutePath()));
        ASSERT_EQ(QProcess::execute(QStringLiteral("cmd.exe"),
            {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
             QDir::toNativeSeparators(link), QDir::toNativeSeparators(external)}), 0);
        const auto removeLink = qScopeGuard([&]() { QDir().rmdir(link); });
        EXPECT_FALSE(Executor::planUnrealMigration(target, destination).valid);
    }
    EXPECT_TRUE(QDir(external).isEmpty());
}

TEST_F(UnrealMigrationTest, RequiresClosingOnlyTheTargetBeforeMigrationAndRecovery)
{
    const auto plan = Executor::planUnrealMigration(target, destination);
    ASSERT_TRUE(plan.valid);
    const QString editor = directory.filePath("UnrealEditor.exe");
    ASSERT_TRUE(QFile::copy(qEnvironmentVariable("COMSPEC"), editor));
    QProcess process;
    const auto stop = qScopeGuard([&]() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); } });
    process.start(editor, {"/d", "/q", "/k", "rem", QDir::toNativeSeparators(directory.filePath("Other/Game.uproject"))});
    ASSERT_TRUE(process.waitForStarted());
    EXPECT_TRUE(Executor::planUnrealMigration(target, destination).valid);
    process.kill();
    ASSERT_TRUE(process.waitForFinished());
    process.start(editor, {"/d", "/q", "/k", "rem", QDir::toNativeSeparators(target.descriptorPath)});
    ASSERT_TRUE(process.waitForStarted());
    EXPECT_FALSE(Executor::planUnrealMigration(target, destination).valid);
    EXPECT_FALSE(Executor::executeUnrealMigration(target, plan, true).success);
    EXPECT_FALSE(QFileInfo::exists(plan.backupRoot));
    process.kill();
    ASSERT_TRUE(process.waitForFinished());
    ASSERT_TRUE(Executor::executeUnrealMigration(target, plan, true).success);
    restoreJournal(plan);
    const auto recovery = Executor::planUnrealMigration(target, {}, true);
    ASSERT_TRUE(recovery.valid);
    process.start(editor, {"/d", "/q", "/k", "rem", QDir::toNativeSeparators(target.descriptorPath)});
    ASSERT_TRUE(process.waitForStarted());
    EXPECT_FALSE(Executor::planUnrealMigration(target, {}, true).valid);
    EXPECT_FALSE(Executor::executeUnrealMigration(target, recovery, true).success);
    EXPECT_TRUE(Executor::hasPendingUnrealMigration(target));
}
#endif

TEST(ProjectIntegrationExecutorTest, UpdatesOnlyChangedFilesAndPreservesTargetOnlyFiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectRoot =
        QDir(directory.path()).filePath(QStringLiteral("Game"));
    const QString descriptor =
        QDir(projectRoot).filePath(QStringLiteral("Game.uproject"));
    const QString sourceRoot =
        QDir(directory.path()).filePath(QStringLiteral("Sdk/Harken"));
    ASSERT_TRUE(writeFile(descriptor, QByteArrayLiteral("{}")));
    ASSERT_TRUE(writeFile(
        QDir(sourceRoot).filePath(QStringLiteral("Harken.uplugin")),
        QByteArrayLiteral("{\"VersionName\":\"1.0\"}")));
    ASSERT_TRUE(writeUnrealHeaders(sourceRoot));
    ASSERT_TRUE(writeFile(
        QDir(sourceRoot).filePath(QStringLiteral("Binaries/runtime.bin")),
        QByteArrayLiteral("one")));

    VNH::EngineTargetRecord target;
    target.engineKind = VNH::EngineKind::Unreal;
    target.canonicalRoot = projectRoot;
    target.descriptorPath = descriptor;

    const VNH::IntegrationPlan install =
        VNH::ProjectIntegrationExecutor::planUnreal(
            target, sourceRoot, VNH::IntegrationOperation::Install);
    ASSERT_TRUE(install.valid) << install.error.toStdString();
    ASSERT_TRUE(VNH::ProjectIntegrationExecutor::executeUnreal(
                    target, install, VNH::IntegrationOperation::Install, true)
                    .success);
    EXPECT_TRUE(QFileInfo::exists(QDir(install.targetRoot).filePath(
        QStringLiteral(".harken-integration-state.json"))));

    const QString targetOnly = QDir(projectRoot).filePath(
        QStringLiteral("Plugins/Harken/Config/Custom.ini"));
    ASSERT_TRUE(writeFile(targetOnly, QByteArrayLiteral("custom")));
    const QDir targetDirectory(install.targetRoot);
    const QString hiddenFile = targetDirectory.filePath(
        QStringLiteral(".user-config.ini"));
    const QString hiddenDirectory = targetDirectory.filePath(
        QStringLiteral(".user"));
    const QString nestedFile = QDir(hiddenDirectory).filePath(
        QStringLiteral("nested/Notes.txt"));
    ASSERT_TRUE(writeFile(hiddenFile, QByteArrayLiteral("custom")));
    ASSERT_TRUE(writeFile(nestedFile, QByteArrayLiteral("custom")));
#ifdef Q_OS_WIN
    for (const QString& path : {hiddenFile, hiddenDirectory}) {
        const auto nativePath = reinterpret_cast<LPCWSTR>(path.utf16());
        const DWORD attributes = GetFileAttributesW(nativePath);
        ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
        ASSERT_TRUE(SetFileAttributesW(nativePath,
                                      attributes | FILE_ATTRIBUTE_HIDDEN));
    }
#endif
    ASSERT_TRUE(QFileInfo(hiddenFile).isHidden());
    ASSERT_TRUE(QFileInfo(hiddenDirectory).isHidden());
    const VNH::IntegrationPlan current =
        VNH::ProjectIntegrationExecutor::planUnreal(
            target, sourceRoot, VNH::IntegrationOperation::Update);
    ASSERT_TRUE(current.valid) << current.error.toStdString();
    EXPECT_TRUE(current.changes.empty());

    ASSERT_TRUE(writeFile(
        QDir(sourceRoot).filePath(QStringLiteral("Binaries/runtime.bin")),
        QByteArrayLiteral("two")));
    const VNH::IntegrationPlan update =
        VNH::ProjectIntegrationExecutor::planUnreal(
            target, sourceRoot, VNH::IntegrationOperation::Update);
    ASSERT_TRUE(update.valid) << update.error.toStdString();
    ASSERT_EQ(update.changes.size(), 1u);
    EXPECT_EQ(update.changes.front().action, QStringLiteral("replace"));
    EXPECT_EQ(update.preservedTargetFiles,
              (QStringList{QStringLiteral(".user-config.ini"),
                           QStringLiteral(".user/nested/Notes.txt"),
                           QStringLiteral("Config/Custom.ini")}));
    const QString lateTargetOnly = QDir(projectRoot).filePath(
        QStringLiteral("Plugins/Harken/Source/Late.Build.cs"));
    ASSERT_TRUE(writeFile(lateTargetOnly, QByteArrayLiteral("late")));
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::executeUnreal(
                    target, update, VNH::IntegrationOperation::Update, true)
                    .success);
    const auto rechecked = VNH::ProjectIntegrationExecutor::planUnreal(
        target, sourceRoot, VNH::IntegrationOperation::Update);
    ASSERT_TRUE(rechecked.valid);
    ASSERT_TRUE(VNH::ProjectIntegrationExecutor::executeUnreal(
                    target, rechecked, VNH::IntegrationOperation::Update, true).success);
    for (const QString& path : {targetOnly, hiddenFile, nestedFile}) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
        EXPECT_EQ(file.readAll(), QByteArrayLiteral("custom"));
    }
    EXPECT_TRUE(QFileInfo::exists(lateTargetOnly));
    const auto inspected = VNH::ProjectIntegrationExecutor::inspectUnreal(target);
    ASSERT_TRUE(inspected.valid);
    EXPECT_TRUE(inspected.modifiedManagedFiles.isEmpty());
    const auto removal = VNH::ProjectIntegrationExecutor::planUnreal(
        target, {}, VNH::IntegrationOperation::Remove);
    ASSERT_TRUE(removal.valid);
    const auto removed = VNH::ProjectIntegrationExecutor::executeUnreal(
        target, removal, VNH::IntegrationOperation::Remove, true);
    ASSERT_TRUE(removed.success) << removed.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(QDir(removed.backupPath).filePath("Harken.uplugin")));
    EXPECT_FALSE(removed.backupPath.contains("Plugins/"));
    EXPECT_FALSE(QFileInfo::exists(QDir(install.targetRoot).filePath("Harken.uplugin")));
    for (const QString& path : {targetOnly, hiddenFile, nestedFile, lateTargetOnly})
        EXPECT_TRUE(QFileInfo::exists(path));
    EXPECT_EQ(VNH::EngineTargetProbe::observe(target).health, VNH::EngineTargetHealth::NotInstalled);
    const auto reinstall = VNH::ProjectIntegrationExecutor::planUnreal(
        target, sourceRoot, VNH::IntegrationOperation::Install);
    ASSERT_TRUE(reinstall.valid) << reinstall.error.toStdString();
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::executeUnreal(
        target, reinstall, VNH::IntegrationOperation::Install, true).success);
}

TEST(ProjectIntegrationExecutorTest, RequiresConfirmationToOverwriteModifiedManagedFiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectRoot =
        QDir(directory.path()).filePath(QStringLiteral("Game"));
    const QString descriptor =
        QDir(projectRoot).filePath(QStringLiteral("Game.uproject"));
    const QString sourceRoot =
        QDir(directory.path()).filePath(QStringLiteral("Sdk/Harken"));
    const QString sourceRuntime =
        QDir(sourceRoot).filePath(QStringLiteral("Binaries/runtime.bin"));
    const QString installedRuntime =
        QDir(projectRoot).filePath(
            QStringLiteral("Plugins/Harken/Binaries/runtime.bin"));
    ASSERT_TRUE(writeFile(descriptor, QByteArrayLiteral("{}")));
    ASSERT_TRUE(writeFile(
        QDir(sourceRoot).filePath(QStringLiteral("Harken.uplugin")),
        QByteArrayLiteral("{\"VersionName\":\"1.0\"}")));
    ASSERT_TRUE(writeUnrealHeaders(sourceRoot));
    ASSERT_TRUE(writeFile(sourceRuntime, QByteArrayLiteral("one")));

    VNH::EngineTargetRecord target;
    target.engineKind = VNH::EngineKind::Unreal;
    target.canonicalRoot = projectRoot;
    target.descriptorPath = descriptor;

    const VNH::IntegrationPlan install =
        VNH::ProjectIntegrationExecutor::planUnreal(
            target, sourceRoot, VNH::IntegrationOperation::Install);
    ASSERT_TRUE(install.valid) << install.error.toStdString();
    ASSERT_TRUE(VNH::ProjectIntegrationExecutor::executeUnreal(
                    target, install, VNH::IntegrationOperation::Install, true)
                    .success);

    ASSERT_TRUE(writeFile(installedRuntime, QByteArrayLiteral("local")));
    ASSERT_TRUE(writeFile(sourceRuntime, QByteArrayLiteral("two")));
    const VNH::IntegrationPlan update =
        VNH::ProjectIntegrationExecutor::planUnreal(
            target, sourceRoot, VNH::IntegrationOperation::Update);
    ASSERT_TRUE(update.valid) << update.error.toStdString();
    EXPECT_TRUE(update.confirmationRequired);
    EXPECT_TRUE(update.modifiedManagedFiles.contains(
        QStringLiteral("Binaries/runtime.bin")));
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::executeUnreal(
                     target, update, VNH::IntegrationOperation::Update, false)
                     .success);
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::executeUnreal(
                    target, update, VNH::IntegrationOperation::Update, true)
                    .success);

    QFile installed(installedRuntime);
    ASSERT_TRUE(installed.open(QIODevice::ReadOnly));
    EXPECT_EQ(installed.readAll(), QByteArrayLiteral("two"));
}

TEST(ProjectIntegrationExecutorTest, DetectsProjectKindsAndRejectsAmbiguousFolders)
{
    QTemporaryDir directory(QDir::current().filePath(".temp/engine-probe-XXXXXX"));
    ASSERT_TRUE(directory.isValid());
    const QString root = directory.filePath("Game");
    VNH::EngineTargetRecord target;
    QString error;
    EXPECT_FALSE(VNH::EngineTargetProbe::resolve(root, &target, &error));
    for (const auto* folder : {"Assets", "Packages", "ProjectSettings"})
        ASSERT_TRUE(QDir().mkpath(QDir(root).filePath(folder)));
    ASSERT_TRUE(VNH::EngineTargetProbe::resolve(root, &target, &error));
    EXPECT_EQ(target.engineKind, VNH::EngineKind::Unity);
    const QString descriptor = QDir(root).filePath("Game.uproject");
    ASSERT_TRUE(writeFile(descriptor, "{}"));
    EXPECT_FALSE(VNH::EngineTargetProbe::resolve(root, &target, &error));
    ASSERT_TRUE(VNH::EngineTargetProbe::resolve(descriptor, &target, &error));
    EXPECT_EQ(target.engineKind, VNH::EngineKind::Unreal);
    const QString bundle = directory.filePath("SDK/Unreal/Harken");
    ASSERT_TRUE(writeFile(QDir(bundle).filePath("Harken.uplugin"), R"({"VersionName":"1.0"})"));
    for (const QString& input : {directory.filePath("SDK"), directory.filePath("SDK/Unreal"), bundle})
        EXPECT_EQ(VNH::ProjectIntegrationExecutor::locateUnrealSource(input), bundle);
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::locateUnrealSource(root).isEmpty());
    ASSERT_TRUE(QDir(QDir(root).filePath("Assets")).removeRecursively());
    ASSERT_TRUE(VNH::EngineTargetProbe::resolve(root, &target, &error));
    EXPECT_EQ(target.engineKind, VNH::EngineKind::Unreal);
}

TEST(ProjectIntegrationExecutorTest, RejectsSourceChangesAndInvalidOwnershipWithoutTouchingProject)
{
    QTemporaryDir directory(QDir::current().filePath(".temp/unreal-safety-XXXXXX"));
    ASSERT_TRUE(directory.isValid());
    const QString root = directory.filePath("Game");
    const QString source = directory.filePath("SDK");
    ASSERT_TRUE(writeFile(QDir(root).filePath("Game.uproject"), "{}"));
    ASSERT_TRUE(writeFile(QDir(source).filePath("Harken.uplugin"), R"({"VersionName":"1.0"})"));
    ASSERT_TRUE(writeFile(QDir(source).filePath("Runtime.bin"), "one"));
    ASSERT_TRUE(writeUnrealHeaders(source));
    VNH::EngineTargetRecord target;
    ASSERT_TRUE(VNH::EngineTargetProbe::resolve(root, &target));
    const auto plan = VNH::ProjectIntegrationExecutor::planUnreal(target, source, VNH::IntegrationOperation::Install);
    ASSERT_TRUE(plan.valid) << plan.error.toStdString();
    ASSERT_TRUE(writeFile(QDir(source).filePath("Runtime.bin"), "two"));
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::executeUnreal(target, plan, VNH::IntegrationOperation::Install, true).success);
    EXPECT_FALSE(QDir(plan.targetRoot).exists());
    const auto fresh = VNH::ProjectIntegrationExecutor::planUnreal(target, source, VNH::IntegrationOperation::Install);
    ASSERT_TRUE(VNH::ProjectIntegrationExecutor::executeUnreal(target, fresh, VNH::IntegrationOperation::Install, true).success);
    ASSERT_TRUE(writeFile(QDir(plan.targetRoot).filePath("Runtime.bin"), "modified"));
    const auto inspection = VNH::ProjectIntegrationExecutor::inspectUnreal(target);
    EXPECT_TRUE(inspection.valid);
    EXPECT_EQ(inspection.modifiedManagedFiles, QStringList{"Runtime.bin"});
    const QString recordPath = QDir(plan.targetRoot).filePath(".harken-integration-state.json");
    ASSERT_TRUE(writeFile(recordPath, R"({"schema_version":1,"engine":"unreal","sdk_version":"1.0","managed_files":{"../outside.txt":"bad"}})"));
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::inspectUnreal(target).valid);
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::planUnreal(target, {}, VNH::IntegrationOperation::Remove).valid);
    EXPECT_TRUE(QFileInfo::exists(QDir(plan.targetRoot).filePath("Runtime.bin")));
}

#ifdef Q_OS_WIN
TEST(ProjectIntegrationExecutorTest, BlocksOnlyTheRunningTargetAndRechecksBeforeWriting)
{
    QTemporaryDir directory(QDir::current().filePath(".temp/unreal-process-XXXXXX"));
    ASSERT_TRUE(directory.isValid());
    const QString root = directory.filePath("Game With Spaces");
    const QString descriptor = QDir(root).filePath("Game.uproject");
    const QString source = directory.filePath("SDK");
    ASSERT_TRUE(writeFile(descriptor, "{}"));
    ASSERT_TRUE(writeFile(QDir(source).filePath("Harken.uplugin"), R"({"VersionName":"1.0"})"));
    ASSERT_TRUE(writeUnrealHeaders(source));
    VNH::EngineTargetRecord target;
    ASSERT_TRUE(VNH::EngineTargetProbe::resolve(root, &target));
    const auto plan = VNH::ProjectIntegrationExecutor::planUnreal(target, source, VNH::IntegrationOperation::Install);
    ASSERT_TRUE(plan.valid) << plan.error.toStdString();
    const QString editor = directory.filePath("UnrealEditor.exe");
    ASSERT_TRUE(QFile::copy(qEnvironmentVariable("COMSPEC"), editor));
    QProcess process;
    process.start(editor, {"/d", "/q", "/k", "rem", QDir::toNativeSeparators(descriptor)});
    const auto stop = qScopeGuard([&]() { process.kill(); process.waitForFinished(); });
    ASSERT_TRUE(process.waitForStarted());
    QString error;
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(target, &error));
    EXPECT_TRUE(error.contains("This Unreal project is open")) << error.toStdString();
    auto other = target;
    other.canonicalRoot = directory.filePath("OtherGame");
    other.descriptorPath = QDir(other.canonicalRoot).filePath("Game.uproject");
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(other, &error));
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::executeUnreal(target, plan, VNH::IntegrationOperation::Install, true).success);
    EXPECT_FALSE(QDir(plan.targetRoot).exists());
    process.kill();
    ASSERT_TRUE(process.waitForFinished());
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(target, &error));
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::executeUnreal(target, plan, VNH::IntegrationOperation::Install, true).success);
    process.start(editor, {"/d", "/q", "/k", "rem", "Game.uproject"});
    ASSERT_TRUE(process.waitForStarted());
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(target, &error));
    EXPECT_TRUE(error.contains("relative project path")) << error.toStdString();
    other.descriptorPath = QDir(other.canonicalRoot).filePath("Other.uproject");
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(other, &error));
    process.kill();
    ASSERT_TRUE(process.waitForFinished());
    for (const QString& argument : {QStringLiteral("Other.uproject"),
                                    QStringLiteral("-project=Other.uproject"),
                                    QStringLiteral("-project=Other")}) {
        process.start(editor, {"/d", "/q", "/k", "rem", argument});
        ASSERT_TRUE(process.waitForStarted());
        EXPECT_TRUE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(target, &error))
            << argument.toStdString() << ": " << error.toStdString();
        EXPECT_TRUE(VNH::ProjectIntegrationExecutor::planUnreal(
            target, source, VNH::IntegrationOperation::Update).valid);
        EXPECT_TRUE(VNH::ProjectIntegrationExecutor::planUnreal(
            target, {}, VNH::IntegrationOperation::Remove).valid);
        process.kill();
        ASSERT_TRUE(process.waitForFinished());
    }
    process.start(editor, {"/d", "/q", "/k", "rem", "-project=Game"});
    ASSERT_TRUE(process.waitForStarted());
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(target, &error));
    EXPECT_TRUE(error.contains("relative project path")) << error.toStdString();
}
#endif
