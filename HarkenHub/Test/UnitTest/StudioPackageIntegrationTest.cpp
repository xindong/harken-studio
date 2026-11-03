#include "Launch/LaunchPreflight.h"
#include "Launch/StudioLauncher.h"
#include "Projects/ProjectIntakeConsumer.h"
#include "Packaging/PackageReader.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectStore.h"
#include "Support/ManifestTestUtils.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/InstallPackageCache.h"
#include "Transactions/InstallRequest.h"
#include "Transactions/RepairExecutor.h"
#include "Versions/IntegrityChecker.h"
#include "Versions/LocalBuildRegistry.h"
#include "Versions/SelfTestRunner.h"
#include "Versions/StudioCapabilities.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <QSettings>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

namespace {

bool editorIsVisible(const QString& project)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, 9527);
    if (!socket.waitForConnected(500))
        return false;
    const auto query = [&socket](const QString& command, const QJsonObject& arguments = QJsonObject{}) {
        const QJsonObject request{{"cmd", command}, {"args", arguments}, {"request_id", command}};
        socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 1000) {
            while (socket.canReadLine()) {
                const auto response = QJsonDocument::fromJson(socket.readLine()).object();
                if (response.value("request_id").toString() == command)
                    return response.value("result").toObject();
            }
            if (!socket.waitForReadyRead(100)
                && socket.state() != QAbstractSocket::ConnectedState)
                break;
        }
        return QJsonObject{};
    };
    const auto state = query(QStringLiteral("get_project_state"));
    if (!state.value("project_loaded").toBool()
        || QFileInfo(state.value("project_path").toString()).canonicalFilePath()
            != QFileInfo(project).canonicalFilePath())
        return false;
    return query(QStringLiteral("get_ui_tree"), {{"root", "MainWindow"}, {"depth", 0}})
        .value("visible").toBool();
}

}

