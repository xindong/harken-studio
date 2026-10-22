#include "UnityIntegrationService.h"
#include "UnityInstallation.h"
#include "Integrations/HubUnityIntegrationService.h"
#include "Integrations/Unity/UnityProjectAccess.h"
#include "Support/UnityProjectLock.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QLockFile>
#include <QProcess>
#include <QScopeGuard>

#include <atomic>

namespace {

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

QString makeUnityProject(const QString& root)
{
    EXPECT_TRUE(QDir().mkpath(QDir(root).filePath(QStringLiteral("Assets"))));
    EXPECT_TRUE(QDir().mkpath(QDir(root).filePath(QStringLiteral("Packages"))));
    EXPECT_TRUE(QDir().mkpath(QDir(root).filePath(QStringLiteral("ProjectSettings"))));
    return root;
}

QJsonObject integrationManifest()
{
    QJsonObject manifest;
    manifest[QStringLiteral("schema_version")] = 3;
    manifest[QStringLiteral("package_name")] = QStringLiteral("com.harken.core");
    manifest[QStringLiteral("supports_custom_install_path")] = true;
    manifest[QStringLiteral("expected_platforms_by_editor_host")] = QJsonObject{
        {QStringLiteral("Windows"), QJsonArray{
            QStringLiteral("Windows"), QStringLiteral("Linux"), QStringLiteral("Android")}},
        {QStringLiteral("macOS"), QJsonArray{
            QStringLiteral("Windows"), QStringLiteral("macOS"),
            QStringLiteral("Android"), QStringLiteral("iOS")}},
        {QStringLiteral("Linux"), QJsonArray{
            QStringLiteral("Windows"), QStringLiteral("Linux"), QStringLiteral("Android")}},
    };
    manifest[QStringLiteral("required_files")] = QJsonArray{
        QStringLiteral("package.json"),
        QStringLiteral("Runtime/Harken.Core.asmdef"),
        QStringLiteral("Runtime/Generated/HarkenAbi.g.cs"),
        QStringLiteral("Runtime/Generated/HarkenNative.g.cs"),
        QStringLiteral("Runtime/Generated/HarkenTypes.g.cs"),
        QStringLiteral("Runtime/Unity/UnityCoordinates.cs"),
        QStringLiteral("Runtime/Unity/Harken.Unity.asmdef"),
        QStringLiteral("Runtime/Unity/UnitySpatial.cs"),
    };

    QJsonArray plugins;
    const auto addPlugin = [&](const QString& platform, const QString& path) {
        plugins.append(QJsonObject{
            {QStringLiteral("platform"), platform},
            {QStringLiteral("alternatives"), QJsonArray{path}},
        });
    };
    addPlugin(QStringLiteral("Windows"),
              QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"));
    addPlugin(QStringLiteral("macOS"),
              QStringLiteral("Runtime/Plugins/macOS/libHarkenCore.dylib"));
    addPlugin(QStringLiteral("Linux"),
              QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so"));
    addPlugin(QStringLiteral("Android"),
              QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so"));
    addPlugin(QStringLiteral("iOS"),
              QStringLiteral("Runtime/Plugins/iOS/libHarkenCore.a"));
    manifest[QStringLiteral("native_plugins")] = plugins;
    return manifest;
}

QString hostRuntimeRelativePath()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("Runtime/Plugins/macOS/libHarkenCore.dylib");
#else
    return QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so");
#endif
}

QStringList requiredRuntimeRelativePaths()
{
#if defined(Q_OS_WIN)
    return {
        QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"),
        QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so"),
        QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so"),
    };
#elif defined(Q_OS_MACOS)
    return {
        QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"),
        QStringLiteral("Runtime/Plugins/macOS/libHarkenCore.dylib"),
        QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so"),
        QStringLiteral("Runtime/Plugins/iOS/libHarkenCore.a"),
    };
#else
    return {
        QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"),
        QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so"),
        QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so"),
    };
#endif
}

QString optionalRuntimeRelativePath()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so");
#else
    return QStringLiteral("Runtime/Plugins/macOS/libHarkenCore.dylib");
#endif
}

void makeUnreadable(const QString& path)
{
    ASSERT_TRUE(QFile::remove(path)) << path.toStdString();
    ASSERT_TRUE(QDir().mkpath(path)) << path.toStdString();
}

QString makeCompleteSdk(const QString& root)
{
    const QByteArray package = QJsonDocument(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("com.harken.core")},
        {QStringLiteral("version"), QStringLiteral("0.2.0-test")},
    }).toJson();
    writeFile(QDir(root).filePath(QStringLiteral("package.json")), package);
    writeFile(QDir(root).filePath(QStringLiteral("harken-integration.json")),
              QJsonDocument(integrationManifest()).toJson());
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Harken.Core.asmdef")),
              QByteArrayLiteral("{\"name\":\"Harken.Core\"}"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Generated/HarkenAbi.g.cs")),
              QByteArrayLiteral("// abi"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Generated/HarkenNative.g.cs")),
              QByteArrayLiteral("// native"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Generated/HarkenTypes.g.cs")),
              QByteArrayLiteral("// types"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Unity/UnityCoordinates.cs")),
              QByteArrayLiteral("// coordinate conversion"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Unity/Harken.Unity.asmdef")),
              QByteArrayLiteral("{\"name\":\"Harken.Unity\",\"references\":[\"Harken.Core\"]}"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Unity/UnitySpatial.cs")),
              QByteArrayLiteral("// unity spatial adapter"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll")),
              QByteArrayLiteral("windows-runtime"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Plugins/macOS/libHarkenCore.dylib")),
              QByteArrayLiteral("macos-runtime"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so")),
              QByteArrayLiteral("linux-runtime"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so")),
              QByteArrayLiteral("android-runtime"));
    writeFile(QDir(root).filePath(QStringLiteral("Runtime/Plugins/iOS/libHarkenCore.a")),
              QByteArrayLiteral("ios-runtime"));
    return root;
}

} // namespace

class UnitySdkMigrationTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
        source = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
        request.unityProjectPath = project;
        request.sdkSourcePath = source;
        ASSERT_TRUE(service.run(request).success);
        original = QDir(project).filePath(QStringLiteral("Packages/com.harken.core"));
        destination = QDir(project).filePath(QStringLiteral("Assets/ThirdParty/HarkenSDK"));
        request.sdkSourcePath.clear();
        request.sdkInstallPath = QStringLiteral("Packages/com.harken.core");
        request.migrationTargetPath = QStringLiteral("Assets/ThirdParty/HarkenSDK");
        request.migrationBackupPath = dir.filePath(QStringLiteral("Backup"));
        request.migrateIntegration = true;
    }

    void approve()
    {
        request.confirmMigration = false;
        const auto preview = service.run(request);
        ASSERT_TRUE(preview.requiresConfirmation) << preview.steps.back().cause.toStdString();
        request.migrationPlanHash = preview.migrationPlanHash;
        request.confirmMigration = true;
    }

    QTemporaryDir dir{QDir::current().filePath(QStringLiteral(".temp/sdk-migration-XXXXXX"))};
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    QString project;
    QString source;
    QString original;
    QString destination;
};

TEST_F(UnitySdkMigrationTest, StaleUnityLockAndEmbeddedResolutionDoNotPreventMaintenanceAfterMoveOrRemoval)
{
    const QString unityLockPath = project + "/Temp/UnityLockfile";
    writeFile(unityLockPath, "stale");
    const QString lockPath = project + "/Packages/packages-lock.json";
    const QByteArray resolution = R"({"dependencies":{"com.harken.core":{"version":"file:com.harken.core","depth":0,"source":"embedded","dependencies":{}}}})";
    writeFile(lockPath, resolution);
    approve();
    const auto moved = service.run(request);
    ASSERT_TRUE(moved.success) << moved.steps.back().cause.toStdString();
    EXPECT_EQ(readFile(lockPath), resolution);
    const auto discovery = VNX::UnityInstallation::discover(project);
    EXPECT_TRUE(discovery.errors.isEmpty());
    EXPECT_TRUE(discovery.sdkReferences.isEmpty());
    ASSERT_EQ(discovery.installations.size(), 1);
    EXPECT_TRUE(VNX::UnityInstallation::samePath(discovery.installations.front().path, destination));
    VNX::UnityIntegrationService::Request remove;
    remove.unityProjectPath = project;
    remove.sdkInstallPath = request.migrationTargetPath;
    remove.removeIntegration = true;
    remove.confirmRemoval = true;
    remove.removalBackupPath = dir.filePath("RemovalBackup");
    ASSERT_TRUE(service.run(remove).success);
    EXPECT_EQ(readFile(unityLockPath), "stale");
    EXPECT_EQ(readFile(lockPath), resolution);
    EXPECT_TRUE(VNX::UnityInstallation::discover(project).sdkReferences.isEmpty());
}

TEST_F(UnitySdkMigrationTest, UnsafeOrNonEmbeddedResolutionsStillBlockMaintenance)
{
    const QString lockPath = project + "/Packages/packages-lock.json";
    for (const QJsonObject& resolution : {
        QJsonObject{{"version", "file:../External"}, {"depth", 0}, {"source", "embedded"}},
        QJsonObject{{"version", "file:%2e%2e%2fExternal"}, {"depth", 0}, {"source", "embedded"}},
        QJsonObject{{"version", "file:D:/External"}, {"depth", 0}, {"source", "embedded"}},
        QJsonObject{{"version", "file:com.harken.core"}, {"depth", 1}, {"source", "embedded"}},
        QJsonObject{{"version", "file:com.harken.core"}, {"source", "embedded"}},
        QJsonObject{{"version", "file:com.harken.core"}, {"depth", 0}, {"source", "local"}},
        QJsonObject{{"version", "1.2.3"}, {"depth", 0}, {"source", "registry"}}
    }) {
        SCOPED_TRACE(QJsonDocument(resolution).toJson().toStdString());
        const QByteArray bytes = QJsonDocument(QJsonObject{{"dependencies",
            QJsonObject{{"com.harken.core", resolution}}}}).toJson();
        writeFile(lockPath, bytes);
        EXPECT_FALSE(VNX::UnityInstallation::discover(project).sdkReferences.isEmpty());
        const auto preview = service.run(request);
        EXPECT_FALSE(preview.success);
        EXPECT_FALSE(preview.requiresConfirmation);
        EXPECT_TRUE(QFileInfo::exists(original));
        EXPECT_FALSE(QFileInfo::exists(destination));
        EXPECT_EQ(readFile(lockPath), bytes);
    }
}

TEST_F(UnitySdkMigrationTest, VerifiesInstalledFilesWithoutSourceAndNeverRewritesThem)
{
    ASSERT_TRUE(QDir(source).removeRecursively());
    writeFile(original + "/Runtime.meta", "guid: preserve-unity-metadata");
    writeFile(original + "/Custom.txt", "project-specific data");
    const QString statePath = original + "/.harken-integration-state.json";
    const QByteArray state = readFile(statePath);
    const auto installedAt = QFileInfo(statePath).lastModified();
    VNH::UnityIntegrationRequest verify;
    verify.unityProjectPath = project;
    verify.sdkSourcePath = source;
    verify.verifyInstalledOnly = true;
    auto result = VNH::HubUnityIntegrationService::run(verify);
    ASSERT_TRUE(result.success);
    EXPECT_FALSE(result.targetAlreadyCurrent);
    EXPECT_EQ(result.sdkFilesCopied, 0);
    EXPECT_TRUE(result.sdkSourcePath.isEmpty());
    EXPECT_TRUE(result.markdownReportPath.isEmpty());
    EXPECT_EQ(result.sdkInstallPath, "Packages/com.harken.core");
    ASSERT_EQ(result.steps.size(), 3);
    EXPECT_EQ(result.steps.back().status, VNH::UnityIntegrationStepStatus::Succeeded);

    const QString missing = "Runtime/Generated/HarkenAbi.g.cs";
    const QString modified = "Runtime/Generated/HarkenNative.g.cs";
    const QString unreadable = "Runtime/Generated/HarkenTypes.g.cs";
    ASSERT_TRUE(QFile::remove(QDir(original).filePath(missing)));
    writeFile(QDir(original).filePath(modified), "local modification");
    makeUnreadable(QDir(original).filePath(unreadable));
    result = VNH::HubUnityIntegrationService::run(verify);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.changesDetected);
    const QString details = result.steps.back().details.join('\n');
    EXPECT_TRUE(details.contains(missing));
    EXPECT_TRUE(details.contains(modified));
    EXPECT_TRUE(details.contains(unreadable));
    EXPECT_FALSE(QFileInfo::exists(QDir(original).filePath(missing)));
    EXPECT_EQ(readFile(QDir(original).filePath(modified)), "local modification");
    EXPECT_EQ(readFile(original + "/Runtime.meta"), "guid: preserve-unity-metadata");
    EXPECT_EQ(readFile(original + "/Custom.txt"), "project-specific data");
    EXPECT_EQ(readFile(statePath), state);
    EXPECT_EQ(QFileInfo(statePath).lastModified(), installedAt);
}

TEST_F(UnitySdkMigrationTest, VerificationRejectsIncompleteAndEscapingOwnershipRecords)
{
    const QString statePath = original + "/.harken-integration-state.json";
    const QJsonObject originalState = QJsonDocument::fromJson(readFile(statePath)).object();
    VNH::UnityIntegrationRequest verify;
    verify.unityProjectPath = project;
    verify.verifyInstalledOnly = true;
    for (const QString& path : {QStringLiteral("../outside.txt"), QStringLiteral("package.json")}) {
        QJsonObject invalid = originalState;
        invalid["managed_files"] = QJsonArray{path};
        invalid["sha256"] = QJsonObject{{path, QString(64, QLatin1Char('0'))}};
        writeFile(statePath, QJsonDocument(invalid).toJson());
        EXPECT_FALSE(VNH::HubUnityIntegrationService::run(verify).success);
    }
    QJsonObject invalid = originalState;
    invalid.remove("sha256");
    writeFile(statePath, QJsonDocument(invalid).toJson());
    auto result = VNH::HubUnityIntegrationService::run(verify);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.steps[1].summary, "The installed SDK has incomplete or invalid verification data.");
    invalid = originalState;
    auto hashes = invalid["sha256"].toObject();
    hashes["package.json"] = QString(64, QLatin1Char('z'));
    invalid["sha256"] = hashes;
    writeFile(statePath, QJsonDocument(invalid).toJson());
    EXPECT_FALSE(VNH::HubUnityIntegrationService::run(verify).success);
    ASSERT_TRUE(QFile::remove(statePath));
    result = VNH::HubUnityIntegrationService::run(verify);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.steps[1].summary.contains("ownership record"));
    EXPECT_FALSE(QFileInfo::exists(statePath));
}

TEST_F(UnitySdkMigrationTest, VerificationDiagnosesMissingUnmanagedAndDuplicateInstallations)
{
    VNH::UnityIntegrationRequest verify;
    verify.unityProjectPath = makeUnityProject(dir.filePath("AnotherGame"));
    verify.verifyInstalledOnly = true;
    auto result = VNH::HubUnityIntegrationService::run(verify);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.targetInstalled);
    EXPECT_EQ(result.steps[1].summary, "No Harken Unity SDK is installed in this project.");
    const QString unmanaged = QDir(verify.unityProjectPath).filePath("Packages/com.harken.core/package.json");
    writeFile(unmanaged, R"({"name":"com.harken.core","version":"0.1.0"})");
    result = VNH::HubUnityIntegrationService::run(verify);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.targetInstalled);
    EXPECT_EQ(result.sdkInstallPath, "Packages/com.harken.core");
    EXPECT_TRUE(result.steps[1].summary.contains("ownership record"));
    EXPECT_TRUE(result.sdkSourcePath.isEmpty());
    EXPECT_FALSE(QDir(QDir(verify.unityProjectPath).filePath("Library")).exists());
    verify.unityProjectPath = project;
    writeFile(QDir(project).filePath("Assets/OtherSDK/package.json"), readFile(unmanaged));
    result = VNH::HubUnityIntegrationService::run(verify);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.steps[1].summary, "Multiple SDK installations were found:");
    EXPECT_EQ(result.steps[1].artifacts.size(), 2);
}

