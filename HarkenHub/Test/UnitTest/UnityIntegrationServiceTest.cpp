#include "UnityIntegrationService.h"
#include "UnityInstallation.h"
#include "Integrations/EngineTargetProbe.h"
#include "Integrations/HubUnityIntegrationService.h"
#include "Integrations/Unity/UnityProjectAccess.h"
#include "Support/UnityProjectLock.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

using Service = VNX::UnityIntegrationService;

void writeFile(const QString& path, const QByteArray& contents)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << path.toStdString();
    ASSERT_EQ(file.write(contents), contents.size());
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

QMap<QString, QByteArray> tree(const QString& root)
{
    QMap<QString, QByteArray> files;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        files.insert(QDir(root).relativeFilePath(path), readFile(path));
    }
    return files;
}

QString makeUnityProject(const QString& root)
{
    for (const auto* folder : {"Assets", "Packages", "ProjectSettings"})
        EXPECT_TRUE(QDir().mkpath(QDir(root).filePath(QLatin1String(folder))));
    return root;
}

QString makeCompleteIntegration(const QString& root, const QString& version = QStringLiteral("0.2.0-test"))
{
    writeFile(QDir(root).filePath("package.json"), QJsonDocument(QJsonObject{
        {"name", "com.harken.core"}, {"version", version}}).toJson());
    writeFile(QDir(root).filePath("Runtime/Harken.Core.asmdef"), R"({"name":"Harken.Core"})");
    writeFile(QDir(root).filePath("Runtime/API/Values.cs"), "// values " + version.toUtf8());
    writeFile(QDir(root).filePath("Runtime/Unity/Harken.Unity.asmdef"), R"({"name":"Harken.Unity","references":["Harken.Core"]})");
    writeFile(QDir(root).filePath("Runtime/Plugins/x86_64/HarkenEngine.dll"), "windows-runtime");
    writeFile(QDir(root).filePath("Runtime/Plugins/macOS/libHarkenEngine.dylib"), "macos-runtime");
    return root;
}

QString diagnostics(const Service::Result& result)
{
    QStringList lines;
    for (const auto& step : result.steps)
        lines << step.id + ": " + step.summary + " " + step.cause + " " + step.details.join(' ');
    return lines.join('\n');
}

} // namespace

class UnityIntegrationTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.filePath("HubData").toUtf8());
        project = makeUnityProject(dir.filePath("Game"));
        source = makeCompleteIntegration(dir.filePath("SDK"));
    }

    void TearDown() override { qunsetenv("HARKEN_HUB_USER_ROOT"); }

    Service::Result run(Service::Request request)
    {
        request.unityProjectPath = project;
        return service.run(request);
    }

    Service::Result install(const QString& location = {}, bool checkOnly = false)
    {
        Service::Request request;
        request.integrationSourcePath = source;
        request.integrationInstallPath = location;
        request.checkOnly = checkOnly;
        return run(request);
    }

    Service::Result remove(bool checkOnly = false)
    {
        Service::Request request;
        request.removeIntegration = true;
        request.checkOnly = checkOnly;
        return run(request);
    }

    Service::Result restore(bool checkOnly = false)
    {
        Service::Request request;
        request.restorePrevious = true;
        request.checkOnly = checkOnly;
        return run(request);
    }

    QString path(const QString& relative) const { return QDir(project).filePath(relative); }

    QTemporaryDir dir{QDir::current().filePath(".temp/unity-integration-XXXXXX")};
    Service service;
    QString project;
    QString source;
};

