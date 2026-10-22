#include "Integrations/ProjectIntegrationExecutor.h"
#include "Integrations/EngineTargetProbe.h"
#include "Integrations/IntegrationProgress.h"
#include "Integrations/IntegrationFileSystem.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QLockFile>
#include <chrono>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <sddl.h>
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

bool writeUnrealRuntimeFiles(const QString& source)
{
    const QDir include(QDir(source).filePath("Source/HarkenCore/Include/harken"));
    return writeFile(include.filePath("Harken.h"), "#include <harken/ObjectId.h>\n")
        && writeFile(include.filePath("ObjectId.h"), "typedef struct { unsigned char bytes[16]; } VnObjectId;\n")
        && writeFile(source + "/Source/HarkenCore/Lib/Win64/HarkenCoreImport.lib", "import")
        && writeFile(source + "/Binaries/ThirdParty/HarkenCore/Win64/HarkenCore.dll", "runtime")
        && writeFile(source + "/Binaries/ThirdParty/HarkenCore/Mac/libHarkenCore.dylib", "runtime")
        && writeFile(source + "/Binaries/ThirdParty/HarkenCore/Linux/x86_64/libHarkenCore.so", "runtime");
}

}

TEST(IntegrationDirectoryTest, ExistingDestinationIsNeverReplaced)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source = directory.filePath("stage %3");
    const QString target = directory.filePath("installed %4");
    ASSERT_TRUE(writeFile(source + "/new.txt", "new"));
    ASSERT_TRUE(writeFile(target + "/existing.txt", "existing"));
    QString error;
    EXPECT_FALSE(VNH::renameIntegrationDirectory(source, target, &error));
    EXPECT_TRUE(error.contains(source));
    EXPECT_TRUE(error.contains(target));
    EXPECT_TRUE(QFileInfo::exists(source + "/new.txt"));
    QFile preserved(target + "/existing.txt");
    ASSERT_TRUE(preserved.open(QIODevice::ReadOnly));
    EXPECT_EQ(preserved.readAll(), "existing");
}

TEST(IntegrationDirectoryTest, ChecksCreationAndRenamePermissionsWithoutChangingContents)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString parent = directory.filePath("plugins");
    const QString installed = parent + "/SDK";
    ASSERT_TRUE(writeFile(installed + "/payload.txt", "original"));
    QString error;
    EXPECT_TRUE(VNH::checkIntegrationDirectoryChanges({parent + "/nested/new"}, {installed}, &error));
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    const auto closeToken = qScopeGuard([&]() { CloseHandle(token); });
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    ASSERT_GT(bytes, 0u);
    QByteArray tokenData(bytes, Qt::Uninitialized);
    ASSERT_TRUE(GetTokenInformation(token, TokenUser, tokenData.data(), bytes, &bytes));
    LPWSTR sid = nullptr;
    ASSERT_TRUE(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tokenData.data())->User.Sid, &sid));
    const QString account = '*' + QString::fromWCharArray(sid);
    LocalFree(sid);
    const auto restore = qScopeGuard([&]() {
        QProcess::execute("icacls", {parent, "/remove:d", account});
        QProcess::execute("icacls", {installed, "/remove:d", account});
    });
    ASSERT_EQ(QProcess::execute("icacls", {parent, "/deny", account + ":(AD)"}), 0);
#else
    const auto permissions = QFileInfo(parent).permissions();
    const auto restore = qScopeGuard([&]() { QFile::setPermissions(parent, permissions); });
    ASSERT_TRUE(QFile::setPermissions(parent, permissions & ~(QFile::WriteOwner | QFile::WriteGroup | QFile::WriteOther | QFile::WriteUser)));
#endif
    EXPECT_TRUE(VNH::checkIntegrationFilesWritable({installed + "/payload.txt"}, &error));
    EXPECT_FALSE(VNH::checkIntegrationDirectoryChanges({parent + "/nested/new"}, {}, &error)) << error.toStdString();
    EXPECT_TRUE(error.contains(QDir::toNativeSeparators(parent))) << error.toStdString();