TEST_F(UnitySdkMigrationTest, PreservesModifiedFilesHiddenFilesEmptyDirectoriesMetadataAndOwnership)
{
    const QString modified = QStringLiteral("Runtime/Generated/HarkenAbi.g.cs");
    const QString state = QStringLiteral(".harken-integration-state.json");
    const QByteArray ownership = readFile(QDir(original).filePath(state));
    writeFile(QDir(original).filePath(modified), "custom implementation");
    writeFile(QDir(original).filePath(modified + ".meta"), "guid: custom-script-guid");
    writeFile(QDir(original).filePath(".settings/custom.json"), "custom settings");
    writeFile(QDir(original).filePath("UserContent.txt"), "project content");
    writeFile(original + ".meta", "guid: sdk-root-guid");
    ASSERT_TRUE(QDir().mkpath(QDir(original).filePath("EmptyDirectory")));
    ASSERT_TRUE(QDir(source).removeRecursively());
    approve();
    EXPECT_TRUE(QDir(original).exists());
    EXPECT_FALSE(QFileInfo::exists(request.migrationBackupPath));
    const auto result = service.run(request);
    ASSERT_TRUE(result.success) << result.steps.back().cause.toStdString();
    EXPECT_TRUE(result.integrationMigrated);
    EXPECT_FALSE(result.integrationRemoved);
    EXPECT_FALSE(QFileInfo::exists(original));
    EXPECT_FALSE(QFileInfo::exists(original + ".meta"));
    EXPECT_EQ(readFile(QDir(destination).filePath(modified)), "custom implementation");
    EXPECT_EQ(readFile(QDir(destination).filePath(modified + ".meta")), "guid: custom-script-guid");
    EXPECT_EQ(readFile(QDir(destination).filePath(".settings/custom.json")), "custom settings");
    EXPECT_EQ(readFile(QDir(destination).filePath("UserContent.txt")), "project content");
    EXPECT_EQ(readFile(destination + ".meta"), "guid: sdk-root-guid");
    EXPECT_EQ(readFile(QDir(destination).filePath(state)), ownership);
    EXPECT_TRUE(QDir(QDir(destination).filePath("EmptyDirectory")).exists());
    EXPECT_EQ(readFile(QDir(request.migrationBackupPath).filePath("SDK/" + modified)), "custom implementation");
    EXPECT_EQ(readFile(QDir(request.migrationBackupPath).filePath("SDK.meta")), "guid: sdk-root-guid");
    EXPECT_EQ(VNX::UnityInstallation::discover(project).installations.size(), 1);
    EXPECT_FALSE(VNX::UnityInstallation::hasPendingMigration(project));
    request.sdkInstallPath = result.sdkInstallPath;
    request.migrationTargetPath = QStringLiteral("Packages/com.harken.core");
    request.migrationBackupPath = dir.filePath("SecondBackup");
    approve();
    EXPECT_TRUE(service.run(request).success);
    EXPECT_EQ(readFile(QDir(original).filePath(state)), ownership);
    EXPECT_EQ(readFile(original + ".meta"), "guid: sdk-root-guid");
}

TEST_F(UnitySdkMigrationTest, RejectsUnsafeDestinationsDependenciesAndUnsupportedInstalledSdk)
{
    for (const QString& path : {QStringLiteral("../Outside"), QStringLiteral("Assets/Editor/SDK"),
             QStringLiteral("Packages/com.harken.core"), QStringLiteral("Packages/com.harken.core/Nested")}) {
        request.migrationTargetPath = path;
        EXPECT_FALSE(service.run(request).requiresConfirmation) << path.toStdString();
    }
    request.migrationTargetPath = QStringLiteral("Assets/ThirdParty/HarkenSDK");
    writeFile(QDir(destination).filePath("user.txt"), "keep");
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    ASSERT_TRUE(QFile::remove(QDir(destination).filePath("user.txt")));
    writeFile(destination + ".meta", "guid: existing-target");
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    EXPECT_EQ(readFile(destination + ".meta"), "guid: existing-target");
    ASSERT_TRUE(QFile::remove(destination + ".meta"));
    const QString manifestPath = QDir(project).filePath("Packages/manifest.json");
    for (const QByteArray& manifest : {QByteArray(R"({"dependencies":{"com.harken.core":"1.0.0"}})"),
                                    QByteArray(R"({"dependencies":{"alias":"file:com.harken.core"}})")}) {
        writeFile(manifestPath, manifest);
        EXPECT_FALSE(service.run(request).requiresConfirmation);
    }
    ASSERT_TRUE(QFile::remove(manifestPath));
    QJsonObject manifest = integrationManifest();
    manifest.remove("supports_custom_install_path");
    writeFile(QDir(original).filePath("harken-integration.json"), QJsonDocument(manifest).toJson());
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    writeFile(QDir(original).filePath("harken-integration.json"), QJsonDocument(integrationManifest()).toJson());
    const QString duplicate = makeCompleteSdk(QDir(project).filePath("Assets/Duplicate"));
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    ASSERT_TRUE(QDir(duplicate).removeRecursively());
    ASSERT_TRUE(QFile::remove(QDir(original).filePath(".harken-integration-state.json")));
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    EXPECT_TRUE(QDir(original).exists());
    EXPECT_FALSE(QFileInfo::exists(request.migrationBackupPath));
}

TEST_F(UnitySdkMigrationTest, BackupCannotCreateAnotherSdkInsideAReferencedLocalPackage)
{
    const QString localPackage = dir.filePath("LocalPackage");
    writeFile(QDir(localPackage).filePath("package.json"), R"({"name":"com.test.local","version":"1.0.0"})");
    writeFile(QDir(project).filePath("Packages/manifest.json"), R"({"dependencies":{"com.test.local":"file:../../LocalPackage"}})");
    request.migrationBackupPath = QDir(localPackage).filePath("SDKBackup");
    const auto result = service.run(request);
    EXPECT_FALSE(result.requiresConfirmation);
    EXPECT_TRUE(result.steps.back().cause.contains("overlaps a local Unity package"));
    EXPECT_FALSE(QFileInfo::exists(request.migrationBackupPath));
    EXPECT_TRUE(QDir(original).exists());
}

TEST_F(UnitySdkMigrationTest, RequiresReviewedUnchangedPlanClosedUnityAndProjectLock)
{
    request.confirmMigration = true;
    EXPECT_FALSE(service.run(request).success);
    approve();
    writeFile(QDir(original).filePath("User.txt"), "changed after preview");
    EXPECT_FALSE(service.run(request).success);
    EXPECT_FALSE(QFileInfo::exists(request.migrationBackupPath));
    approve();
    HarkenHubTest::UnityProjectLock unityLock;
    ASSERT_TRUE(unityLock.lock(project));
    EXPECT_FALSE(service.run(request).success);
    ASSERT_TRUE(unityLock.unlock());
    QLockFile lock(QDir(project).filePath("Library/HarkenIntegration/integration.lock"));
    ASSERT_TRUE(lock.tryLock());
    EXPECT_FALSE(service.run(request).success);
    lock.unlock();
    request.migrationBackupPath = QDir(project).filePath("Backups/SDK");
    EXPECT_FALSE(service.run(request).success);
    EXPECT_TRUE(QDir(original).exists());
    EXPECT_FALSE(QDir(destination).exists());
}

TEST_F(UnitySdkMigrationTest, RechecksUnityAfterMigrationBackup)
{
    approve();
    HarkenHubTest::UnityProjectLock unityLock;
    QObject::connect(&service, &VNX::UnityIntegrationService::stepUpdated,
        [&](int, const VNX::UnityIntegrationService::StepResult& step) {
            if (step.id == "migrate_sdk" && step.status == VNX::UnityIntegrationService::StepStatus::Running)
                ASSERT_TRUE(unityLock.lock(project));
        });
    const auto result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.integrationMigrated);
    EXPECT_TRUE(result.steps.back().cause.contains("This Unity project is open."));
    EXPECT_TRUE(QDir(original).exists());
    EXPECT_FALSE(QDir(destination).exists());
    EXPECT_TRUE(QFileInfo::exists(request.migrationBackupPath));
    EXPECT_FALSE(VNX::UnityInstallation::hasPendingMigration(project));
}

TEST_F(UnitySdkMigrationTest, RechecksAfterBackupAndRollsBackIfFinalProjectVerificationFails)
{
    writeFile(original + ".meta", "guid: sdk-root");
    approve();
    const auto connection = QObject::connect(&service, &VNX::UnityIntegrationService::stepUpdated,
        [&](int, const VNX::UnityIntegrationService::StepResult& step) {
            if (step.id == "migrate_sdk" && step.status == VNX::UnityIntegrationService::StepStatus::Running)
                writeFile(QDir(original).filePath("LateChange.txt"), "must not move");
        });
    EXPECT_FALSE(service.run(request).success);
    EXPECT_TRUE(QDir(original).exists());
    EXPECT_FALSE(QDir(destination).exists());
    QObject::disconnect(connection);
    request.migrationBackupPath = dir.filePath("SecondBackup");
    approve();
    QObject::connect(&service, &VNX::UnityIntegrationService::stepUpdated,
        [&](int, const VNX::UnityIntegrationService::StepResult& step) {
            if (step.id == "verify_migration" && step.status == VNX::UnityIntegrationService::StepStatus::Running)
                makeCompleteSdk(QDir(project).filePath("Assets/ExternalCopy"));
        });
    EXPECT_FALSE(service.run(request).success);
    EXPECT_TRUE(QDir(original).exists());
    EXPECT_FALSE(QDir(destination).exists());
    EXPECT_EQ(readFile(original + ".meta"), "guid: sdk-root");
    EXPECT_EQ(readFile(QDir(original).filePath("LateChange.txt")), "must not move");
    EXPECT_FALSE(VNX::UnityInstallation::hasPendingMigration(project));
}