TEST(StudioPackageIntegration, InstallsRepairsRegistersCreatesAndOpensRealStudio)
{
    const auto package = qEnvironmentVariable("HARKEN_HUB_STUDIO_PACKAGE");
    if (package.isEmpty())
        GTEST_SKIP() << "Set HARKEN_HUB_STUDIO_PACKAGE to a freshly built Studio package";
    const auto retainedRoot = qEnvironmentVariable("HARKEN_HUB_STUDIO_TEST_ROOT");
    QTemporaryDir directory(QDir(retainedRoot.isEmpty() ? QDir::tempPath() : retainedRoot)
                               .filePath("hub-studio-XXXXXX"));
    directory.setAutoRemove(retainedRoot.isEmpty());
    ASSERT_TRUE(directory.isValid());
    if (!retainedRoot.isEmpty())
        qInfo().noquote() << "Studio integration workspace:" << directory.path();
    const auto keys = HarkenHubTest::makeKeypair();
    qputenv("HARKEN_HUB_USER_ROOT", directory.filePath("user").toUtf8());
    qputenv("HARKEN_HUB_MACHINE_ROOT", directory.filePath("machine").toUtf8());
    qputenv("HARKEN_HUB_VERSIONS_ROOT", directory.filePath("versions").toUtf8());
    qputenv("HARKEN_HUB_MANIFEST_PUBKEY", keys.publicKey.toHex());
    const auto cleanup = qScopeGuard([]() {
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
        qunsetenv("HARKEN_HUB_VERSIONS_ROOT");
        qunsetenv("HARKEN_HUB_MANIFEST_PUBKEY");
    });
    QString error;
    QByteArray manifestBytes;
    ASSERT_TRUE(VNH::PackageReader::readEntry(package, "package-manifest.json", &manifestBytes, &error));
    const auto manifest = QJsonDocument::fromJson(manifestBytes).object();
    const auto build = manifest.value("buildId").toString();
    const auto version = manifest.value("version").toString();
    QByteArray capabilitiesBytes;
    ASSERT_TRUE(VNH::PackageReader::readEntry(package, "capabilities.json", &capabilitiesBytes, &error));
    const auto protocol = QJsonDocument::fromJson(capabilitiesBytes).object().value("capabilitiesVersion").toInt();
    auto payload = HarkenHubTest::baseManifestPayload();
    payload["version"] = version;
    payload["buildId"] = build;
    payload["channel"] = manifest.value("channel");
    payload["packageSize"] = QFileInfo(package).size();
    payload["sha256"] = VNH::PackageReader::fileSha256Hex(package);
    const auto envelope = HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(QJsonDocument::Compact), keys);
    const auto envelopePath = directory.filePath("studio.manifest");
    QFile envelopeFile(envelopePath);
    ASSERT_TRUE(envelopeFile.open(QIODevice::WriteOnly));
    envelopeFile.write(envelope);
    envelopeFile.close();
    const auto now = QDateTime::fromString("2026-09-08T00:00:00Z", Qt::ISODate);
    const auto preparation = VNH::InstallRequest::prepare(package, envelopePath, keys.publicKey, now);
    ASSERT_TRUE(preparation.ok) << preparation.error.toStdString();
    const auto installed = VNH::InstallExecutor::run(preparation.transactionId, keys.publicKey, now);
    ASSERT_TRUE(installed.ok) << installed.error.toStdString();
    const auto versions = VNH::VersionScanner::scanInstalled();
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions.front().expectedBuildId(), build);
    EXPECT_EQ(versions.front().capabilitiesVersion, protocol);
    const auto selfTest = VNH::SelfTestRunner::runForInstall(installed.installPath, build);
    ASSERT_TRUE(selfTest.ok) << selfTest.error.toStdString();
    EXPECT_TRUE(VNH::IntegrityChecker::check(installed.installPath).ok);

    const auto files = manifest.value("files").toArray();
    const auto schemaFile = std::find_if(files.begin(), files.end(), [](const auto& entry) {
        return entry.toObject().value("path").toString().startsWith("schema/");
    });
    ASSERT_NE(schemaFile, files.end());
    QFile damaged(QDir(installed.installPath).filePath(schemaFile->toObject().value("path").toString()));
    ASSERT_TRUE(damaged.open(QIODevice::WriteOnly));
    damaged.write("damaged schema payload");
    damaged.close();
    EXPECT_FALSE(VNH::IntegrityChecker::check(installed.installPath).ok);
    ASSERT_TRUE(VNH::InstallPackageCache::store(build, package, envelopePath, &error));
    const auto repaired = VNH::RepairExecutor::run(build, now);
    ASSERT_TRUE(repaired.ok) << repaired.error.toStdString();
    EXPECT_TRUE(VNH::SelfTestRunner::runForInstall(installed.installPath, build).ok);

    VNH::LocalBuildRecord local;
    ASSERT_TRUE(VNH::LocalBuildRegistry::probe(versions.front().executablePath, &local, &error)) << error.toStdString();
    ASSERT_TRUE(VNH::LocalBuildRegistry::upsert(local, &error));
    EXPECT_TRUE(VNH::LocalBuildRegistry::metadataMatchesFile(local));
    EXPECT_EQ(VNH::VersionScanner::scanLocalBuilds().front().expectedBuildId(), build);

    const auto project = directory.filePath("Projects/Smoke/Smoke.hsp");
    ASSERT_TRUE(QDir().mkpath(directory.filePath("Projects")));
    const auto restoreRecent = qScopeGuard([&]() {
        QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "HarkenSound", "HarkenStudio");
        auto recent = settings.value("RecentProjects").toStringList();
        if (recent.removeAll(project) > 0)
            settings.setValue("RecentProjects", recent);
    });
    VNH::ProjectStore store;
    ASSERT_TRUE(store.open(directory.filePath("projects.db")));
    VNH::ProjectIntakeConsumer intake(&store);
    const auto runStudio = [&](bool create) {
        VNH::StudioLaunchSpec launch;
        launch.installPath = installed.installPath;
        launch.executablePath = versions.front().executablePath;
        launch.projectPath = project;
        VNH::StudioCreateSpec creation;
        creation.launch = launch;
        creation.projectName = "Smoke";
        creation.projectLocation = directory.filePath("Projects");
        QProcess process;
        process.setWorkingDirectory(installed.installPath);
        process.start(launch.executablePath, create ? VNH::StudioLauncher::argumentsForCreate(creation)
                                                    : VNH::StudioLauncher::argumentsFor(launch));
        if (!process.waitForStarted(10000))
            return false;
        QElapsedTimer elapsed;
        elapsed.start();
        bool synchronized = false;
        bool editorShown = false;
        while ((!synchronized || !editorShown)
               && process.state() != QProcess::NotRunning && elapsed.elapsed() < 60000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            synchronized = intake.consumeAll() || synchronized;
            if (synchronized)
                editorShown = editorIsVisible(project);
            QThread::msleep(5);
        }
        process.kill();
        process.waitForFinished(5000);
        EXPECT_TRUE(synchronized) << process.readAllStandardError().toStdString();
        EXPECT_TRUE(editorShown);
        return synchronized && editorShown;
    };
    ASSERT_TRUE(runStudio(true));
    const auto probe = VNH::ProjectProbe::probe(project);
    ASSERT_TRUE(probe.ok);
    if (protocol == VNH::kStudioCapabilitiesVersion)
        EXPECT_EQ(probe.savedStudioVersion, version);
    VNH::ProjectRecord record;
    record.canonicalPath = probe.canonicalPath;
    record.projectId = probe.projectId;
    const auto decision = VNH::LaunchPreflight::evaluate(record, versions, VNH::InstallIndex::snapshot(), build);
    ASSERT_EQ(decision.verdict, VNH::LaunchVerdict::Ready) << decision.reason.toStdString();
    ASSERT_TRUE(runStudio(false));

    QFile cached(VNH::InstallPackageCache::packagePath(build));
    ASSERT_TRUE(cached.open(QIODevice::Append));
    cached.write("tampered");
    cached.close();
    ASSERT_TRUE(damaged.open(QIODevice::WriteOnly));
    damaged.write("damaged again");
    damaged.close();
    const auto rejected = VNH::RepairExecutor::run(build, now);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.failureCode, QStringLiteral("RepairSourceRejected"));
}