#ifdef Q_OS_WIN
    ASSERT_EQ(QProcess::execute("icacls", {parent, "/remove:d", account}), 0);
    ASSERT_EQ(QProcess::execute("icacls", {parent, "/deny", account + ":(DC)"}), 0);
    ASSERT_EQ(QProcess::execute("icacls", {installed, "/deny", account + ":(D)"}), 0);
#endif
    EXPECT_FALSE(VNH::checkIntegrationDirectoryChanges({}, {installed}, &error)) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(parent + "/nested"));
    QFile payload(installed + "/payload.txt");
    ASSERT_TRUE(payload.open(QIODevice::ReadOnly));
    EXPECT_EQ(payload.readAll(), "original");
}

#ifdef Q_OS_WIN
TEST(IntegrationFileWriteTest, TransientLockAllowsAtomicReplacementAfterRelease)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = QDir::toNativeSeparators(directory.filePath("report.json"));
    ASSERT_TRUE(writeFile(path, "previous report"));
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const std::jthread release([handle]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CloseHandle(handle);
    });
    QString error;
    EXPECT_TRUE(VNH::writeIntegrationFile(path, "new report", &error)) << error.toStdString();
    QFile report(path);
    ASSERT_TRUE(report.open(QIODevice::ReadOnly));
    EXPECT_EQ(report.readAll(), "new report");
}

TEST(IntegrationFileWriteTest, PersistentLockKeepsPreviousContentsAndReportsFailure)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = QDir::toNativeSeparators(directory.filePath("report.json"));
    ASSERT_TRUE(writeFile(path, "previous report"));
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    {
        const auto close = qScopeGuard([handle]() { CloseHandle(handle); });
        QString error;
        EXPECT_FALSE(VNH::writeIntegrationFile(path, "new report", &error));
        EXPECT_FALSE(error.isEmpty());
        QFile report(path);
        ASSERT_TRUE(report.open(QIODevice::ReadOnly));
        EXPECT_EQ(report.readAll(), "previous report");
    }
    QString error;
    EXPECT_TRUE(VNH::writeIntegrationFile(path, "new report", &error)) << error.toStdString();
    QFile report(path);
    ASSERT_TRUE(report.open(QIODevice::ReadOnly));
    EXPECT_EQ(report.readAll(), "new report");
}

TEST(IntegrationFileWriteTest, WritePreflightReportsReadOnlyAndLockedFilesWithoutChangingThem)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString readOnly = directory.filePath("readonly %1.txt");
    const QString locked = directory.filePath("locked %2.txt");
    ASSERT_TRUE(writeFile(readOnly, "original readonly"));
    ASSERT_TRUE(writeFile(locked, "original locked"));
    const auto permissions = QFileInfo(readOnly).permissions();
    const auto restore = qScopeGuard([&]() { QFile::setPermissions(readOnly, permissions); });
    ASSERT_TRUE(QFile::setPermissions(readOnly, QFile::ReadOwner | QFile::ReadUser));
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(locked.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    QString error;
    EXPECT_FALSE(VNH::checkIntegrationFilesWritable({readOnly, locked}, &error));
    CloseHandle(handle);
    EXPECT_TRUE(error.contains("Read-only files:"));
    EXPECT_TRUE(error.contains("Files that could not be opened for writing:"));
    EXPECT_TRUE(error.contains(QDir::toNativeSeparators(readOnly)));
    EXPECT_TRUE(error.contains(QDir::toNativeSeparators(locked)));
    ASSERT_TRUE(QFile::setPermissions(readOnly, permissions));
    EXPECT_TRUE(VNH::checkIntegrationFilesWritable({readOnly, locked}, &error));
    EXPECT_TRUE(error.isEmpty());
    QFile original(readOnly);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), "original readonly");
    QFile other(locked);
    ASSERT_TRUE(other.open(QIODevice::ReadOnly));
    EXPECT_EQ(other.readAll(), "original locked");
}