TEST_F(UnitySdkMigrationTest, RecoversInterruptedMigrationAtEachRenameBoundaryAndBlocksOtherOperations)
{
    writeFile(QDir(project).filePath("Temp/UnityLockfile"), "stale");
    writeFile(original + ".meta", "guid: sdk-root");
    for (int boundary = 0; boundary < 3; ++boundary) {
        request.migrationBackupPath = dir.filePath(QStringLiteral("Backup%1").arg(boundary));
        approve();
        ASSERT_TRUE(service.run(request).success);
        if (boundary < 2) ASSERT_TRUE(QFile::rename(destination + ".meta", original + ".meta"));
        if (boundary == 0) ASSERT_TRUE(QDir().rename(destination, original));
        writeFile(VNX::UnityInstallation::migrationJournalPath(project),
                  readFile(QDir(request.migrationBackupPath).filePath("migration.json")));
        VNX::UnityIntegrationService::Request other;
        other.unityProjectPath = project;
        other.sdkSourcePath = source;
        other.checkOnly = true;
        EXPECT_FALSE(service.run(other).success);
        other.removeIntegration = true;
        EXPECT_FALSE(service.run(other).success);
        VNX::UnityIntegrationService::Request recovery;
        recovery.unityProjectPath = project;
        recovery.recoverMigration = true;
        HarkenHubTest::UnityProjectLock unityLock;
        ASSERT_TRUE(unityLock.lock(project));
        EXPECT_FALSE(service.run(recovery).requiresConfirmation);
        ASSERT_TRUE(unityLock.unlock());
        const auto preview = service.run(recovery);
        ASSERT_TRUE(preview.requiresConfirmation) << preview.steps.back().cause.toStdString();
        recovery.confirmMigration = true;
        recovery.migrationPlanHash = preview.migrationPlanHash;
        const auto result = service.run(recovery);
        ASSERT_TRUE(result.success) << result.steps.back().cause.toStdString();
        EXPECT_TRUE(result.migrationRecovered);
        EXPECT_EQ(result.sdkInstallPath, "Packages/com.harken.core");
        EXPECT_EQ(readFile(original + ".meta"), "guid: sdk-root");
        EXPECT_FALSE(QDir(destination).exists());
        EXPECT_FALSE(VNX::UnityInstallation::hasPendingMigration(project));
    }
}

TEST_F(UnitySdkMigrationTest, RecoversIntoRecreatedEmptySourceButPreservesFilesAndMetadataConflicts)
{
    writeFile(original + ".meta", "guid: sdk-root");
    writeFile(QDir(original).filePath("UserChange.txt"), "keep modified SDK");
    approve();
    ASSERT_TRUE(service.run(request).success);
    writeFile(VNX::UnityInstallation::migrationJournalPath(project),
              readFile(QDir(request.migrationBackupPath).filePath("migration.json")));
    ASSERT_TRUE(QDir().mkpath(original));
    request.recoverMigration = true;
    request.confirmMigration = false;
    const QString conflict = QDir(original).filePath(".user-file");
    writeFile(conflict, "keep source content");
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    EXPECT_EQ(readFile(conflict), "keep source content");
    ASSERT_TRUE(QFile::remove(conflict));
    writeFile(original + ".meta", "guid: recreated-folder");
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    EXPECT_EQ(readFile(original + ".meta"), "guid: recreated-folder");
    ASSERT_TRUE(QFile::remove(original + ".meta"));

    const auto preview = service.run(request);
    ASSERT_TRUE(preview.requiresConfirmation) << preview.steps.back().cause.toStdString();
    EXPECT_TRUE(QDir(original).exists());
    EXPECT_TRUE(QDir(original).isEmpty());
    EXPECT_TRUE(QDir(destination).exists());
    request.confirmMigration = true;
    request.migrationPlanHash = preview.migrationPlanHash;
    writeFile(conflict, "changed after preview");
    EXPECT_FALSE(service.run(request).success);
    EXPECT_EQ(readFile(conflict), "changed after preview");
    ASSERT_TRUE(QFile::remove(conflict));
    const auto recovered = service.run(request);
    ASSERT_TRUE(recovered.success) << recovered.steps.back().cause.toStdString();
    EXPECT_EQ(readFile(QDir(original).filePath("UserChange.txt")), "keep modified SDK");
    EXPECT_EQ(readFile(original + ".meta"), "guid: sdk-root");
    EXPECT_FALSE(QDir(destination).exists());
    EXPECT_FALSE(VNX::UnityInstallation::hasPendingMigration(project));
}

TEST_F(UnitySdkMigrationTest, RejectsLinkedSourceDestinationAndBackup)
{
#ifdef Q_OS_WIN
    const QString external = dir.filePath("External");
    ASSERT_TRUE(QDir().mkpath(external));
    const QString link = QDir(project).filePath("Assets/Linked");
    ASSERT_EQ(QProcess::execute(QStringLiteral("cmd.exe"),
        {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
         QDir::toNativeSeparators(link), QDir::toNativeSeparators(external)}), 0);
    const auto removeLink = qScopeGuard([&]() { QDir().rmdir(link); });
    request.migrationTargetPath = QStringLiteral("Assets/Linked/SDK");
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    ASSERT_TRUE(QDir().rmdir(link));
    request.migrationTargetPath = QStringLiteral("Assets/ThirdParty/HarkenSDK");
    const QString backupLink = dir.filePath("LinkedBackup");
    ASSERT_EQ(QProcess::execute(QStringLiteral("cmd.exe"),
        {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
         QDir::toNativeSeparators(backupLink), QDir::toNativeSeparators(external)}), 0);
    const auto removeBackupLink = qScopeGuard([&]() { QDir().rmdir(backupLink); });
    request.migrationBackupPath = QDir(backupLink).filePath("Snapshot");
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    request.migrationBackupPath = dir.filePath("Backup");
    const QString sourceLink = QDir(original).filePath("LinkedContent");
    ASSERT_EQ(QProcess::execute(QStringLiteral("cmd.exe"),
        {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
         QDir::toNativeSeparators(sourceLink), QDir::toNativeSeparators(external)}), 0);
    const auto removeSourceLink = qScopeGuard([&]() { QDir().rmdir(sourceLink); });
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    EXPECT_TRUE(QDir(external).isEmpty());
#else
    GTEST_SKIP() << "Windows directory junction coverage";
#endif
}

TEST_F(UnitySdkMigrationTest, RecoveryDoesNotOverwriteChangesOrTrustEscapingJournalPaths)
{
    approve();
    ASSERT_TRUE(service.run(request).success);
    const QString journal = VNX::UnityInstallation::migrationJournalPath(project);
    QJsonObject record = QJsonDocument::fromJson(readFile(QDir(request.migrationBackupPath).filePath("migration.json"))).object();
    writeFile(journal, QJsonDocument(record).toJson());
    writeFile(QDir(destination).filePath("UserChange.txt"), "keep after interruption");
    request.recoverMigration = true;
    request.confirmMigration = false;
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    EXPECT_EQ(readFile(QDir(destination).filePath("UserChange.txt")), "keep after interruption");
    ASSERT_TRUE(QFile::remove(QDir(destination).filePath("UserChange.txt")));
    record["source"] = "../Outside";
    writeFile(journal, QJsonDocument(record).toJson());
    EXPECT_FALSE(service.run(request).requiresConfirmation);
    EXPECT_TRUE(QDir(destination).exists());
    EXPECT_TRUE(QFileInfo::exists(journal));
}

TEST(UnityIntegrationService, CustomLocationSupportsCheckInstallUpdateBackupAndRemoval)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/custom-sdk-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
    const QString source = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
    const QString relative = QStringLiteral("Assets/ThirdParty/Audio/HarkenSDK");
    const QString target = QDir(project).filePath(relative);
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    request.sdkInstallPath = target;
    request.checkOnly = true;
    auto result = service.run(request);
    ASSERT_TRUE(result.success) << result.steps.front().cause.toStdString();
    EXPECT_FALSE(QDir(target).exists());
    EXPECT_EQ(result.sdkInstallPath, relative);
    request.checkOnly = false;
    result = service.run(request);
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.targetCurrent);
    EXPECT_EQ(VNX::UnityInstallation::discover(project).installations.size(), 1);
    EXPECT_FALSE(QDir(QDir(project).filePath(QStringLiteral("Packages/com.harken.core"))).exists());
    writeFile(QDir(target).filePath(QStringLiteral("UserContent.txt")), "keep");
    writeFile(QDir(source).filePath(QStringLiteral("Runtime/Generated/HarkenAbi.g.cs")), "updated");
    EXPECT_TRUE(service.run(request).requiresConfirmation);
    request.confirmTargetChanges = true;
    result = service.run(request);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(readFile(QDir(target).filePath(QStringLiteral("Runtime/Generated/HarkenAbi.g.cs"))), "updated");
    EXPECT_EQ(readFile(QDir(target).filePath(QStringLiteral("UserContent.txt"))), "keep");
    EXPECT_TRUE(readFile(result.jsonReportPath).contains(relative.toUtf8()));
    request.removeIntegration = true;
    request.removalBackupPath = dir.filePath(QStringLiteral("Backup"));
    EXPECT_TRUE(service.run(request).requiresConfirmation);
    request.confirmRemoval = true;
    result = service.run(request);
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.integrationRemoved);
    EXPECT_EQ(readFile(QDir(target).filePath(QStringLiteral("UserContent.txt"))), "keep");
    EXPECT_TRUE(QFileInfo::exists(QDir(request.removalBackupPath).filePath(
        QStringLiteral("UnityProject/") + relative + QStringLiteral("/.harken-integration-state.json"))));
    EXPECT_TRUE(VNX::UnityInstallation::discover(project).installations.isEmpty());
}

TEST(UnityIntegrationService, RejectsUnsafeCustomLocationsAndNonemptyDirectories)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-paths-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
    const QString source = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    const QStringList invalid = {
        QStringLiteral("../Outside"), QStringLiteral("Assets/../Other"),
        QStringLiteral("Assets"), QStringLiteral("Packages"), QStringLiteral("ProjectSettings/SDK"),
        QStringLiteral("Packages/Other"), QStringLiteral("Assets/Editor/SDK"),
        QStringLiteral("Assets/Resources/SDK"), QStringLiteral("Assets/StreamingAssets/SDK"),
        QStringLiteral("Assets/Hidden~/SDK"), QStringLiteral("Assets/.Hidden/SDK"),
        QStringLiteral("Assets/SDK:stream"), QStringLiteral("Assets/NUL"),
        dir.filePath(QStringLiteral("External")),
    };
    for (const QString& location : invalid) {
        SCOPED_TRACE(location.toStdString());
        request.sdkInstallPath = location;
        EXPECT_FALSE(service.run(request).success);
    }
    request.sdkInstallPath = QStringLiteral("Assets/ThirdParty");
    const QString userFile = QDir(project).filePath(QStringLiteral("Assets/ThirdParty/User.txt"));
    writeFile(userFile, "keep");
    const auto result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.steps.front().cause.contains(QStringLiteral("unrelated files")));
    EXPECT_EQ(readFile(userFile), "keep");
}

TEST(UnityIntegrationService, DiscoversMovedSdkAndBlocksSecondOrNestedInstallation)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-discovery-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
    const QString source = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    ASSERT_TRUE(service.run(request).success);
    const QString original = QDir(project).filePath(QStringLiteral("Packages/com.harken.core"));
    const QString moved = QDir(project).filePath(QStringLiteral("Assets/RenamedAudio"));
    ASSERT_TRUE(QDir().rename(original, moved));
    auto scan = VNX::UnityInstallation::discover(project);
    ASSERT_TRUE(scan.errors.isEmpty());
    ASSERT_EQ(scan.installations.size(), 1);
    EXPECT_EQ(scan.installations.front().path, moved);
    EXPECT_TRUE(scan.installations.front().managed);
    EXPECT_FALSE(service.run(request).success);
    EXPECT_FALSE(QDir(original).exists());
    request.sdkInstallPath = QStringLiteral("Assets/RenamedAudio");
    const auto relocated = service.run(request);
    QStringList diagnostics;
    for (const auto& step : relocated.steps)
        diagnostics << step.id + ": " + step.summary + "\n" + step.cause;
    EXPECT_TRUE(relocated.success) << diagnostics.join('\n').toStdString();
    makeCompleteSdk(QDir(moved).filePath(QStringLiteral("Copy")));
    const auto result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.steps.front().cause.contains(QStringLiteral("Multiple SDK")));
}

TEST(UnityIntegrationService, BlocksSdkPackageReferencesAndPartialCopiesButIgnoresCaches)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-references-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
    const QString source = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
    makeCompleteSdk(QDir(project).filePath(QStringLiteral("Library/PackageCache/com.harken.core@unused")));
    makeCompleteSdk(QDir(project).filePath(QStringLiteral("Backups/SDK")));
    EXPECT_TRUE(VNX::UnityInstallation::discover(project).installations.isEmpty());
    const QString manifest = QDir(project).filePath(QStringLiteral("Packages/manifest.json"));
    writeFile(manifest, R"({"dependencies":{"com.harken.core":"1.0.0"}})");
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    EXPECT_FALSE(service.run(request).success);
    writeFile(manifest, R"({"dependencies":{"com.harken.core":"file:../../SDK"}})");
    const auto referenced = service.run(request);
    EXPECT_FALSE(referenced.success);
    EXPECT_TRUE(referenced.steps.front().cause.contains(source));
    ASSERT_TRUE(QFile::remove(manifest));
    writeFile(QDir(project).filePath(QStringLiteral("Assets/Plugins/libharken_core.so")), "partial");
    const auto partial = service.run(request);
    EXPECT_FALSE(partial.success);
    EXPECT_TRUE(partial.steps.front().cause.contains(QStringLiteral("without an identifiable")));
}