TEST_F(UnityIntegrationTest, UpgradeReplacesTheWholeDirectoryWithoutReadingThePreviousVersion)
{
    const QString installed = path("Packages/com.harken.core");
    makeCompleteIntegration(installed, "0.0.1-unknown");
    writeFile(installed + "/Runtime/Removed.cs", "// removed in the new version");
    writeFile(installed + "/Notes/user.txt", "user edit");
    writeFile(installed + "/.harken-integration-state.json", "{}");

    const auto check = install({}, true);
    ASSERT_TRUE(check.success) << diagnostics(check).toStdString();
    EXPECT_TRUE(check.targetInstalled);
    EXPECT_FALSE(check.targetCurrent);
    EXPECT_EQ(check.installedVersion, "0.0.1-unknown");
    EXPECT_EQ(readFile(installed + "/Notes/user.txt"), "user edit");

    const auto result = install();
    ASSERT_TRUE(result.success) << diagnostics(result).toStdString();
    EXPECT_EQ(tree(installed), tree(source));
    EXPECT_FALSE(result.previousVersionAvailable);
    EXPECT_TRUE(Service::previousVersionBackup(project).isEmpty());
    EXPECT_FALSE(QFileInfo::exists(path("Library/HarkenIntegration")));

    const auto again = install({}, true);
    ASSERT_TRUE(again.success);
    EXPECT_TRUE(again.targetCurrent);
    EXPECT_TRUE(install().success);
    EXPECT_EQ(tree(installed), tree(source));
}

TEST_F(UnityIntegrationTest, AssetsInstallationLeavesFolderMetadataAndSettingsOutsideTheDirectoryAlone)
{
    const QString installed = path("Assets/Plugins/Harken");
    ASSERT_TRUE(install("Assets/Plugins/Harken").success);
    writeFile(installed + ".meta", "guid: folder");
    writeFile(path("Assets/Resources/HarkenProjectSettings.asset"), "settings");
    writeFile(path("Assets/Editor/HarkenEditorProjectSettings.asset"), "editor settings");

    makeCompleteIntegration(source, "0.3.0-test");
    const auto upgrade = install("Assets/Plugins/Harken");
    ASSERT_TRUE(upgrade.success) << diagnostics(upgrade).toStdString();
    EXPECT_EQ(tree(installed), tree(source));
    EXPECT_EQ(readFile(installed + ".meta"), "guid: folder");

    const auto removal = remove();
    ASSERT_TRUE(removal.success) << diagnostics(removal).toStdString();
    EXPECT_TRUE(removal.integrationRemoved);
    EXPECT_FALSE(QFileInfo::exists(installed));
    EXPECT_FALSE(QFileInfo::exists(installed + ".meta"));
    EXPECT_EQ(readFile(path("Assets/Resources/HarkenProjectSettings.asset")), "settings");
    EXPECT_EQ(readFile(path("Assets/Editor/HarkenEditorProjectSettings.asset")), "editor settings");
    EXPECT_TRUE(Service::previousVersionBackup(project).isEmpty());
}

TEST_F(UnityIntegrationTest, RefusesDirectoriesWithOtherContentAndSecondInstallations)
{
    writeFile(path("Assets/Audio/Music.wav"), "music");
    EXPECT_FALSE(install("Assets/Audio").success);
    EXPECT_EQ(readFile(path("Assets/Audio/Music.wav")), "music");
    EXPECT_FALSE(install("Assets/Editor/Harken").success);

    ASSERT_TRUE(install().success);
    const auto elsewhere = install("Assets/Plugins/Harken");
    EXPECT_FALSE(elsewhere.success);
    EXPECT_TRUE(elsewhere.steps.front().cause.contains("already installed"));
    EXPECT_FALSE(QFileInfo::exists(path("Assets/Plugins/Harken")));

    makeCompleteIntegration(path("Assets/Copy"));
    const auto duplicate = install();
    EXPECT_FALSE(duplicate.success);
    EXPECT_TRUE(duplicate.steps.front().cause.contains("Multiple Integration Package"));
}