TEST(IntegrationDirectoryTest, TransientFileLockAllowsAtomicRenameAfterRelease)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source = directory.filePath("stage");
    const QString target = directory.filePath("installed");
    const QString file = QDir::toNativeSeparators(source + "/payload.txt");
    ASSERT_TRUE(writeFile(file, "payload"));
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(file.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const std::jthread release([handle]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CloseHandle(handle);
    });
    EXPECT_TRUE(VNH::renameIntegrationDirectory(source, target));
    EXPECT_FALSE(QFileInfo::exists(source));
    QFile installed(target + "/payload.txt");
    ASSERT_TRUE(installed.open(QIODevice::ReadOnly));
    EXPECT_EQ(installed.readAll(), "payload");
}

TEST(IntegrationDirectoryTest, PersistentFileLockPreservesSourceAndAllowsLaterRetry)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source = directory.filePath("stage");
    const QString target = directory.filePath("installed");
    const QString file = QDir::toNativeSeparators(source + "/payload.txt");
    ASSERT_TRUE(writeFile(file, "payload"));
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(file.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    {
        const auto close = qScopeGuard([handle]() { CloseHandle(handle); });
        QString error;
        EXPECT_FALSE(VNH::renameIntegrationDirectory(source, target, &error));
        EXPECT_TRUE(error.contains(source));
        EXPECT_TRUE(error.contains(target));
        EXPECT_TRUE(error.contains("system error"));
        EXPECT_FALSE(QFileInfo::exists(target));
        QFile preserved(file);
        ASSERT_TRUE(preserved.open(QIODevice::ReadOnly));
        EXPECT_EQ(preserved.readAll(), "payload");
    }
    EXPECT_TRUE(VNH::renameIntegrationDirectory(source, target));
    EXPECT_TRUE(QFileInfo::exists(target + "/payload.txt"));
}
#endif

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
        ASSERT_TRUE(writeUnrealRuntimeFiles(source));
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

TEST_F(UnrealMigrationTest, WriteProtectionBlocksSdkChangesAfterApprovalAndAllowsRetry)
{
    const QString changed = original + "/Runtime.bin";
    const QString receipt = original + "/.harken-integration-state.json";
    const QByteArray originalReceipt = read(receipt);
    ASSERT_TRUE(writeFile(source + "/Runtime.bin", "updated"));
    for (const auto operation : {Operation::Update, Operation::Repair, Operation::Remove}) {
        const auto reviewed = Executor::planUnreal(target, source, operation);
        ASSERT_TRUE(reviewed.valid) << reviewed.error.toStdString();
        for (const auto& path : {changed, receipt}) {
            SCOPED_TRACE(path.toStdString());
            const auto permissions = QFileInfo(path).permissions();
            const auto restore = qScopeGuard([&]() { QFile::setPermissions(path, permissions); });
            ASSERT_TRUE(QFile::setPermissions(path, QFile::ReadOwner | QFile::ReadUser));
            const auto blocked = Executor::planUnreal(target, source, operation);
            EXPECT_FALSE(blocked.valid);
            EXPECT_TRUE(blocked.error.contains(QDir::toNativeSeparators(path)));
            const auto result = Executor::executeUnreal(target, reviewed, operation, true);
            EXPECT_FALSE(result.success);
            EXPECT_TRUE(result.error.contains(QDir::toNativeSeparators(path)));
            EXPECT_EQ(read(changed), "runtime");
            EXPECT_EQ(read(receipt), originalReceipt);
            EXPECT_TRUE(result.backupPath.isEmpty());
        }
    }
    const QString custom = original + "/Custom.txt";
    const QString unchanged = original + "/Harken.uplugin";
    ASSERT_TRUE(writeFile(custom, "preserved"));
    const auto permissions = QFileInfo(unchanged).permissions();
    const auto restore = qScopeGuard([&]() {
        for (const auto& path : {custom, unchanged, source + "/Runtime.bin"}) QFile::setPermissions(path, permissions);
    });
    for (const auto& path : {custom, unchanged, source + "/Runtime.bin"})
        ASSERT_TRUE(QFile::setPermissions(path, QFile::ReadOwner | QFile::ReadUser));
    const auto reviewed = Executor::planUnreal(target, source, Operation::Update);
    ASSERT_TRUE(reviewed.valid) << reviewed.error.toStdString();
    EXPECT_TRUE(Executor::executeUnreal(target, reviewed, Operation::Update, true).success);
    EXPECT_EQ(read(changed), "updated");
    EXPECT_EQ(read(custom), "preserved");
}