TEST(UnityIntegrationService, LargeProjectsStillDiscoverConflictsAndAllowKnownTargetChecks)
{
    QTemporaryDir dir(QDir::current().filePath(".temp/sdk-discovery-large-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath("Game"));
    const QString installed = makeCompleteSdk(QDir(project).filePath("Packages/com.harken.core"));
    const QString assets = QDir(project).filePath("Assets/LargeContent");
    ASSERT_TRUE(QDir().mkpath(assets));
    for (int index = 0; index < 100010; ++index) {
        QFile file(QDir(assets).filePath(QString::number(index) + ".asset"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
    auto discovery = VNX::UnityInstallation::discover(project);
    EXPECT_TRUE(discovery.errors.isEmpty()) << discovery.errors.join('\n').toStdString();
    ASSERT_EQ(discovery.installations.size(), 1);
    QString error;
    EXPECT_TRUE(VNX::UnityInstallation::validateTarget(project, installed, &error)) << error.toStdString();
    makeCompleteSdk(QDir(assets).filePath("AnotherSDK"));
    EXPECT_FALSE(VNX::UnityInstallation::validateTarget(project, installed, &error));
    EXPECT_TRUE(error.contains("Multiple SDK installations")) << error.toStdString();
    const QString external = dir.filePath("External");
    ASSERT_TRUE(QDir().rename(assets, external));
    writeFile(external + "/package.json", R"({"name":"com.example.external"})");
    writeFile(QDir(project).filePath("Packages/manifest.json"),
              R"({"dependencies":{"com.example.external":"file:../../External"}})");
    discovery = VNX::UnityInstallation::discover(project);
    EXPECT_TRUE(discovery.errors.join('\n').contains("External package discovery exceeded"));
    writeFile(QDir(project).filePath("Packages/manifest.json"),
              R"({"dependencies":{"com.example.external":"file:../../"}})");
    EXPECT_FALSE(VNX::UnityInstallation::validateTarget(project, installed, &error));
}

TEST(UnityIntegrationService, FailedPostInstallVerificationRestoresOldSdkAndProjectFiles)
{
    QTemporaryDir dir(QDir::current().filePath(".temp/sdk-update-rollback-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = makeUnityProject(dir.filePath("Game"));
    request.sdkSourcePath = makeCompleteSdk(dir.filePath("Source"));
    ASSERT_TRUE(service.run(request).success);
    const QString installed = QDir(request.unityProjectPath).filePath("Packages/com.harken.core");
    const QString runtime = hostRuntimeRelativePath();
    const QByteArray original = readFile(QDir(installed).filePath(runtime));
    const QByteArray receipt = readFile(installed + "/.harken-integration-state.json");
    writeFile(QDir(installed).filePath(runtime + ".meta"), "guid: keep-plugin-guid");
    writeFile(installed + "/Custom.txt", "project-specific");
    writeFile(QDir(request.sdkSourcePath).filePath(runtime), "new-runtime");
    bool injected = false;
    QObject::connect(&service, &VNX::UnityIntegrationService::stepUpdated,
        [&](int, const VNX::UnityIntegrationService::StepResult& step) {
            if (step.id == "verify" && step.status == VNX::UnityIntegrationService::StepStatus::Running) {
                writeFile(QDir(installed).filePath(runtime), "damaged-after-activation");
                injected = true;
            }
        });
    request.confirmTargetChanges = true;
    const auto result = service.run(request);
    EXPECT_TRUE(injected);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.targetCurrent);
    EXPECT_EQ(readFile(QDir(installed).filePath(runtime)), original);
    EXPECT_EQ(readFile(installed + "/.harken-integration-state.json"), receipt);
    EXPECT_EQ(readFile(QDir(installed).filePath(runtime + ".meta")), "guid: keep-plugin-guid");
    EXPECT_EQ(readFile(installed + "/Custom.txt"), "project-specific");
    EXPECT_TRUE(result.steps[4].remediation.contains("original installation state was restored"));
    ASSERT_FALSE(result.steps[4].artifacts.isEmpty());
    EXPECT_TRUE(QDir(result.steps[4].artifacts.front()).exists());
}

TEST(UnityIntegrationService, FailedNewInstallationDoesNotLeaveAnActiveUnverifiedSdk)
{
    QTemporaryDir dir(QDir::current().filePath(".temp/sdk-install-rollback-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = makeUnityProject(dir.filePath("Game"));
    request.sdkSourcePath = makeCompleteSdk(dir.filePath("Source"));
    const QString installed = QDir(request.unityProjectPath).filePath("Packages/com.harken.core");
    QObject::connect(&service, &VNX::UnityIntegrationService::stepUpdated,
        [&](int, const VNX::UnityIntegrationService::StepResult& step) {
            if (step.id == "verify" && step.status == VNX::UnityIntegrationService::StepStatus::Running)
                writeFile(QDir(installed).filePath(hostRuntimeRelativePath()), "damaged");
        });
    const auto result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.targetInstalled);
    EXPECT_FALSE(QDir(installed).exists());
    EXPECT_TRUE(VNX::UnityInstallation::discover(request.unityProjectPath).installations.isEmpty());
}

TEST(UnityIntegrationService, BoundsLocalPackageDiscoveryAndRejectsProjectAncestors)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-discovery-limits-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath("Game"));
    const QString local = dir.filePath("Local");
    QString nested = local;
    writeFile(QDir(local).filePath("package.json"), R"({"name":"unrelated"})");
    for (int depth = 0; depth < 66; ++depth) nested += "/d";
    writeFile(QDir(nested).filePath("marker.txt"), "deep content");
    const QString manifest = QDir(project).filePath("Packages/manifest.json");
    writeFile(manifest, R"({"dependencies":{"unrelated":"file:../../Local"}})");
    auto discovery = VNX::UnityInstallation::discover(project);
    EXPECT_TRUE(discovery.errors.join('\n').contains("safety limits"));
    EXPECT_TRUE(discovery.localPackagePaths.contains(local));

    writeFile(manifest, R"({"dependencies":{"unrelated":"file:../.."}})");
    discovery = VNX::UnityInstallation::discover(project);
    EXPECT_TRUE(discovery.errors.join('\n').contains("Reference a package directory"));
    EXPECT_TRUE(VNX::UnityInstallation::containsPath(QDir::rootPath(),
        QDir(QDir::rootPath()).filePath("Game/Assets")));
    writeFile(manifest, R"({"dependencies":{"unrelated":"file:/"}})");
    discovery = VNX::UnityInstallation::discover(project);
    EXPECT_TRUE(discovery.errors.join('\n').contains("Reference a package directory"));
}

TEST(UnityIntegrationService, BoundsIdentityReadsWithoutAcceptingIncompleteDiscovery)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-discovery-json-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath("Game"));
    writeFile(QDir(project).filePath("Assets/Other/package.json"),
              QByteArray(1024 * 1024, ' ') + "{}");
    const auto discovery = VNX::UnityInstallation::discover(project);
    EXPECT_TRUE(discovery.errors.join('\n').contains("safety limits"));
    QString error;
    EXPECT_FALSE(VNX::UnityInstallation::validateTarget(project,
        QDir(project).filePath("Packages/com.harken.core"), &error));
}

TEST(UnityIntegrationService, DetectsSdkInsideAliasedCyclicAndOverlappingLocalPackages)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-discovery-alias-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath("Game"));
    const QString local = dir.filePath("Local");
    const QString sdk = makeCompleteSdk(QDir(local).filePath("SDK"));
    writeFile(QDir(local).filePath("package.json"),
              R"({"name":"unrelated","dependencies":{"self":"file:.","nested":"file:SDK"}})");
    writeFile(QDir(project).filePath("Packages/manifest.json"),
              R"({"dependencies":{"alias":"file:../../Local","nested":"file:../../Local/SDK"}})");
    const auto discovery = VNX::UnityInstallation::discover(project);
    EXPECT_TRUE(discovery.errors.isEmpty()) << discovery.errors.join('\n').toStdString();
    ASSERT_EQ(discovery.installations.size(), 1);
    EXPECT_TRUE(VNX::UnityInstallation::samePath(discovery.installations.front().path, sdk));
    EXPECT_TRUE(discovery.localPackagePaths.contains(local));
    EXPECT_TRUE(discovery.localPackagePaths.contains(sdk));
    QString error;
    EXPECT_FALSE(VNX::UnityInstallation::validateTarget(project,
        QDir(project).filePath("Packages/com.harken.core"), &error));
}

TEST(UnityIntegrationService, UnrelatedBrokenIdentitiesDoNotBlockButSdkRemnantsAndUpmManifestsDo)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-discovery-json-errors-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath("Game"));
    const QString source = makeCompleteSdk(dir.filePath("SDK"));
    const QString manifest = QDir(project).filePath("Packages/manifest.json");
    writeFile(manifest, R"({"dependencies":{"unrelated":"file:../../Local"}})");
    writeFile(dir.filePath("Local/package.json"), "broken local package");
    writeFile(QDir(project).filePath("Assets/Other/package.json"), "broken asset package");
    writeFile(QDir(project).filePath("Assets/Other/Game.asmdef"), "broken game assembly");
    writeFile(QDir(project).filePath("Packages/Other/package.json"), "broken embedded package");
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    const auto installed = service.run(request);
    ASSERT_TRUE(installed.success) << installed.steps.front().cause.toStdString();
    const QString remnant = QDir(project).filePath("Assets/Partial/Harken.Core.asmdef");
    writeFile(remnant, "broken SDK assembly");
    EXPECT_FALSE(VNX::UnityInstallation::discover(project).errors.isEmpty());
    ASSERT_TRUE(QFile::remove(remnant));
    writeFile(manifest, "broken UPM manifest");
    EXPECT_FALSE(VNX::UnityInstallation::discover(project).errors.isEmpty());
    ASSERT_TRUE(QFile::remove(manifest));
    writeFile(QDir(project).filePath("Packages/packages-lock.json"), "broken UPM lock");
    EXPECT_FALSE(VNX::UnityInstallation::discover(project).errors.isEmpty());
}

TEST(UnityIntegrationService, DamagedStandardPackageIdentitiesBlockAnotherInstallation)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-standard-identity-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath("Game"));
    const QString source = makeCompleteSdk(dir.filePath("SDK"));
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    request.sdkInstallPath = QStringLiteral("Assets/ThirdParty/NewSDK");
    const QString target = QDir(project).filePath(request.sdkInstallPath);
    for (const QString& location : {QStringLiteral("Packages/com.harken.core"),
                                    QStringLiteral("Assets/HarkenSDK")}) {
        SCOPED_TRACE(location.toStdString());
        const QString manifest = QDir(project).filePath(location + "/package.json");
        for (const QByteArray& contents : {QByteArray("broken JSON"), QByteArray("[]"),
                                           QByteArray("{}"), QByteArray(R"({"name":42})")}) {
            SCOPED_TRACE(contents.toStdString());
            writeFile(manifest, contents);
            const auto discovery = VNX::UnityInstallation::discover(project);
            EXPECT_TRUE(discovery.errors.join('\n').contains(manifest));
            const auto result = service.run(request);
            ASSERT_FALSE(result.success);
            EXPECT_FALSE(QDir(target).exists());
            EXPECT_EQ(readFile(manifest), contents);
            ASSERT_TRUE(QFile::remove(manifest));
        }
        ASSERT_TRUE(QDir().mkpath(manifest));
        EXPECT_FALSE(VNX::UnityInstallation::discover(project).errors.isEmpty());
        EXPECT_FALSE(service.run(request).success);
        EXPECT_FALSE(QDir(target).exists());
        ASSERT_TRUE(QDir().rmdir(manifest));
        writeFile(manifest, R"({"name":"com.example.unrelated"})");
        EXPECT_TRUE(VNX::UnityInstallation::discover(project).errors.isEmpty());
    }
    const auto installed = service.run(request);
    EXPECT_TRUE(installed.success) << installed.steps.front().cause.toStdString();
}

TEST(UnityIntegrationService, CanRepairManagedSdkWithDamagedPackageIdentity)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-managed-identity-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath("Game"));
    const QString source = makeCompleteSdk(dir.filePath("SDK"));
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    ASSERT_TRUE(service.run(request).success);
    const QString manifest = QDir(project).filePath("Packages/com.harken.core/package.json");
    writeFile(manifest, "broken SDK package");
    const auto discovery = VNX::UnityInstallation::discover(project);
    ASSERT_EQ(discovery.installations.size(), 1);
    EXPECT_TRUE(discovery.installations.front().managed);
    EXPECT_TRUE(discovery.errors.isEmpty());
    request.sdkInstallPath = QStringLiteral("Assets/OtherSDK");
    EXPECT_FALSE(service.run(request).success);
    EXPECT_FALSE(QDir(QDir(project).filePath(request.sdkInstallPath)).exists());
    request.sdkInstallPath.clear();
    EXPECT_TRUE(service.run(request).requiresConfirmation);
    request.confirmTargetChanges = true;
    const auto repaired = service.run(request);
    EXPECT_TRUE(repaired.success) << repaired.steps.front().cause.toStdString();
    EXPECT_EQ(readFile(manifest), readFile(QDir(source).filePath("package.json")));
}

TEST(UnityIntegrationService, UsesConsistentPlatformPathCaseAndContainmentRules)
{
    const QString path = QDir::current().filePath(".temp/Game/Assets/SDK");
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    EXPECT_TRUE(VNX::UnityInstallation::samePath(path, path.toUpper()));
    EXPECT_TRUE(VNX::UnityInstallation::containsPath(path, path.toLower() + "/Runtime"));
#else
    EXPECT_FALSE(VNX::UnityInstallation::samePath(path, path.toUpper()));
    EXPECT_FALSE(VNX::UnityInstallation::containsPath(path, path.toUpper() + "/Runtime"));
#endif
    EXPECT_FALSE(VNX::UnityInstallation::containsPath(path, path + "Copy/Runtime"));
}