TEST_F(UnityIntegrationTest, FailedInstallationKeepsThePreviousVersionUntilItIsRestored)
{
    const QString installed = path("Packages/com.harken.core");
    ASSERT_TRUE(install().success);
    const auto original = tree(installed);
    makeCompleteIntegration(source, "0.3.0-test");
    {
        QFile held(installed + "/Runtime/API/Values.cs");
#ifdef Q_OS_WIN
        ASSERT_TRUE(held.open(QIODevice::ReadOnly));
#else
        ASSERT_TRUE(QFile::setPermissions(installed + "/Runtime/API", QFile::ReadOwner | QFile::ExeOwner));
#endif
        const auto failed = install();
        EXPECT_FALSE(failed.success);
        EXPECT_TRUE(failed.previousVersionAvailable);
#ifndef Q_OS_WIN
        ASSERT_TRUE(QFile::setPermissions(installed + "/Runtime/API",
                                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
#endif
    }
    EXPECT_FALSE(Service::previousVersionBackup(project).isEmpty());
    VNH::EngineTargetRecord record;
    record.engineKind = VNH::EngineKind::Unity;
    record.canonicalRoot = project;
    EXPECT_TRUE(VNH::EngineTargetProbe::observe(record).canRestorePrevious);

    const auto preview = restore(true);
    ASSERT_TRUE(preview.success) << diagnostics(preview).toStdString();
    EXPECT_EQ(preview.installedVersion, "0.2.0-test");
    const auto restored = restore();
    ASSERT_TRUE(restored.success) << diagnostics(restored).toStdString();
    EXPECT_TRUE(restored.previousRestored);
    EXPECT_EQ(tree(installed), original);
    EXPECT_TRUE(Service::previousVersionBackup(project).isEmpty());
    EXPECT_FALSE(VNH::EngineTargetProbe::observe(record).canRestorePrevious);
    EXPECT_FALSE(restore().success);
}

TEST_F(UnityIntegrationTest, SettingsInsideTheInstallationDirectoryBlockReplacementAndRemoval)
{
    ASSERT_TRUE(install("Assets/Harken").success);
    writeFile(path("Assets/Harken/Resources/HarkenProjectSettings.asset"), "settings");
    const auto upgrade = install("Assets/Harken");
    EXPECT_FALSE(upgrade.success);
    EXPECT_TRUE(upgrade.steps.front().cause.contains("HarkenProjectSettings.asset"));
    EXPECT_FALSE(remove().success);
    EXPECT_EQ(readFile(path("Assets/Harken/Resources/HarkenProjectSettings.asset")), "settings");
}

TEST_F(UnityIntegrationTest, BackupKeepsEveryFileAndIncompleteBackupsAreNotOffered)
{
    const QString installed = path("Packages/com.harken.core");
    makeCompleteIntegration(installed, "0.0.1-unknown");
    ASSERT_TRUE(QFile::remove(installed + "/package.json"));
    writeFile(installed + "/.git/HEAD", "ref: refs/heads/main");
    writeFile(installed + "/Unreal/Notes.txt", "notes");
    const auto original = tree(installed);
    {
        QFile held(installed + "/Runtime/API/Values.cs");
#ifdef Q_OS_WIN
        ASSERT_TRUE(held.open(QIODevice::ReadOnly));
#else
        ASSERT_TRUE(QFile::setPermissions(installed + "/Runtime/API", QFile::ReadOwner | QFile::ExeOwner));
#endif
        EXPECT_FALSE(install().success);
#ifndef Q_OS_WIN
        ASSERT_TRUE(QFile::setPermissions(installed + "/Runtime/API",
                                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
#endif
    }
    const QString backup = Service::previousVersionBackup(project);
    ASSERT_FALSE(backup.isEmpty());
    ASSERT_TRUE(restore().success);
    EXPECT_EQ(tree(installed), original);

    ASSERT_TRUE(QDir(installed).removeRecursively());
    makeCompleteIntegration(installed);
    {
        QFile held(installed + "/Runtime/API/Values.cs");
#ifdef Q_OS_WIN
        ASSERT_TRUE(held.open(QIODevice::ReadOnly));
#else
        ASSERT_TRUE(QFile::setPermissions(installed + "/Runtime/API", QFile::ReadOwner | QFile::ExeOwner));
#endif
        EXPECT_FALSE(install().success);
#ifndef Q_OS_WIN
        ASSERT_TRUE(QFile::setPermissions(installed + "/Runtime/API",
                                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
#endif
    }
    ASSERT_FALSE(Service::previousVersionBackup(project).isEmpty());
    ASSERT_TRUE(QFile::remove(QDir(Service::previousVersionBackup(project)).filePath("package/package.json")));
    EXPECT_TRUE(Service::previousVersionBackup(project).isEmpty());
    EXPECT_FALSE(restore().success);
}

TEST_F(UnityIntegrationTest, CheckOnlyNeverChangesTheProject)
{
    ASSERT_TRUE(install({}, true).success);
    EXPECT_FALSE(QFileInfo::exists(path("Packages/com.harken.core")));
    ASSERT_TRUE(install().success);
    const auto before = tree(path("Packages/com.harken.core"));
    makeCompleteIntegration(source, "0.3.0-test");
    ASSERT_TRUE(install({}, true).success);
    ASSERT_TRUE(remove(true).success);
    EXPECT_EQ(tree(path("Packages/com.harken.core")), before);
    EXPECT_TRUE(Service::previousVersionBackup(project).isEmpty());
}

TEST_F(UnityIntegrationTest, ChangesRequireUnityToBeClosed)
{
    ASSERT_TRUE(install().success);
    const auto before = tree(path("Packages/com.harken.core"));
    makeCompleteIntegration(source, "0.3.0-test");
    HarkenHubTest::UnityProjectLock unityLock;
    ASSERT_TRUE(unityLock.lock(project));
    for (const auto& result : {install(), install({}, true), remove(), restore()}) {
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.steps.front().cause.contains("This Unity project is open."));
    }
    EXPECT_EQ(tree(path("Packages/com.harken.core")), before);
    ASSERT_TRUE(unityLock.unlock());
    EXPECT_TRUE(install().success);
}

TEST_F(UnityIntegrationTest, BlocksPackageManagerReferencesAndPartialCopiesButIgnoresCaches)
{
    makeCompleteIntegration(path("Library/PackageCache/com.harken.core@unused"));
    makeCompleteIntegration(path("Backups/SDK"));
    EXPECT_TRUE(VNX::UnityInstallation::discover(project).installations.isEmpty());
    const QString manifest = path("Packages/manifest.json");
    writeFile(manifest, R"({"dependencies":{"com.harken.core":"1.0.0"}})");
    EXPECT_FALSE(install().success);
    writeFile(manifest, R"({"dependencies":{"com.harken.core":"file:../../SDK"}})");
    const auto referenced = install();
    EXPECT_FALSE(referenced.success);
    EXPECT_TRUE(referenced.steps.front().cause.contains(source));
    ASSERT_TRUE(QFile::remove(manifest));
    writeFile(path("Assets/Plugins/libharken_core.so"), "partial");
    const auto partial = install();
    EXPECT_FALSE(partial.success);
    EXPECT_TRUE(partial.steps.front().cause.contains("without an identifiable"));
}

TEST_F(UnityIntegrationTest, MovedInstallationIsUpdatedInPlace)
{
    ASSERT_TRUE(install().success);
    ASSERT_TRUE(QDir().rename(path("Packages/com.harken.core"), path("Assets/RenamedAudio")));
    const auto scan = VNX::UnityInstallation::discover(project);
    ASSERT_TRUE(scan.errors.isEmpty());
    ASSERT_EQ(scan.installations.size(), 1);
    EXPECT_EQ(scan.installations.front().path, path("Assets/RenamedAudio"));
    EXPECT_FALSE(install().success);
    EXPECT_TRUE(install("Assets/RenamedAudio").success);
    EXPECT_FALSE(QFileInfo::exists(path("Packages/com.harken.core")));
}

TEST(UnityIntegrationService, RejectsInvalidExplicitIntegrationSourceWithoutFallback)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    qputenv("HARKEN_HUB_USER_ROOT", dir.filePath("HubData").toUtf8());
    const QString unityRoot = makeUnityProject(dir.filePath("UnityGame"));
    const QString invalidIntegrationRoot = dir.filePath("NotHarkenSDK");
    ASSERT_TRUE(QDir().mkpath(invalidIntegrationRoot));
    Service service;
    Service::Request request;
    request.unityProjectPath = unityRoot;
    request.integrationSourcePath = invalidIntegrationRoot;
    const auto result = service.run(request);
    qunsetenv("HARKEN_HUB_USER_ROOT");
    ASSERT_FALSE(result.success);
    EXPECT_TRUE(result.integrationSourcePath.isEmpty());
    EXPECT_EQ(result.steps[1].status, Service::StepStatus::Failed);
    EXPECT_TRUE(result.steps[1].cause.contains(invalidIntegrationRoot));
}

TEST(UnityIntegrationService, ResolvesIntegrationTotalDirectoryToUnityPackageRoot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const auto* name : {"HarkenIntegration", "HarkenSDK"}) {
        const QString totalRoot = dir.filePath(QLatin1String(name));
        const QString packageRoot = QDir(totalRoot).filePath("Unity/Harken");
        makeCompleteIntegration(packageRoot);
        EXPECT_EQ(Service::locateIntegrationSource(totalRoot), QDir(packageRoot).absolutePath());
    }
}

TEST(UnityIntegrationService, ResolvesUnityClassificationDirectoryToPackageRoot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = dir.filePath("Unity");
    const QString packageRoot = QDir(unityRoot).filePath("Harken");
    makeCompleteIntegration(packageRoot);
    EXPECT_EQ(Service::locateIntegrationSource(unityRoot), QDir(packageRoot).absolutePath());
    EXPECT_EQ(Service::locateIntegrationSource(packageRoot), QDir(packageRoot).absolutePath());
}

TEST(UnityProjectAccess, DistinguishesStaleFilesFromLiveLocksWithoutChangingThem)
{
    QTemporaryDir dir(QDir::current().filePath(".temp/unity-project-lock-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    qputenv("HARKEN_HUB_USER_ROOT", dir.filePath("HubData").toUtf8());
    const QString project = dir.filePath("Game %2 with spaces");
    const QString path = QDir(project).filePath("Temp/UnityLockfile");
    QString error;
    bool isOpen = true;
    EXPECT_TRUE(VNX::checkUnityProjectClosed(project, &error, &isOpen));
    EXPECT_FALSE(isOpen);
    EXPECT_FALSE(QFileInfo::exists(path));
    writeFile(path, "stale lock contents");
    const auto modified = QFileInfo(path).lastModified();
    EXPECT_TRUE(VNX::checkUnityProjectClosed(project, &error));
    QStringList modes{QStringLiteral("exclusive")};
#ifndef Q_OS_WIN
    modes << QStringLiteral("record");
#endif
    for (const QString& mode : modes) {
        HarkenHubTest::UnityProjectLock unityLock;
        ASSERT_TRUE(unityLock.lock(project, mode));
        EXPECT_FALSE(VNX::checkUnityProjectClosed(project, &error, &isOpen));
        EXPECT_TRUE(isOpen);
        EXPECT_TRUE(error.contains("This Unity project is open."));
        ASSERT_TRUE(unityLock.unlock());
        EXPECT_TRUE(VNX::checkUnityProjectClosed(project, &error, &isOpen));
        EXPECT_FALSE(isOpen);
        EXPECT_TRUE(error.isEmpty());
    }
    EXPECT_EQ(readFile(path), "stale lock contents");
    EXPECT_EQ(QFileInfo(path).lastModified(), modified);
    ASSERT_TRUE(QFile::remove(path));
    ASSERT_TRUE(QDir().mkpath(path));
    EXPECT_FALSE(VNX::checkUnityProjectClosed(project, &error, &isOpen));
    EXPECT_FALSE(isOpen);
    EXPECT_TRUE(error.startsWith("Cannot check whether this Unity project is open:"));
    makeUnityProject(project);
    VNH::UnityIntegrationRequest request;
    request.unityProjectPath = project;
    request.removeIntegration = true;
    const auto result = VNH::HubUnityIntegrationService::run(request);
    qunsetenv("HARKEN_HUB_USER_ROOT");
    ASSERT_FALSE(result.steps.isEmpty());
    EXPECT_EQ(result.steps.front().cause, error);
}