TEST_F(UnrealMigrationTest, IncompleteSourcesCannotModifyNewOrInstalledProjects)
{
    const QStringList required = {
        "Source/HarkenCore/Include/harken/ObjectId.h",
#ifdef Q_OS_WIN
        "Source/HarkenCore/Lib/Win64/HarkenCoreImport.lib",
        "Binaries/ThirdParty/HarkenCore/Win64/HarkenCore.dll",
#elif defined(Q_OS_MACOS)
        "Binaries/ThirdParty/HarkenCore/Mac/libHarkenCore.dylib",
#else
        "Binaries/ThirdParty/HarkenCore/Linux/x86_64/libHarkenCore.so",
#endif
    };
    for (const auto& relative : required) {
        SCOPED_TRACE(relative.toStdString());
        const QString header = QDir(source).filePath(relative);
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
            EXPECT_EQ(read(QDir(original).filePath(relative)), contents);
            EXPECT_EQ(read(custom), "keep");
            ASSERT_TRUE(writeFile(header, contents));
        }
        ASSERT_TRUE(QFile::remove(QDir(original).filePath(relative)));
        auto state = QJsonDocument::fromJson(ownership).object();
        auto managed = state.value("managed_files").toObject();
        managed.remove(relative);
        state.insert("managed_files", managed);
        ASSERT_TRUE(writeFile(QDir(original).filePath(".harken-integration-state.json"), QJsonDocument(state).toJson()));
        EXPECT_FALSE(Executor::inspectUnreal(target).valid);
        EXPECT_TRUE(Executor::planUnreal(target, {}, Operation::Remove).valid);
        ASSERT_TRUE(writeFile(QDir(original).filePath(".harken-integration-state.json"), ownership));
        const auto repair = Executor::planUnreal(target, source, Operation::Repair);
        ASSERT_TRUE(repair.valid) << repair.error.toStdString();
        ASSERT_TRUE(Executor::executeUnreal(target, repair, Operation::Repair, true).success);
        EXPECT_EQ(read(QDir(original).filePath(relative)), contents);
        EXPECT_EQ(read(custom), "keep");
    }
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
    ASSERT_TRUE(writeUnrealRuntimeFiles(sourceRoot));
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
    ASSERT_TRUE(writeUnrealRuntimeFiles(sourceRoot));
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
    ASSERT_TRUE(writeUnrealRuntimeFiles(source));
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
    const auto changed = VNH::EngineTargetProbe::observe(target, true);
    EXPECT_EQ(changed.sdkContentState, VNH::SdkContentState::Modified);
    EXPECT_TRUE(changed.sdkContentDetails.contains("Runtime.bin"));
    EXPECT_TRUE(changed.canChangeSdk);
    const QString requiredFile = "Source/HarkenCore/Include/harken/ObjectId.h";
    const QString requiredPath = QDir(plan.targetRoot).filePath(requiredFile);
    ASSERT_TRUE(QFile::remove(requiredPath));
    const auto incomplete = VNH::ProjectIntegrationExecutor::inspectUnreal(target);
    EXPECT_FALSE(incomplete.valid);
    EXPECT_EQ(incomplete.missingManagedFiles, QStringList{requiredFile});
    EXPECT_TRUE(incomplete.modifiedManagedFiles.contains(requiredFile));
    const auto repair = VNH::ProjectIntegrationExecutor::planUnreal(target, source, VNH::IntegrationOperation::Update);
    ASSERT_TRUE(repair.valid) << repair.error.toStdString();
    EXPECT_EQ(repair.missingManagedFiles, QStringList{requiredFile});
    EXPECT_TRUE(repair.unreadableManagedFiles.isEmpty());
    const auto both = VNH::EngineTargetProbe::observe(target, true);
    EXPECT_EQ(both.sdkContentState, VNH::SdkContentState::ModifiedAndMissing);
    EXPECT_TRUE(both.sdkContentDetails.contains(requiredFile));
    EXPECT_TRUE(both.sdkContentDetails.contains("Runtime.bin"));
    ASSERT_TRUE(writeFile(QDir(plan.targetRoot).filePath("Runtime.bin"), "two"));
    EXPECT_EQ(VNH::EngineTargetProbe::observe(target, true).sdkContentState,
              VNH::SdkContentState::Missing);
    EXPECT_FALSE(QFileInfo::exists(requiredPath));
    ASSERT_TRUE(QFile::copy(QDir(source).filePath(requiredFile), requiredPath));
    EXPECT_EQ(VNH::EngineTargetProbe::observe(target, true).sdkContentState,
              VNH::SdkContentState::MatchesInstallation);
    const QString runtimePath = QDir(plan.targetRoot).filePath("Runtime.bin");
    ASSERT_TRUE(QFile::remove(runtimePath));
    ASSERT_TRUE(QDir().mkpath(runtimePath));
    const auto unreadable = VNH::ProjectIntegrationExecutor::inspectUnreal(target);
    EXPECT_EQ(unreadable.unreadableManagedFiles, QStringList{"Runtime.bin"});
    EXPECT_EQ(unreadable.modifiedManagedFiles, QStringList{"Runtime.bin"});
    EXPECT_TRUE(unreadable.missingManagedFiles.isEmpty());
    const auto unreadablePlan = VNH::ProjectIntegrationExecutor::planUnreal(target, source, VNH::IntegrationOperation::Update);
    EXPECT_FALSE(unreadablePlan.valid);
    EXPECT_EQ(unreadablePlan.unreadableManagedFiles, QStringList{"Runtime.bin"});
    EXPECT_EQ(VNH::EngineTargetProbe::observe(target, true).sdkContentState,
              VNH::SdkContentState::Unverified);
    ASSERT_TRUE(QDir().rmdir(runtimePath));
    ASSERT_TRUE(writeFile(runtimePath, "modified"));
    ASSERT_TRUE(writeFile(recordPath, R"({"schema_version":1,"engine":"unreal","sdk_version":"1.0","managed_files":{"../outside.txt":"bad"}})"));
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::inspectUnreal(target).valid);
    EXPECT_EQ(VNH::EngineTargetProbe::observe(target, true).sdkContentState,
              VNH::SdkContentState::Unverified);
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
    ASSERT_TRUE(writeUnrealRuntimeFiles(source));
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
    bool isOpen = false;
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(target, &error, &isOpen));
    EXPECT_TRUE(isOpen);
    EXPECT_TRUE(error.contains("This Unreal project is open")) << error.toStdString();
    auto other = target;
    other.canonicalRoot = directory.filePath("OtherGame");
    other.descriptorPath = QDir(other.canonicalRoot).filePath("Game.uproject");
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(other, &error, &isOpen));
    EXPECT_FALSE(isOpen);
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::executeUnreal(target, plan, VNH::IntegrationOperation::Install, true).success);
    EXPECT_FALSE(QDir(plan.targetRoot).exists());
    process.kill();
    ASSERT_TRUE(process.waitForFinished());
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(target, &error));
    EXPECT_TRUE(VNH::ProjectIntegrationExecutor::executeUnreal(target, plan, VNH::IntegrationOperation::Install, true).success);
    process.start(editor, {"/d", "/q", "/k", "rem", "Game.uproject"});
    ASSERT_TRUE(process.waitForStarted());
    EXPECT_FALSE(VNH::ProjectIntegrationExecutor::checkUnrealClosed(target, &error, &isOpen));
    EXPECT_FALSE(isOpen);
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