TEST(UnityIntegrationService, RejectsManifestTraversalAndOverlappingSource)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-safety-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
    const QString source = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    auto manifest = integrationManifest();
    manifest[QStringLiteral("required_files")] = QJsonArray{QStringLiteral("../outside.txt")};
    writeFile(QDir(source).filePath(QStringLiteral("harken-integration.json")), QJsonDocument(manifest).toJson());
    auto result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.steps[1].summary.contains(QStringLiteral("unsafe file path")));
    makeCompleteSdk(source);
    ASSERT_TRUE(service.run(request).success);
    request.sdkSourcePath = QDir(project).filePath(QStringLiteral("Packages/com.harken.core"));
    result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.steps[1].cause.contains(QStringLiteral("containment")));
    request.sdkSourcePath = source;
    const QString statePath = QDir(project).filePath(QStringLiteral("Packages/com.harken.core/.harken-integration-state.json"));
    auto state = QJsonDocument::fromJson(readFile(statePath)).object();
    state[QStringLiteral("managed_files")] = QJsonArray{QStringLiteral("../outside.txt")};
    writeFile(statePath, QJsonDocument(state).toJson());
    request.removeIntegration = true;
    request.confirmRemoval = true;
    EXPECT_FALSE(service.run(request).integrationRemoved);
}

TEST(UnityIntegrationService, ChecksCapabilitiesAndSerializesProjectOperations)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-lock-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
    const QString source = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
    auto manifest = integrationManifest();
    manifest.remove(QStringLiteral("supports_custom_install_path"));
    writeFile(QDir(source).filePath(QStringLiteral("harken-integration.json")), QJsonDocument(manifest).toJson());
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    request.sdkInstallPath = QStringLiteral("Assets/ThirdParty/SDK");
    auto result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.steps[1].summary.contains(QStringLiteral("does not support")));
    request.sdkInstallPath.clear();
    const QString lockPath = QDir(project).filePath(QStringLiteral("Library/HarkenIntegration/integration.lock"));
    QLockFile lock(lockPath);
    ASSERT_TRUE(lock.tryLock());
    result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.steps.front().cause.contains(QStringLiteral("Another integration")));
    lock.unlock();
    HarkenHubTest::UnityProjectLock unityLock;
    ASSERT_TRUE(unityLock.lock(project));
    EXPECT_FALSE(service.run(request).success);
    request.checkOnly = true;
    EXPECT_TRUE(service.run(request).success);
}

TEST(HubUnityIntegrationService, BlocksAllSdkChangesWhileUnityIsOpenBeforeInspectingTheSdk)
{
    QTemporaryDir dir(QDir::current().filePath(".temp/sdk-open-unity-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath("Game"));
    const QString source = makeCompleteSdk(dir.filePath("SDK"));
    VNH::HubUnityIntegrationService service;
    VNH::UnityIntegrationRequest request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    writeFile(QDir(project).filePath("Temp/UnityLockfile"), "stale");
    ASSERT_TRUE(service.run(request).success);
    const QString installed = QDir(project).filePath("Packages/com.harken.core");
    const QByteArray package = readFile(installed + "/package.json");
    writeFile(installed + ".meta", "guid: unchanged-sdk");
    HarkenHubTest::UnityProjectLock unityLock;
    ASSERT_TRUE(unityLock.lock(project));
    request.sdkSourcePath = dir.filePath("missing-sdk");
    for (const bool confirmed : {false, true}) {
        for (const int operation : {0, 1, 2, 3}) {
            auto change = request;
            change.checkOnly = operation == 0 && !confirmed;
            change.confirmTargetChanges = confirmed;
            change.removeIntegration = operation == 1;
            change.confirmRemoval = confirmed;
            change.removalBackupPath = dir.filePath("RemovalBackup");
            change.migrateIntegration = operation == 2;
            change.recoverMigration = operation == 3;
            change.confirmMigration = confirmed;
            change.migrationTargetPath = "Assets/MovedSDK";
            change.migrationBackupPath = dir.filePath("MigrationBackup");
            const auto result = service.run(change);
            EXPECT_FALSE(result.success);
            EXPECT_FALSE(result.requiresConfirmation);
            EXPECT_EQ(result.sdkFilesCopied, 0);
            ASSERT_FALSE(result.steps.isEmpty());
            EXPECT_TRUE((result.steps.front().summary + result.steps.front().cause)
                            .contains("This Unity project is open."));
            EXPECT_EQ(readFile(installed + "/package.json"), package);
            EXPECT_EQ(readFile(installed + ".meta"), "guid: unchanged-sdk");
            EXPECT_FALSE(QFileInfo::exists(change.removalBackupPath));
            EXPECT_FALSE(QFileInfo::exists(change.migrationBackupPath));
            EXPECT_FALSE(QFileInfo::exists(QDir(project).filePath(change.migrationTargetPath)));
        }
    }
    request.verifyInstalledOnly = true;
    EXPECT_TRUE(service.run(request).success);
    request.verifyInstalledOnly = false;
    request.checkOnly = true;
    request.sdkSourcePath = source;
    ASSERT_TRUE(unityLock.unlock());
    const QString otherProject = makeUnityProject(dir.filePath("OtherGame"));
    ASSERT_TRUE(unityLock.lock(otherProject));
    EXPECT_TRUE(service.run(request).success);
    EXPECT_EQ(readFile(QDir(project).filePath("Temp/UnityLockfile")), "stale");
}

TEST(HubUnityIntegrationService, RechecksUnityAfterSafetyCheckBeforeInstallingUpdatingOrRemovingFiles)
{
    for (const int operation : {0, 1, 2}) {
        QTemporaryDir dir(QDir::current().filePath(".temp/sdk-unity-recheck-XXXXXX"));
        ASSERT_TRUE(dir.isValid());
        const QString project = makeUnityProject(dir.filePath("Game"));
        const QString source = makeCompleteSdk(dir.filePath("SDK"));
        VNH::HubUnityIntegrationService service;
        VNH::UnityIntegrationRequest request;
        request.unityProjectPath = project;
        request.sdkSourcePath = source;
        const QString installed = QDir(project).filePath("Packages/com.harken.core");
        QByteArray originalPackage;
        if (operation != 0) {
            ASSERT_TRUE(service.run(request).success);
            originalPackage = readFile(installed + "/package.json");
            writeFile(installed + ".meta", "guid: unchanged-sdk");
            writeFile(QDir(source).filePath("package.json"),
                      R"({"name":"com.harken.core","version":"2.0.0"})");
        }
        request.checkOnly = operation != 2;
        request.removeIntegration = operation == 2;
        request.removalBackupPath = dir.filePath("Backup");
        const auto preview = service.run(request);
        EXPECT_TRUE(preview.success || preview.requiresConfirmation);
        HarkenHubTest::UnityProjectLock unityLock;
        ASSERT_TRUE(unityLock.lock(project));
        request.checkOnly = false;
        request.confirmTargetChanges = true;
        request.confirmRemoval = true;
        const auto result = service.run(request);
        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.integrationRemoved);
        EXPECT_EQ(result.sdkFilesCopied, 0);
        ASSERT_FALSE(result.steps.isEmpty());
        EXPECT_TRUE(result.steps.front().summary.contains("This Unity project is open."));
        EXPECT_FALSE(QFileInfo::exists(request.removalBackupPath));
        if (operation == 0) {
            EXPECT_FALSE(QDir(installed).exists());
        } else {
            EXPECT_EQ(readFile(installed + "/package.json"), originalPackage);
            EXPECT_EQ(readFile(installed + ".meta"), "guid: unchanged-sdk");
        }
        ASSERT_TRUE(unityLock.unlock());
        EXPECT_TRUE(service.run(request).success);
        EXPECT_TRUE(QFileInfo::exists(QDir(project).filePath("Temp/UnityLockfile")));
    }
}

TEST(UnityProjectAccess, DistinguishesStaleFilesFromLiveLocksWithoutChangingThem)
{
    QTemporaryDir dir(QDir::current().filePath(".temp/unity-project-lock-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    const QString project = dir.filePath("Game %2 with spaces");
    const QString path = QDir(project).filePath("Temp/UnityLockfile");
    QString error;
    EXPECT_TRUE(VNX::checkUnityProjectClosed(project, &error));
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
        EXPECT_FALSE(VNX::checkUnityProjectClosed(project, &error));
        EXPECT_TRUE(error.contains("This Unity project is open."));
        EXPECT_FALSE(VNX::checkUnityProjectClosed(project, &error));
        ASSERT_TRUE(unityLock.unlock());
        EXPECT_TRUE(VNX::checkUnityProjectClosed(project, &error));
        EXPECT_TRUE(error.isEmpty());
    }
    EXPECT_EQ(readFile(path), "stale lock contents");
    EXPECT_EQ(QFileInfo(path).lastModified(), modified);
    ASSERT_TRUE(QFile::remove(path));
    ASSERT_TRUE(QDir().mkpath(path));
    EXPECT_FALSE(VNX::checkUnityProjectClosed(project, &error));
    EXPECT_TRUE(error.startsWith("Cannot check whether this Unity project is open:"));
    EXPECT_TRUE(error.contains(QDir::toNativeSeparators(path)));
    VNH::UnityIntegrationRequest request;
    request.unityProjectPath = project;
    request.removeIntegration = true;
    const auto result = VNH::HubUnityIntegrationService::run(request);
    ASSERT_EQ(result.steps.size(), 1);
    EXPECT_EQ(result.steps.front().summary, error);
}

TEST(UnityIntegrationService, RechecksForDuplicatesBeforeWriting)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-recheck-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
    const QString source = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
    VNX::UnityIntegrationService service;
    QObject::connect(&service, &VNX::UnityIntegrationService::stepUpdated,
                     [&](int, const VNX::UnityIntegrationService::StepResult& step) {
        if (step.id == QStringLiteral("install_sdk")
            && step.status == VNX::UnityIntegrationService::StepStatus::Running)
            makeCompleteSdk(QDir(project).filePath(QStringLiteral("Assets/ExternalCopy")));
    });
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    const auto result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.sdkFilesCopied, 0);
    EXPECT_FALSE(QDir(QDir(project).filePath(QStringLiteral("Packages/com.harken.core"))).exists());
}

TEST(UnityIntegrationService, RemovalDoesNotReactivateAnOverriddenPackageDependency)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-dependency-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
    ASSERT_TRUE(service.run(request).success);
    writeFile(QDir(project).filePath(QStringLiteral("Packages/manifest.json")),
              R"({"dependencies":{"com.harken.core":"1.0.0"}})");
    request.checkOnly = true;
    EXPECT_TRUE(service.run(request).success);
    request.removeIntegration = true;
    request.confirmRemoval = true;
    const auto result = service.run(request);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.integrationRemoved);
    EXPECT_TRUE(result.steps.front().cause.contains(QStringLiteral("package dependencies")));
    EXPECT_TRUE(QFileInfo::exists(QDir(project).filePath(QStringLiteral("Packages/com.harken.core/package.json"))));
}

#ifdef Q_OS_WIN
TEST(UnityIntegrationService, RejectsJunctionsInTargetsSourcesAndBackupParents)
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral(".temp/sdk-junction-XXXXXX")));
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath(QStringLiteral("Game")));
    const QString source = makeCompleteSdk(dir.filePath(QStringLiteral("SDK")));
    const QString external = dir.filePath(QStringLiteral("External"));
    ASSERT_TRUE(QDir().mkpath(external));
    const QString link = QDir(project).filePath(QStringLiteral("Assets/Linked"));
    ASSERT_EQ(QProcess::execute(QStringLiteral("cmd.exe"),
        {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
         QDir::toNativeSeparators(link), QDir::toNativeSeparators(external)}), 0);
    const auto removeLink = qScopeGuard([&]() { QDir().rmdir(link); });
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    request.sdkInstallPath = QStringLiteral("Assets/Linked/SDK");
    EXPECT_FALSE(service.run(request).success);
    EXPECT_FALSE(QDir(QDir(external).filePath(QStringLiteral("SDK"))).exists());
    request.sdkInstallPath.clear();
    EXPECT_FALSE(service.run(request).success);
    ASSERT_TRUE(QDir().rmdir(link));
    const QString sourceLink = QDir(source).filePath(QStringLiteral("Linked"));
    ASSERT_EQ(QProcess::execute(QStringLiteral("cmd.exe"),
        {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
         QDir::toNativeSeparators(sourceLink), QDir::toNativeSeparators(external)}), 0);
    const auto removeSourceLink = qScopeGuard([&]() { QDir().rmdir(sourceLink); });
    EXPECT_FALSE(service.run(request).success);
    ASSERT_TRUE(QDir().rmdir(sourceLink));
    ASSERT_TRUE(service.run(request).success);
    const QString backupLink = dir.filePath(QStringLiteral("BackupLink"));
    ASSERT_EQ(QProcess::execute(QStringLiteral("cmd.exe"),
        {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
         QDir::toNativeSeparators(backupLink), QDir::toNativeSeparators(external)}), 0);
    const auto removeBackupLink = qScopeGuard([&]() { QDir().rmdir(backupLink); });
    request.removeIntegration = true;
    request.confirmRemoval = true;
    request.removalBackupPath = QDir(backupLink).filePath(QStringLiteral("Snapshot"));
    EXPECT_FALSE(service.run(request).integrationRemoved);
    EXPECT_FALSE(QDir(QDir(external).filePath(QStringLiteral("Snapshot"))).exists());
}
#endif

TEST(UnityIntegrationService, RejectsInvalidExplicitSdkSourceWithoutFallback)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString invalidSdkRoot = QDir(dir.path()).filePath(
        QStringLiteral("NotHarkenSDK"));
    ASSERT_TRUE(QDir().mkpath(invalidSdkRoot));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = invalidSdkRoot;

    const auto result = service.run(request);

    ASSERT_FALSE(result.success);
    EXPECT_TRUE(result.sdkSourcePath.isEmpty());
    EXPECT_EQ(result.steps[1].status,
              VNX::UnityIntegrationService::StepStatus::Failed);
    EXPECT_TRUE(result.steps[1].cause.contains(invalidSdkRoot));
}

TEST(UnityIntegrationService, ResolvesSdkTotalDirectoryToUnityPackageRoot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString totalRoot = QDir(dir.path()).filePath(QStringLiteral("HarkenSDK"));
    const QString packageRoot = QDir(totalRoot).filePath(QStringLiteral("Unity/Harken"));
    makeCompleteSdk(packageRoot);

    EXPECT_EQ(VNX::UnityIntegrationService::locateSdkSource(totalRoot),
              QDir(packageRoot).absolutePath());
}

TEST(UnityIntegrationService, ResolvesUnityClassificationDirectoryToPackageRoot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = QDir(dir.path()).filePath(QStringLiteral("Unity"));
    const QString packageRoot = QDir(unityRoot).filePath(QStringLiteral("Harken"));
    makeCompleteSdk(packageRoot);

    EXPECT_EQ(VNX::UnityIntegrationService::locateSdkSource(unityRoot),
              QDir(packageRoot).absolutePath());
    EXPECT_EQ(VNX::UnityIntegrationService::locateSdkSource(packageRoot),
              QDir(packageRoot).absolutePath());
}

TEST(UnityIntegrationService, InstallsTheSameSourceDirectlyUnderAssetsHarkenSdk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("ExternalUnityPackage")));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    request.installToAssets = true;

    const auto result = service.run(request);

    ASSERT_TRUE(result.success);
    EXPECT_TRUE(QFileInfo::exists(
        QDir(unityRoot).filePath(QStringLiteral("Assets/HarkenSDK/package.json"))));
    EXPECT_FALSE(QFileInfo::exists(
        QDir(unityRoot).filePath(QStringLiteral("Assets/HarkenSDK/Unity/Harken"))));
}

TEST(UnityIntegrationService, RejectsIncompleteBundledSdkAndWritesReport)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    ASSERT_TRUE(QFile::remove(QDir(sdkRoot).filePath(hostRuntimeRelativePath())));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;

    const auto result = service.run(request);

    ASSERT_FALSE(result.success);
    ASSERT_EQ(result.steps.size(), 6);
    EXPECT_EQ(result.steps[0].status,
              VNX::UnityIntegrationService::StepStatus::Succeeded);
    EXPECT_EQ(result.steps[1].status,
              VNX::UnityIntegrationService::StepStatus::Failed);
#if defined(Q_OS_WIN)
    const QString expectedHostName = QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
    const QString expectedHostName = QStringLiteral("macOS");
#else
    const QString expectedHostName = QStringLiteral("Linux");
#endif
    EXPECT_TRUE(result.steps[1].summary.contains(expectedHostName));
    EXPECT_EQ(result.steps[5].status,
              VNX::UnityIntegrationService::StepStatus::Succeeded);
    EXPECT_TRUE(QFileInfo::exists(result.jsonReportPath));
    EXPECT_TRUE(QFileInfo::exists(result.markdownReportPath));
}

TEST(UnityIntegrationService, ReportsButDoesNotBlockTargetsRequiringAnotherHost)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QStringList runtimePaths = {
        QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"),
        QStringLiteral("Runtime/Plugins/macOS/libHarkenCore.dylib"),
        QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so"),
        QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so"),
        QStringLiteral("Runtime/Plugins/iOS/libHarkenCore.a"),
    };
    const QStringList requiredPaths = requiredRuntimeRelativePaths();
    for (const QString& path : runtimePaths) {
        if (!requiredPaths.contains(path))
            ASSERT_TRUE(QFile::remove(QDir(sdkRoot).filePath(path)));
    }

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;

    const auto result = service.run(request);

    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.steps[1].status,
              VNX::UnityIntegrationService::StepStatus::Warning);
    EXPECT_TRUE(result.hasWarnings);
    EXPECT_TRUE(result.steps[1].details.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("deferred to another operating system")));
}

TEST(UnityIntegrationService, WarnsButDoesNotBlockMissingAndroidRuntime)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    ASSERT_TRUE(QFile::remove(QDir(sdkRoot).filePath(
        QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so"))));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;

    const auto result = service.run(request);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.steps[1].status,
              VNX::UnityIntegrationService::StepStatus::Warning);
    EXPECT_TRUE(result.steps[1].summary.contains(QStringLiteral("Android")));
    EXPECT_TRUE(result.steps[1].summary.contains(QStringLiteral("can still be integrated")));
    EXPECT_TRUE(result.steps[1].cause.contains(QStringLiteral("selected SDK source")));
    EXPECT_TRUE(result.steps[1].remediation.contains(QStringLiteral("continue")));
    EXPECT_TRUE(result.steps[1].details.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("Android APK or AAB")));
    EXPECT_EQ(result.steps[3].status,
              VNX::UnityIntegrationService::StepStatus::Succeeded);
}

TEST(UnityIntegrationService, IntegratesAvailableHostRuntimeWhenOtherTargetsAreMissing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QStringList runtimePaths = {
        QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"),
        QStringLiteral("Runtime/Plugins/macOS/libHarkenCore.dylib"),
        QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so"),
        QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so"),
        QStringLiteral("Runtime/Plugins/iOS/libHarkenCore.a"),
    };
    for (const QString& path : runtimePaths) {
        if (path != hostRuntimeRelativePath())
            ASSERT_TRUE(QFile::remove(QDir(sdkRoot).filePath(path)));
    }

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;

    const auto result = service.run(request);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.hasWarnings);
    EXPECT_EQ(result.steps[1].status,
              VNX::UnityIntegrationService::StepStatus::Warning);
    EXPECT_TRUE(result.steps[1].summary.contains(
        QStringLiteral("Available platforms can still be integrated")));
    EXPECT_EQ(result.steps[3].status,
              VNX::UnityIntegrationService::StepStatus::Succeeded);
    EXPECT_TRUE(QFileInfo::exists(QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core/") + hostRuntimeRelativePath())));
}

TEST(UnityIntegrationService, CheckOnlyNeverInstallsOrReplacesSdk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QString targetRoot = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    request.checkOnly = true;

    const auto missingPreview = service.run(request);
    EXPECT_TRUE(missingPreview.success);
    EXPECT_FALSE(missingPreview.targetInstalled);
    EXPECT_FALSE(missingPreview.targetCurrent);
    EXPECT_TRUE(missingPreview.changesDetected);
    EXPECT_FALSE(QDir(targetRoot).exists());
    EXPECT_EQ(missingPreview.steps[3].status,
              VNX::UnityIntegrationService::StepStatus::Skipped);

    request.checkOnly = false;
    ASSERT_TRUE(service.run(request).success);
    const QString managedPath = QDir(targetRoot).filePath(
        QStringLiteral("Runtime/Harken.Core.asmdef"));
    writeFile(managedPath, QByteArrayLiteral("locally-modified"));

    request.checkOnly = true;
    const auto changedPreview = service.run(request);
    QString failedSummary;
    for (const auto& step : changedPreview.steps) {
        if (step.status == VNX::UnityIntegrationService::StepStatus::Failed)
            failedSummary = step.summary + QLatin1Char('\n') + step.cause;
    }
    EXPECT_TRUE(changedPreview.success) << failedSummary.toStdString();
    EXPECT_TRUE(changedPreview.targetInstalled);
    EXPECT_FALSE(changedPreview.targetCurrent);
    EXPECT_TRUE(changedPreview.changesDetected);
    EXPECT_FALSE(changedPreview.requiresConfirmation);
    EXPECT_EQ(readFile(managedPath), QByteArrayLiteral("locally-modified"));
}

TEST(UnityIntegrationService, IgnoresSdkNamedDirectoriesWithoutSdkIdentity)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    ASSERT_TRUE(QDir().mkpath(QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core"))));
    writeFile(QDir(unityRoot).filePath(
                  QStringLiteral("Assets/HarkenSDK/Notes.txt")),
              QByteArrayLiteral("project-owned"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;

    const auto result = service.run(request);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(QFileInfo::exists(QDir(unityRoot).filePath(
        QStringLiteral("Assets/HarkenSDK/Notes.txt"))));
    EXPECT_TRUE(QFileInfo::exists(QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core/package.json"))));
}

TEST(UnityIntegrationService, InstallsCompleteSdkWithoutBuildingPacks)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;

    const auto result = service.run(request);

    ASSERT_TRUE(result.success);
    EXPECT_FALSE(result.requiresConfirmation);
    EXPECT_TRUE(result.targetCurrent);
    EXPECT_FALSE(result.targetAlreadyCurrent);
    EXPECT_GT(result.sdkFilesCopied, 0);
    EXPECT_TRUE(QFileInfo::exists(QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core/Runtime/Plugins/x86_64/HarkenCore.dll"))));
    EXPECT_TRUE(QFileInfo::exists(QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core/Runtime/Plugins/iOS/libHarkenCore.a"))));
    EXPECT_FALSE(QDir(QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core/Unreal"))).exists());
    EXPECT_FALSE(QDir(QDir(unityRoot).filePath(
        QStringLiteral("Assets/StreamingAssets/Harken"))).exists());
    for (const auto& step : result.steps)
        EXPECT_NE(step.status, VNX::UnityIntegrationService::StepStatus::Failed)
            << step.title.toStdString() << ": " << step.cause.toStdString();
}

TEST(UnityIntegrationService, RemovesManagedIntegrationAndPreservesProjectFiles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QString targetRoot = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request installRequest;
    installRequest.unityProjectPath = unityRoot;
    installRequest.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(installRequest).success);

    const QString projectFile = QDir(targetRoot).filePath(
        QStringLiteral("Runtime/ProjectOnly.cs"));
    const QString projectMeta = projectFile + QStringLiteral(".meta");
    const QString hiddenProjectFile = QDir(targetRoot).filePath(
        QStringLiteral(".cache/project-note.txt"));
    const QString packageMeta = targetRoot + QStringLiteral(".meta");
    const QString legacyRuntime = QDir(targetRoot).filePath(
        QStringLiteral("Runtime/Plugins/macOS/libharken_core.dylib"));
    const QString legacyRuntimeMeta = legacyRuntime + QStringLiteral(".meta");
    const QString finderMetadata = QDir(targetRoot).filePath(
        QStringLiteral("Runtime/Plugins/.DS_Store"));
    writeFile(projectFile, QByteArrayLiteral("project-owned"));
    writeFile(projectMeta, QByteArrayLiteral("project-meta"));
    writeFile(hiddenProjectFile, QByteArrayLiteral("project-note"));
    writeFile(packageMeta, QByteArrayLiteral("package-meta"));
    writeFile(legacyRuntime, QByteArrayLiteral("legacy-runtime"));
    writeFile(legacyRuntimeMeta, QByteArrayLiteral("legacy-runtime-meta"));
    writeFile(finderMetadata, QByteArrayLiteral("finder-metadata"));

    VNX::UnityIntegrationService::Request removeRequest;
    removeRequest.unityProjectPath = unityRoot;
    removeRequest.removeIntegration = true;
    removeRequest.removalBackupPath = QDir(dir.path()).filePath(
        QStringLiteral("AppDataBackup/2026-07-21_UnitTest_UnityGame"));

    const auto preview = service.run(removeRequest);
    EXPECT_FALSE(preview.success);
    EXPECT_TRUE(preview.requiresConfirmation);
    EXPECT_TRUE(preview.integrationFound);
    EXPECT_EQ(preview.removalBackupPath, removeRequest.removalBackupPath);
    EXPECT_FALSE(QFileInfo::exists(removeRequest.removalBackupPath));
    EXPECT_TRUE(QFileInfo::exists(QDir(targetRoot).filePath(
        QStringLiteral("package.json"))));

    removeRequest.confirmRemoval = true;
    const auto result = service.run(removeRequest);

    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.integrationRemoved);
    EXPECT_TRUE(result.removalBackupCreated);
    EXPECT_FALSE(result.targetInstalled);
    EXPECT_FALSE(result.targetCurrent);
    EXPECT_GT(result.filesRemoved, 0);
    EXPECT_EQ(result.targetOnlyFilesPreserved, 3);
    EXPECT_FALSE(QFileInfo::exists(QDir(targetRoot).filePath(
        QStringLiteral("package.json"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(targetRoot).filePath(
        QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(targetRoot).filePath(
        QStringLiteral(".harken-integration-state.json"))));
    EXPECT_FALSE(QFileInfo::exists(legacyRuntime));
    EXPECT_FALSE(QFileInfo::exists(legacyRuntimeMeta));
    EXPECT_FALSE(QFileInfo::exists(finderMetadata));
    EXPECT_EQ(readFile(projectFile), QByteArrayLiteral("project-owned"));
    EXPECT_EQ(readFile(projectMeta), QByteArrayLiteral("project-meta"));
    EXPECT_EQ(readFile(hiddenProjectFile), QByteArrayLiteral("project-note"));
    EXPECT_EQ(readFile(packageMeta), QByteArrayLiteral("package-meta"));
    const QDir backup(removeRequest.removalBackupPath);
    const QString snapshot = backup.filePath(
        QStringLiteral("UnityProject/Packages/com.harken.core"));
    const QString reintegrationSdk = backup.filePath(
        QStringLiteral("ReintegrationSDK"));
    EXPECT_TRUE(QFileInfo::exists(QDir(snapshot).filePath(
        QStringLiteral(".harken-integration-state.json"))));
    EXPECT_TRUE(QFileInfo::exists(QDir(snapshot).filePath(
        QStringLiteral("Runtime/Plugins/macOS/libharken_core.dylib"))));
    EXPECT_TRUE(QFileInfo::exists(QDir(snapshot).filePath(
        QStringLiteral("Runtime/Plugins/.DS_Store"))));
    EXPECT_EQ(readFile(QDir(snapshot).filePath(
                  QStringLiteral("Runtime/ProjectOnly.cs"))),
              QByteArrayLiteral("project-owned"));
    EXPECT_EQ(readFile(backup.filePath(
                  QStringLiteral("UnityProject/Packages/com.harken.core.meta"))),
              QByteArrayLiteral("package-meta"));
    EXPECT_TRUE(QFileInfo::exists(QDir(reintegrationSdk).filePath(
        QStringLiteral("package.json"))));
    EXPECT_TRUE(QFileInfo::exists(QDir(reintegrationSdk).filePath(
        QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(reintegrationSdk).filePath(
        QStringLiteral("Runtime/ProjectOnly.cs"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(reintegrationSdk).filePath(
        QStringLiteral("Runtime/Plugins/macOS/libharken_core.dylib"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(reintegrationSdk).filePath(
        QStringLiteral("Runtime/Plugins/.DS_Store"))));
    EXPECT_TRUE(QFileInfo::exists(backup.filePath(
        QStringLiteral("backup.json"))));
    EXPECT_TRUE(QFileInfo::exists(backup.filePath(
        QStringLiteral("README-FIRST.txt"))));
    EXPECT_TRUE(QFileInfo::exists(result.jsonReportPath));
    EXPECT_TRUE(QFileInfo::exists(result.markdownReportPath));

    VNX::UnityIntegrationService::Request reinstallRequest;
    reinstallRequest.unityProjectPath = unityRoot;
    reinstallRequest.sdkSourcePath = reintegrationSdk;
    auto reinstallResult = service.run(reinstallRequest);
    ASSERT_FALSE(reinstallResult.success);
    EXPECT_TRUE(reinstallResult.steps[0].cause.contains(QStringLiteral("unrelated files")));
    const QString preservedRoot = QDir(dir.path()).filePath(QStringLiteral("PreservedProjectFiles"));
    ASSERT_TRUE(QDir().rename(targetRoot, preservedRoot));
    reinstallResult = service.run(reinstallRequest);
    ASSERT_TRUE(reinstallResult.success);
    EXPECT_TRUE(reinstallResult.targetCurrent);
    EXPECT_EQ(readFile(QDir(preservedRoot).filePath(QStringLiteral("Runtime/ProjectOnly.cs"))), QByteArrayLiteral("project-owned"));
    EXPECT_EQ(readFile(QDir(preservedRoot).filePath(QStringLiteral(".cache/project-note.txt"))), QByteArrayLiteral("project-note"));
    EXPECT_FALSE(QFileInfo::exists(legacyRuntime));
}

TEST(UnityIntegrationService, RefusesRemovalWithoutOwnershipRecord)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString targetRoot = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core"));
    writeFile(QDir(targetRoot).filePath(QStringLiteral("package.json")),
              QJsonDocument(QJsonObject{
                  {QStringLiteral("name"), QStringLiteral("com.harken.core")},
              }).toJson());
    const QString projectFile = QDir(targetRoot).filePath(
        QStringLiteral("Runtime/ProjectOnly.cs"));
    writeFile(projectFile, QByteArrayLiteral("project-owned"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.removeIntegration = true;
    request.confirmRemoval = true;
    request.removalBackupPath = QDir(dir.path()).filePath(
        QStringLiteral("ShouldNotExist"));

    const auto result = service.run(request);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.integrationFound);
    EXPECT_FALSE(result.integrationRemoved);
    EXPECT_FALSE(result.removalBackupCreated);
    EXPECT_FALSE(QFileInfo::exists(request.removalBackupPath));
    EXPECT_EQ(readFile(projectFile), QByteArrayLiteral("project-owned"));
    EXPECT_TRUE(result.steps[1].summary.contains(
        QStringLiteral("ownership record")));
}

TEST(UnityIntegrationService, RemovesAssetsIntegrationWithAssetsBackupPaths)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QString targetRoot = QDir(unityRoot).filePath(
        QStringLiteral("Assets/HarkenSDK"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request installRequest;
    installRequest.unityProjectPath = unityRoot;
    installRequest.sdkSourcePath = sdkRoot;
    installRequest.installToAssets = true;
    ASSERT_TRUE(service.run(installRequest).success);
    writeFile(targetRoot + QStringLiteral(".meta"),
              QByteArrayLiteral("assets-package-meta"));

    VNX::UnityIntegrationService::Request removeRequest;
    removeRequest.unityProjectPath = unityRoot;
    removeRequest.installToAssets = true;
    removeRequest.removeIntegration = true;
    removeRequest.removalBackupPath = QDir(dir.path()).filePath(
        QStringLiteral("AppDataBackup/Assets_UnityGame"));

    const auto preview = service.run(removeRequest);
    EXPECT_FALSE(preview.success);
    EXPECT_TRUE(preview.requiresConfirmation);
    EXPECT_TRUE(preview.steps[1].details.join(QStringLiteral("\n"))
                    .contains(QStringLiteral("Location: Assets/HarkenSDK/")));

    removeRequest.confirmRemoval = true;
    const auto result = service.run(removeRequest);

    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.integrationRemoved);
    EXPECT_FALSE(QFileInfo::exists(targetRoot));
    const QDir backup(removeRequest.removalBackupPath);
    const QString snapshot = backup.filePath(
        QStringLiteral("UnityProject/Assets/HarkenSDK"));
    EXPECT_TRUE(QFileInfo::exists(QDir(snapshot).filePath(
        QStringLiteral("package.json"))));
    EXPECT_EQ(readFile(backup.filePath(
                  QStringLiteral("UnityProject/Assets/HarkenSDK.meta"))),
              QByteArrayLiteral("assets-package-meta"));
    EXPECT_FALSE(QFileInfo::exists(backup.filePath(
        QStringLiteral("UnityProject/Packages/com.harken.core"))));
}

TEST(UnityIntegrationService, RefusesRemovalWithUnsupportedOwnershipVersion)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QString targetRoot = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request installRequest;
    installRequest.unityProjectPath = unityRoot;
    installRequest.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(installRequest).success);

    const QString statePath = QDir(targetRoot).filePath(
        QStringLiteral(".harken-integration-state.json"));
    QJsonObject state = QJsonDocument::fromJson(readFile(statePath)).object();
    state[QStringLiteral("schema_version")] = 99;
    writeFile(statePath, QJsonDocument(state).toJson());

    VNX::UnityIntegrationService::Request removeRequest;
    removeRequest.unityProjectPath = unityRoot;
    removeRequest.removeIntegration = true;
    removeRequest.confirmRemoval = true;
    removeRequest.removalBackupPath = QDir(dir.path()).filePath(
        QStringLiteral("ShouldNotExist"));

    const auto result = service.run(removeRequest);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.integrationRemoved);
    EXPECT_FALSE(result.removalBackupCreated);
    EXPECT_TRUE(QFileInfo::exists(QDir(targetRoot).filePath(
        QStringLiteral("package.json"))));
    EXPECT_TRUE(result.steps[1].summary.contains(
        QStringLiteral("ownership record")));
}

TEST(UnityIntegrationService, DoesNotRemoveAnythingWhenBackupCannotStart)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QString targetPackage = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core/package.json"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request installRequest;
    installRequest.unityProjectPath = unityRoot;
    installRequest.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(installRequest).success);

    const QString occupiedBackup = QDir(dir.path()).filePath(
        QStringLiteral("ExistingBackup"));
    ASSERT_TRUE(QDir().mkpath(occupiedBackup));
    VNX::UnityIntegrationService::Request removeRequest;
    removeRequest.unityProjectPath = unityRoot;
    removeRequest.removeIntegration = true;
    removeRequest.confirmRemoval = true;
    removeRequest.removalBackupPath = occupiedBackup;

    const auto result = service.run(removeRequest);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.removalBackupCreated);
    EXPECT_FALSE(result.integrationRemoved);
    EXPECT_TRUE(QFileInfo::exists(targetPackage));
    EXPECT_TRUE(result.steps[2].summary.contains(
        QStringLiteral("already exists")));
}

#ifdef Q_OS_WIN
TEST(UnityIntegrationService, BackupFailurePreservesSdkAndReportsSystemDiagnostics)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString project = makeUnityProject(dir.filePath("Game"));
    const QString source = makeCompleteSdk(dir.filePath("Source"));
    VNH::UnityIntegrationRequest request;
    request.unityProjectPath = project;
    request.sdkSourcePath = source;
    ASSERT_TRUE(VNH::HubUnityIntegrationService::run(request).success);
    const QString installed = project + "/Packages/com.harken.core";
    const QByteArray original = readFile(installed + "/Runtime/Harken.Core.asmdef");
    QFile held(installed + "/Runtime/Generated/HarkenAbi.g.cs");
    ASSERT_TRUE(held.open(QIODevice::ReadOnly));
    writeFile(source + "/Runtime/Harken.Core.asmdef", "updated SDK");
    request.confirmTargetChanges = true;
    const auto result = VNH::HubUnityIntegrationService::run(request);
    EXPECT_FALSE(result.success);
    bool backupFailed = false;
    for (const auto& step : result.steps) {
        if (step.summary != "Could not back up the existing Unity SDK.") continue;
        backupFailed = true;
        EXPECT_TRUE(step.cause.contains(installed));
        EXPECT_TRUE(step.cause.contains(".backup-"));
        EXPECT_TRUE(step.cause.contains("system error"));
        EXPECT_TRUE(step.remediation.contains("permissions"));
        EXPECT_TRUE(step.remediation.contains("processes"));
    }
    EXPECT_TRUE(backupFailed);
    EXPECT_EQ(readFile(installed + "/Runtime/Harken.Core.asmdef"), original);
    held.close();
    EXPECT_TRUE(VNH::HubUnityIntegrationService::run(request).success);
    EXPECT_EQ(readFile(installed + "/Runtime/Harken.Core.asmdef"), "updated SDK");
}
#endif

TEST(UnityIntegrationService, RequiresApprovalBeforeReplacingExistingSdk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QString targetRoot = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);

    const QString managedPath = QDir(targetRoot).filePath(
        QStringLiteral("Runtime/Harken.Core.asmdef"));
    writeFile(managedPath, QByteArrayLiteral("locally-modified"));
    const QString customPath = QDir(targetRoot).filePath(
        QStringLiteral("Runtime/ProjectOnly.cs"));
    writeFile(customPath, QByteArrayLiteral("custom"));
    const QString stalePath = QDir(targetRoot).filePath(
        QStringLiteral("Runtime/OldManaged.cs"));
    writeFile(stalePath, QByteArrayLiteral("stale"));

    const QString statePath = QDir(targetRoot).filePath(
        QStringLiteral(".harken-integration-state.json"));
    QJsonObject state = QJsonDocument::fromJson(readFile(statePath)).object();
    QJsonArray managed = state.value(QStringLiteral("managed_files")).toArray();
    managed.append(QStringLiteral("Runtime/OldManaged.cs"));
    state[QStringLiteral("managed_files")] = managed;
    writeFile(statePath, QJsonDocument(state).toJson());

    const auto preview = service.run(request);
    EXPECT_FALSE(preview.success);
    EXPECT_TRUE(preview.requiresConfirmation);
    EXPECT_EQ(preview.filesChanged, 1);
    EXPECT_EQ(preview.staleFilesToRemove, 1);
    EXPECT_EQ(preview.targetOnlyFilesPreserved, 1);
    const QString readableDetails = preview.steps[2].details.join(QStringLiteral("\n\n"));
    EXPECT_TRUE(readableDetails.contains(QStringLiteral("Files to update")));
    EXPECT_TRUE(readableDetails.contains(QStringLiteral("Files kept unchanged")));
    EXPECT_TRUE(readableDetails.contains(QStringLiteral("Location: Packages/com.harken.core/")));
    EXPECT_TRUE(readableDetails.contains(QStringLiteral("Purpose:")));
    EXPECT_TRUE(readableDetails.contains(QStringLiteral("Planned change:")));
    EXPECT_TRUE(readableDetails.contains(QStringLiteral("Impact:")));
    EXPECT_FALSE(readableDetails.contains(QStringLiteral("REPLACE  ")));
    EXPECT_FALSE(readableDetails.contains(QStringLiteral("PRESERVE ")));
    EXPECT_EQ(readFile(managedPath), QByteArrayLiteral("locally-modified"));
    EXPECT_TRUE(QFileInfo::exists(stalePath));

    request.confirmTargetChanges = true;
    const auto updated = service.run(request);
    ASSERT_TRUE(updated.success);
    EXPECT_EQ(readFile(managedPath), readFile(QDir(sdkRoot).filePath(
        QStringLiteral("Runtime/Harken.Core.asmdef"))));
    EXPECT_EQ(readFile(customPath), QByteArrayLiteral("custom"));
    EXPECT_FALSE(QFileInfo::exists(stalePath));
}

TEST(UnityIntegrationService, ReportsUnreadableManagedFileInsteadOfCountingItAsChanged)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QString targetRoot = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);

    const QString changed = QStringLiteral("Runtime/Generated/HarkenAbi.g.cs");
    writeFile(QDir(targetRoot).filePath(changed), QByteArrayLiteral("locally-modified"));
    const QString missing = QStringLiteral("Runtime/Generated/HarkenNative.g.cs");
    ASSERT_TRUE(QFile::remove(QDir(targetRoot).filePath(missing)));
    const QString managed = QStringLiteral("Runtime/Harken.Core.asmdef");
    makeUnreadable(QDir(targetRoot).filePath(managed));

    request.checkOnly = true;
    const auto result = service.run(request);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.steps[2].status,
              VNX::UnityIntegrationService::StepStatus::Failed);
    EXPECT_TRUE(result.steps[2].details.contains(managed));
    EXPECT_TRUE(result.steps[2].summary.contains(QStringLiteral("could not be read")));
    EXPECT_EQ(result.filesAdded, 0);
    EXPECT_EQ(result.filesChanged, 0);
    EXPECT_TRUE(result.fileChanges.isEmpty());
    EXPECT_FALSE(result.targetAlreadyCurrent);
}

TEST(UnityIntegrationService, ReportsUnreadableOwnershipRecordInsteadOfDiscardingIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QString targetRoot = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);

    makeUnreadable(QDir(targetRoot).filePath(
        QStringLiteral(".harken-integration-state.json")));

    request.checkOnly = true;
    const auto result = service.run(request);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.steps[2].status,
              VNX::UnityIntegrationService::StepStatus::Failed);
    EXPECT_TRUE(result.steps[2].cause.contains(
        QStringLiteral(".harken-integration-state.json")));
    EXPECT_EQ(result.targetOnlyFilesPreserved, 0);
    EXPECT_EQ(result.staleFilesToRemove, 0);
}

TEST(UnityIntegrationService, DetectsAlreadyCurrentSdkWithoutCopying)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);
    const QString targetOnly = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core/Runtime/ProjectOnly.cs"));
    writeFile(targetOnly, QByteArrayLiteral("custom"));

    const auto result = service.run(request);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.requiresConfirmation);
    EXPECT_TRUE(result.targetAlreadyCurrent);
    EXPECT_TRUE(result.targetCurrent);
    EXPECT_TRUE(result.hasWarnings);
    EXPECT_EQ(result.targetOnlyFilesPreserved, 1);
    EXPECT_EQ(result.sdkFilesCopied, 0);
    EXPECT_EQ(readFile(targetOnly), QByteArrayLiteral("custom"));
    EXPECT_EQ(result.steps[3].status,
              VNX::UnityIntegrationService::StepStatus::Skipped);
}

TEST(UnityIntegrationService, RenamesRuntimeAndPreservesPluginMetadata)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(QDir(dir.path()).filePath(QStringLiteral("SDK")));
    const QString targetRoot = QDir(unityRoot).filePath(QStringLiteral("Packages/com.harken.core"));
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);
    for (const QString& runtime : {
             QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"),
             QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so"),
             QStringLiteral("Runtime/Plugins/macOS/libHarkenCore.dylib"),
             QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so"),
             QStringLiteral("Runtime/Plugins/iOS/libHarkenCore.a")})
        ASSERT_TRUE(QFile::remove(QDir(targetRoot).filePath(runtime)));
    const QStringList oldPaths = {
        QStringLiteral("Runtime/Plugins/x86_64/harken_core.dll"),
        QStringLiteral("Runtime/Plugins/x86_64/libharken_core.so"),
        QStringLiteral("Runtime/Plugins/macOS/libharken_core.dylib"),
        QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libharken_core.so"),
        QStringLiteral("Runtime/Plugins/iOS/libharken_core.a"),
    };
    const QByteArray metadata = QByteArrayLiteral("guid: 66744ddb495183e44ac9e0bb5435e515\nplatformData: custom\n");
    for (const QString& oldPath : oldPaths) {
        writeFile(QDir(targetRoot).filePath(oldPath), QByteArrayLiteral("old-runtime"));
        writeFile(QDir(targetRoot).filePath(oldPath + QStringLiteral(".meta")), metadata);
    }
    request.checkOnly = true;
    const auto check = service.run(request);
    ASSERT_TRUE(check.success);
    EXPECT_EQ(check.staleFilesToRemove, oldPaths.size() * 2);
    for (const QString& oldPath : oldPaths)
        EXPECT_TRUE(QFileInfo::exists(QDir(targetRoot).filePath(oldPath)));

    request.checkOnly = false;
    request.confirmTargetChanges = true;
    const auto result = service.run(request);
    ASSERT_TRUE(result.success);
    for (const QString& oldPath : oldPaths) {
        QString newPath = oldPath;
        newPath.replace(QStringLiteral("harken_core"), QStringLiteral("HarkenCore"));
        EXPECT_FALSE(QFileInfo::exists(QDir(targetRoot).filePath(oldPath)));
        EXPECT_FALSE(QFileInfo::exists(QDir(targetRoot).filePath(oldPath + QStringLiteral(".meta"))));
        EXPECT_TRUE(QFileInfo::exists(QDir(targetRoot).filePath(newPath)));
        EXPECT_EQ(readFile(QDir(targetRoot).filePath(newPath + QStringLiteral(".meta"))), metadata);
    }
    request.checkOnly = true;
    EXPECT_TRUE(service.run(request).targetAlreadyCurrent);
}

TEST(UnityIntegrationService, RemovesObsoleteRuntimeNamesInsteadOfWarning)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QString targetRoot = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core"));
    const QStringList obsoleteRuntimes = {
        QStringLiteral("Runtime/Plugins/iOS/libharken_core.a"),
        QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libharken_core.so"),
        QStringLiteral("Runtime/Plugins/macOS/libharken_core.dylib"),
        QStringLiteral("Runtime/Plugins/x86_64/harken_core.dll"),
    };

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);
    for (const QString& relative : obsoleteRuntimes) {
        writeFile(QDir(targetRoot).filePath(relative),
                  QByteArrayLiteral("obsolete-runtime"));
        writeFile(QDir(targetRoot).filePath(relative + QStringLiteral(".meta")),
                  QByteArrayLiteral("obsolete-runtime-meta"));
    }

    request.checkOnly = true;
    const auto check = service.run(request);
    ASSERT_TRUE(check.success);
    EXPECT_FALSE(check.targetAlreadyCurrent);
    EXPECT_FALSE(check.targetCurrent);
    EXPECT_EQ(check.staleFilesToRemove, obsoleteRuntimes.size() * 2);
    EXPECT_EQ(check.targetOnlyFilesPreserved, 0);
    EXPECT_TRUE(check.steps[2].summary.contains(
        QStringLiteral("obsolete Harken-managed")));

    request.checkOnly = false;
    const auto preview = service.run(request);
    ASSERT_TRUE(preview.requiresConfirmation);
    EXPECT_FALSE(preview.targetCurrent);
    request.confirmTargetChanges = true;
    const auto updated = service.run(request);
    ASSERT_TRUE(updated.success);
    EXPECT_TRUE(updated.targetCurrent);
    for (const QString& relative : obsoleteRuntimes) {
        EXPECT_FALSE(QFileInfo::exists(QDir(targetRoot).filePath(relative)));
        EXPECT_FALSE(QFileInfo::exists(
            QDir(targetRoot).filePath(relative + QStringLiteral(".meta"))));
    }

    request.confirmTargetChanges = false;
    const auto current = service.run(request);
    if (!current.success) {
        QStringList stepDump;
        for (const auto& step : current.steps) {
            stepDump << QStringLiteral("%1 status=%2 summary=%3 cause=%4 details=%5")
                            .arg(step.id)
                            .arg(static_cast<int>(step.status))
                            .arg(step.summary, step.cause, step.details.join(QLatin1Char('|')));
        }
        ADD_FAILURE() << stepDump.join(QLatin1Char('\n')).toStdString();
    }
    EXPECT_TRUE(current.success);
    EXPECT_TRUE(current.targetAlreadyCurrent);
    EXPECT_TRUE(current.targetCurrent);
    EXPECT_EQ(current.staleFilesToRemove, 0);
}

TEST(UnityIntegrationService, TreatsUnityGeneratedMetadataAsExpected)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);
    const QString unityMetadata = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core/Runtime/Harken.Core.asmdef.meta"));
    writeFile(unityMetadata, QByteArrayLiteral("unity-generated-metadata"));

    request.checkOnly = true;
    const auto result = service.run(request);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.targetAlreadyCurrent);
    EXPECT_FALSE(result.changesDetected);
    EXPECT_FALSE(result.hasWarnings);
    EXPECT_EQ(result.steps[2].status,
              VNX::UnityIntegrationService::StepStatus::Succeeded);
    EXPECT_TRUE(result.steps[2].summary.contains(QStringLiteral("retained as expected")));
}

TEST(UnityIntegrationService, IgnoresMacOsFinderMetadata)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    writeFile(QDir(sdkRoot).filePath(QStringLiteral(".DS_Store")),
              QByteArrayLiteral("source-finder-metadata"));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);
    const QDir target(QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core")));
    EXPECT_FALSE(QFileInfo::exists(target.filePath(QStringLiteral(".DS_Store"))));

    const QStringList finderFiles = {
        QStringLiteral(".DS_Store"),
        QStringLiteral("Runtime/.DS_Store"),
        QStringLiteral("Runtime/Plugins/.DS_Store"),
        QStringLiteral("Runtime/Plugins/._FinderInfo"),
    };
    for (const QString& relative : finderFiles) {
        writeFile(target.filePath(relative), QByteArrayLiteral("finder-metadata"));
    }

    request.checkOnly = true;
    const auto check = service.run(request);
    EXPECT_TRUE(check.success);
    EXPECT_TRUE(check.targetAlreadyCurrent);
    EXPECT_TRUE(check.targetCurrent);
    EXPECT_FALSE(check.changesDetected);
    EXPECT_FALSE(check.hasWarnings);
    EXPECT_EQ(check.targetOnlyFilesPreserved, 0);

    writeFile(target.filePath(QStringLiteral("Runtime/Harken.Core.asmdef")),
              QByteArrayLiteral("changed"));
    request.checkOnly = false;
    EXPECT_TRUE(service.run(request).requiresConfirmation);
    request.confirmTargetChanges = true;
    const auto updated = service.run(request);
    ASSERT_TRUE(updated.success);
    for (const QString& relative : finderFiles) {
        EXPECT_FALSE(QFileInfo::exists(target.filePath(relative)));
    }
}

TEST(UnityIntegrationService, TreatsDeclaredOptionalRuntimeAsExpected)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));
    const QStringList runtimePaths = {
        QStringLiteral("Runtime/Plugins/x86_64/HarkenCore.dll"),
        QStringLiteral("Runtime/Plugins/macOS/libHarkenCore.dylib"),
        QStringLiteral("Runtime/Plugins/x86_64/libHarkenCore.so"),
        QStringLiteral("Runtime/Plugins/Android/arm64-v8a/libHarkenCore.so"),
        QStringLiteral("Runtime/Plugins/iOS/libHarkenCore.a"),
    };
    const QStringList requiredPaths = requiredRuntimeRelativePaths();
    for (const QString& path : runtimePaths) {
        if (!requiredPaths.contains(path))
            ASSERT_TRUE(QFile::remove(QDir(sdkRoot).filePath(path)));
    }

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);

    const QString optionalRuntime = QDir(unityRoot).filePath(
        QStringLiteral("Packages/com.harken.core/") + optionalRuntimeRelativePath());
    writeFile(optionalRuntime, QByteArrayLiteral("android-target-runtime"));

    request.checkOnly = true;
    const auto result = service.run(request);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.targetAlreadyCurrent);
    EXPECT_FALSE(result.changesDetected);
    EXPECT_TRUE(result.hasWarnings);
    EXPECT_EQ(result.steps[2].status,
              VNX::UnityIntegrationService::StepStatus::Succeeded);
    EXPECT_TRUE(result.steps[2].summary.contains(
        QStringLiteral("optional target runtime file(s) are retained as expected")));
}

TEST(UnityIntegrationService, RetriesAnSdkRenameThatIsBrieflyLocked)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString unityRoot = makeUnityProject(
        QDir(dir.path()).filePath(QStringLiteral("UnityGame")));
    const QString sdkRoot = makeCompleteSdk(
        QDir(dir.path()).filePath(QStringLiteral("HarkenSDK")));

    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityRoot;
    request.sdkSourcePath = sdkRoot;
    ASSERT_TRUE(service.run(request).success);

    const QByteArray updated = QByteArrayLiteral("{\"name\":\"Harken.Core.updated\"}");
    writeFile(QDir(sdkRoot).filePath(QStringLiteral("Runtime/Harken.Core.asmdef")),
              updated);
    request.confirmTargetChanges = true;

    // One held managed file is enough for Windows to refuse the whole
    // com.harken.core -> backup rename, which is exactly what a virus scanner
    // does to a tree that was just written. It lets go a moment later.
    const QString held = QDir(unityRoot).filePath(QStringLiteral(
        "Packages/com.harken.core/Runtime/Generated/HarkenAbi.g.cs"));
    std::atomic<bool> ready{false};
    std::atomic<bool> opened{false};
    QThread* holder = QThread::create([&] {
        QFile lock(held);
        opened = lock.open(QIODevice::ReadOnly);
        ready = true;
        if (!opened) return;
        QThread::msleep(120);
        lock.close();
    });
    holder->start();
    while (!ready) QThread::yieldCurrentThread();
    ASSERT_TRUE(opened);

    const auto result = service.run(request);
    holder->wait();
    delete holder;

    EXPECT_TRUE(result.success);
    EXPECT_EQ(readFile(QDir(unityRoot).filePath(QStringLiteral(
        "Packages/com.harken.core/Runtime/Harken.Core.asmdef"))), updated);
}
