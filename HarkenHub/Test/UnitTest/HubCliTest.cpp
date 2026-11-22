#include <gtest/gtest.h>

#include "Support/ManifestTestUtils.h"
#include "Support/StudioProtocolTestUtils.h"
#include "Support/RangeHttpServer.h"
#include "App/SingleInstanceGuard.h"
#include "Platform/PlatformPaths.h"
#include "Platform/ProcessScan.h"
#include "ZipTestUtils.h"

#include <QDir>
#include <QDateTime>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QThread>
#include <QScopeGuard>
#include <memory>
#include <vector>

namespace {

class HubCliTest : public testing::Test
{
protected:
    QTemporaryDir directory;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        environment.insert("HARKEN_HUB_USER_ROOT", directory.filePath("user"));
        environment.insert("HARKEN_HUB_MACHINE_ROOT", directory.filePath("machine"));
        environment.insert("HARKEN_HUB_VERSIONS_ROOT", directory.filePath("versions"));
        environment.insert("QT_QPA_PLATFORM", "invalid-platform-cli-must-not-load");
        const auto paths = run({"diagnostics", "paths"});
        ASSERT_EQ(paths["data"].toObject()["user"].toString(), directory.filePath("user"));
    }

    QJsonObject run(const QStringList& arguments, int expected = 0, const QString& workingDirectory = {})
    {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(workingDirectory.isEmpty() ? directory.path() : workingDirectory);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        process.start(QStringLiteral(HARKEN_HUB_CLI_FIXTURE), QStringList{"--cli"} + arguments);
        if (!process.waitForStarted()) { ADD_FAILURE() << process.errorString().toStdString(); return {}; }
        timeout.start(30000);
        if (process.state() != QProcess::NotRunning) loop.exec();
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
            ADD_FAILURE() << "CLI did not finish: " << arguments.join(' ').toStdString();
            return {};
        }
        const auto output = process.readAllStandardOutput();
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(output, &error);
        EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
        EXPECT_EQ(error.error, QJsonParseError::NoError) << output.toStdString();
        EXPECT_EQ(process.exitCode(), expected) << output.toStdString() << process.readAllStandardError().toStdString();
        EXPECT_EQ(document.object()["schemaVersion"].toInt(), 1);
        return document.object();
    }

    void write(const QString& path, const QByteArray& bytes)
    {
        ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(bytes), bytes.size());
    }

    QByteArray read(const QString& path)
    {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    }

    QJsonObject approve(const QStringList& arguments)
    {
        const auto plan = run(arguments, 4);
        const auto token = plan["confirmation"].toString();
        EXPECT_FALSE(token.isEmpty());
        return run(arguments + QStringList{"--confirm", token});
    }
};

TEST_F(HubCliTest, EveryPublishedCommandRejectsMalformedInputBeforeExecutionWithoutAGui)
{
    const auto reference = run({"help"})["data"].toObject();
    const auto commands = reference["commands"].toArray();
    ASSERT_FALSE(commands.isEmpty());
    EXPECT_FALSE(QJsonDocument(reference).toJson().contains("Unreal"));
    EXPECT_EQ(run({"about"})["data"].toObject()["product"].toString(), "HarkenHub");
    for (const auto& value : commands) {
        const auto command = value.toObject();
        const auto arguments = command["command"].toString().split(' ');
        SCOPED_TRACE(arguments.join(' ').toStdString());
        run(arguments + QStringList{"--unknown-option", "value"}, 2);
        const auto parameters = command["parameters"].toObject();
        bool required = false;
        for (const auto& parameter : parameters) required |= parameter.toObject()["required"].toBool();
        if (required) run(arguments, 2);
        if (!parameters.isEmpty()) {
            const auto name = "--" + parameters.begin().key();
            run(arguments + QStringList{name}, 2);
            run(arguments + QStringList{name, "one", name, "two"}, 2);
        }
    }
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("user/settings.json")));
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("user/consents.json")));

}

TEST_F(HubCliTest, PersistentChoicesAndRegistrationRemovalDoNotModifyProjectFiles)
{
    const auto project = directory.filePath(QString::fromUtf8("Renamed project.hsp"));
    const QByteArray contents = R"({"harkenDocument":{"type":"project.root"},"metadata":{"name":"Previous name"}})";
    write(project, contents);
    run({"projects", "add", "--path", project});
    const auto entries = run({"projects", "list"})["data"].toObject()["projects"].toArray();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].toObject()["name"].toString(), "Renamed project");
    const auto blockedMachine = directory.filePath("blocked-machine");
    write(blockedMachine, "file");
    environment.insert("HARKEN_HUB_MACHINE_ROOT", blockedMachine);
    const auto removed = approve({"projects", "remove", "--path", project});
    EXPECT_FALSE(removed["data"].toObject()["warnings"].toArray().isEmpty());
    EXPECT_EQ(read(project), contents);
    EXPECT_TRUE(run({"projects", "list"})["data"].toObject()["projects"].toArray().isEmpty());
    run({"settings", "set", "--key", "theme", "--value", "dark"});
    run({"settings", "set", "--key", "theme", "--value", "unsupported"}, 2);
    EXPECT_EQ(run({"settings", "get"})["data"].toObject()["theme"].toString(), "dark");
    for (const auto& product : {QStringLiteral("hub"), QStringLiteral("studio")}) {
        run({"legal", "show", "--product", product});
        approve({"legal", "accept", "--product", product});
    }
    const auto consents = run({"legal", "status"})["data"].toObject();
    EXPECT_TRUE(consents["hub"].toBool());
    EXPECT_TRUE(consents["studio"].toBool());
}

TEST_F(HubCliTest, ConcurrentClientsPreserveIndependentConsentsAndBuildRegistrations)
{
    std::vector<QStringList> commands;
    for (const auto* product : {"hub", "studio"}) {
        const QStringList command{"legal", "accept", "--product", product};
        const auto review = run(command, 4);
        commands.push_back(command + QStringList{"--confirm", review["confirmation"].toString()});
    }
    for (int i = 0; i < 4; ++i) {
        const auto path = directory.filePath(QStringLiteral("studio-%1").arg(i)
            + QFileInfo(QString::fromUtf8(HARKEN_HUB_STUDIO_FIXTURE)).suffix().prepend('.'));
        ASSERT_TRUE(QFile::copy(QString::fromUtf8(HARKEN_HUB_STUDIO_FIXTURE), path));
        commands.push_back({"versions", "add-local", "--path", path});
    }
    std::vector<std::unique_ptr<QProcess>> clients;
    for (const auto& command : commands) {
        auto client = std::make_unique<QProcess>();
        client->setProcessEnvironment(environment);
        client->start(QString::fromUtf8(HARKEN_HUB_CLI_FIXTURE), QStringList{"--cli"} + command);
        ASSERT_TRUE(client->waitForStarted());
        clients.push_back(std::move(client));
    }
    for (const auto& client : clients) {
        if (client->state() != QProcess::NotRunning) ASSERT_TRUE(client->waitForFinished(15000));
        EXPECT_EQ(client->exitCode(), 0) << client->readAllStandardOutput().toStdString() << client->readAllStandardError().toStdString();
    }
    const auto accepted = run({"legal", "status"})["data"].toObject();
    EXPECT_TRUE(accepted["hub"].toBool());
    EXPECT_TRUE(accepted["studio"].toBool());
    EXPECT_EQ(run({"versions", "list"})["data"].toObject()["versions"].toArray().size(), 4);
}

TEST_F(HubCliTest, LogTailCountsRecordsWithAndWithoutATerminatingNewline)
{
    const auto path = directory.filePath("user/logs/hub-gui.log");
    for (const auto& bytes : {QByteArray("first\nlast\n"), QByteArray("first\nlast")}) {
        write(path, bytes);
        EXPECT_EQ(run({"diagnostics", "logs", "--lines", "1"})["data"].toObject()["logs"].toArray()[0].toObject()["lines"].toArray(), QJsonArray{"last"});
        EXPECT_EQ(run({"diagnostics", "logs", "--lines", "5"})["data"].toObject()["logs"].toArray()[0].toObject()["lines"].toArray(), (QJsonArray{"first", "last"}));
    }
    write(path, {});
    EXPECT_TRUE(run({"diagnostics", "logs"})["data"].toObject()["logs"].toArray()[0].toObject()["lines"].toArray().isEmpty());
    ASSERT_TRUE(QFile::remove(path));
    write(directory.filePath("user/logs/hub-background.log"), "update completed\n");
    const auto logs = run({"diagnostics", "logs"})["data"].toObject()["logs"].toArray();
    EXPECT_FALSE(logs[0].toObject()["exists"].toBool());
    EXPECT_EQ(logs[1].toObject()["lines"].toArray(), QJsonArray{"update completed"});
}

TEST_F(HubCliTest, FileReviewCannotBeReusedFromAnotherWorkingDirectory)
{
    const QStringList command{"diagnostics", "export", "--path", "logs.zip"};
    const auto original = run(command, 4);
    ASSERT_TRUE(QDir().mkpath(directory.filePath("other")));
    const auto changed = run(command + QStringList{"--confirm", original["confirmation"].toString()}, 4, directory.filePath("other"));
    EXPECT_NE(original["confirmation"], changed["confirmation"]);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("logs.zip")));
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("other/logs.zip")));
    EXPECT_EQ(changed["plan"].toObject()["path"].toString(), directory.filePath("other/logs.zip"));
}

TEST_F(HubCliTest, ProjectDispatchResolvesCatalogChoicesAndConsumesSuccessfulStudioIntake)
{
    const auto version = run({"versions", "add-local", "--path", QString::fromUtf8(HARKEN_HUB_STUDIO_FIXTURE)})["data"].toObject()["buildId"].toString();
    ASSERT_FALSE(version.isEmpty());
    const auto templates = run({"projects", "templates", "--build", version})["data"].toObject()["templates"].toArray();
    ASSERT_EQ(templates.size(), 1);
    const auto filePath = directory.filePath("not-a-directory.txt");
    write(filePath, "file");
    approve({"legal", "accept", "--product", "studio"});
    for (const auto& parent : {directory.filePath("missing"), filePath}) {
        run({"projects", "create", "--build", version, "--name", "Demo", "--location", parent}, 2);
        EXPECT_FALSE(QFileInfo::exists(parent + "/Demo"));
    }
    const QStringList create{"projects", "create", "--build", version, "--name", "Demo", "--location", directory.path()};
    run(create + QStringList{"--template", "missing"}, 2);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("Demo")));
    write(directory.filePath("Demo.scenario.json"), "{}");
    run(create + QStringList{"--template", templates[0].toObject()["id"].toString()});
    const auto project = directory.filePath("Demo/Demo.hsp");
    const auto launchPath = project + ".launch.json";
    QElapsedTimer timer;
    timer.start();
    while (!QFileInfo::exists(launchPath) && timer.elapsed() < 10000) QThread::msleep(10);
    const auto launch = QJsonDocument::fromJson(read(launchPath)).object();
    const auto pid = launch["pid"].toInteger();
    const auto stop = qScopeGuard([&] { if (pid > 0) harken::hub::terminateProcessById(pid); });
    ASSERT_GT(pid, 0);
    const auto arguments = launch["arguments"].toArray().toVariantList();
    const auto templateIndex = arguments.indexOf(QStringLiteral("--project-template-id"));
    ASSERT_GE(templateIndex, 0);
    EXPECT_EQ(arguments.value(templateIndex + 1).toString(), QStringLiteral("audio"));
    EXPECT_FALSE(arguments.contains(QStringLiteral("--project-template-file")));
    write(project, R"({"harkenDocument":{"type":"project.root"},"metadata":{"name":"Old name"}})");
    const auto intakePath = directory.filePath("user/project-intake/created.json");
    const auto openedAt = QDateTime::currentDateTimeUtc().addSecs(1).toString(Qt::ISODateWithMs);
    write(intakePath, QJsonDocument(QJsonObject{{"projectPath", project}, {"buildId", "run1.aaaa1111"}, {"openedAt", openedAt}}).toJson());
    const auto projects = run({"projects", "list"})["data"].toObject()["projects"].toArray();
    ASSERT_EQ(projects.size(), 1);
    EXPECT_EQ(projects[0].toObject()["name"].toString(), "Demo");
    EXPECT_EQ(projects[0].toObject()["lastOpenedAt"].toString(), openedAt);
    EXPECT_EQ(projects[0].toObject()["preferredBuild"].toString(), version);
    EXPECT_FALSE(QFileInfo::exists(intakePath));
}

TEST_F(HubCliTest, InstalledMetadataCannotValidateItselfAndFileChangesInvalidateRemovalReview)
{
    const auto installation = directory.filePath("versions/build");
    const auto hash = [](const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const QByteArray binary("original");
    QJsonObject manifest{{"product", "HarkenStudio"}, {"buildId", "build"}, {"version", "1.0.0"}, {"channel", "stable"},
        {"files", QJsonArray{QJsonObject{{"path", "payload.bin"}, {"size", binary.size()}, {"sha256", hash(binary)}},
            QJsonObject{{"path", harken::hub::studioBinaryRelativePath()}, {"size", binary.size()}, {"sha256", hash(binary)}}}}};
    const auto manifestBytes = QJsonDocument(manifest).toJson();
    write(installation + "/package-manifest.json", manifestBytes);
    const auto capabilities = QJsonDocument(HarkenHubTest::studioCapabilities("build", "1.0.0")).toJson();
    write(installation + "/capabilities.json", capabilities);
    write(installation + '/' + harken::hub::studioBinaryRelativePath(), binary);
    write(installation + "/payload.bin", binary);
    const QJsonObject record{{"buildId", "build"}, {"semanticVersion", "1.0.0"}, {"channel", "stable"},
        {"installPath", installation}, {"packageHash", "unused"}, {"packageManifestHash", hash(manifestBytes)},
        {"capabilitiesHash", hash(capabilities)}, {"installedAt", "2026-01-01"}, {"healthState", "Installed"}};
    write(directory.filePath("machine/install-index.json"), QJsonDocument(QJsonObject{{"lastReleaseSequence", 1}, {"installed", QJsonArray{record}}}).toJson());
    run({"versions", "verify", "--build", "build"});
    write(installation + "/payload.bin", "modified");
    approve({"legal", "accept", "--product", "studio"});
    const auto create = run({"projects", "create", "--build", "build", "--name", "Rejected", "--location", directory.path()}, 3);
    EXPECT_EQ(create["error"].toObject()["code"].toString(), "VersionNeedsRepair");
    EXPECT_FALSE(QDir(directory.filePath("Rejected")).exists());
    const auto review = run({"versions", "uninstall", "--build", "build"}, 4);
    write(installation + "/payload.bin", "modified again");
    run({"versions", "uninstall", "--build", "build", "--confirm", review["confirmation"].toString()}, 4);
    manifest["files"] = QJsonArray{QJsonObject{{"path", "payload.bin"}, {"size", 14}, {"sha256", hash("modified again")}}};
    write(installation + "/package-manifest.json", QJsonDocument(manifest).toJson());
    const auto invalid = run({"versions", "verify", "--build", "build"}, 3);
    EXPECT_TRUE(invalid["data"].toObject()["modified"].toArray().contains("package-manifest.json"));
    EXPECT_EQ(read(installation + "/payload.bin"), "modified again");
}

TEST_F(HubCliTest, AutomaticPreferencesNotifyTheRunningServiceWithoutOpeningTheGui)
{
    const auto previousRoot = qgetenv("HARKEN_HUB_USER_ROOT");
    qputenv("HARKEN_HUB_USER_ROOT", environment.value("HARKEN_HUB_USER_ROOT").toUtf8());
    const auto restore = qScopeGuard([&] {
        if (previousRoot.isNull()) qunsetenv("HARKEN_HUB_USER_ROOT");
        else qputenv("HARKEN_HUB_USER_ROOT", previousRoot);
    });
    harken::hub::SingleInstanceGuard service(harken::hub::SingleInstanceGuard::backgroundServerName());
    ASSERT_TRUE(service.isPrimary());
    int notifications = 0;
    QObject::connect(&service, &harken::hub::SingleInstanceGuard::activationRequested, &service, [&](const QStringList& arguments) {
        EXPECT_EQ(arguments, QStringList{"--settings-changed"});
        ++notifications;
    });
    approve({"legal", "accept", "--product", "hub"});
    for (const auto* key : {"auto-hub-update", "auto-studio-download"})
        for (const auto* value : {"false", "true"}) {
            const auto result = run({"settings", "set", "--key", key, "--value", value})["data"].toObject();
            EXPECT_TRUE(result["backgroundNotified"].toBool());
            EXPECT_EQ(result[key].toBool(), QString::fromLatin1(value) == "true");
        }
    const auto language = run({"settings", "set", "--key", "language", "--value", "zh_CN"});
    EXPECT_TRUE(language["data"].toObject()["backgroundNotified"].toBool());
    EXPECT_TRUE(QTest::qWaitFor([&] { return notifications == 5; }, 5000)) << notifications;
}

TEST_F(HubCliTest, UpdateApplicationRejectsActiveTransactionsAndUntrustedFilesBeforeConsentOrElevation)
{
    const auto keys = HarkenHubTest::makeKeypair();
    environment.insert("HARKEN_HUB_MANIFEST_PUBKEY", QString::fromLatin1(keys.publicKey.toHex()));
    const QByteArray package("test payload");
    auto payload = HarkenHubTest::baseManifestPayload();
    payload["packageSize"] = package.size();
    payload["sha256"] = QString::fromLatin1(QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex());
    const auto envelope = HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(QJsonDocument::Compact), keys);
    const auto packagePath = directory.filePath("download.zip");
    const auto manifestPath = directory.filePath("release.manifest");
    write(packagePath, package);
    write(manifestPath, envelope);
    QJsonObject state{{"state", "AwaitingUserInstall"}, {"version", payload["version"]}, {"buildId", payload["buildId"]},
        {"downloadPath", packagePath}, {"manifestEnvelopePath", manifestPath}, {"expectedSha256", payload["sha256"]},
        {"packageSize", package.size()}, {"releaseSequence", payload["releaseSequence"]}};
    const auto statePath = directory.filePath("user/update-state.json");
    const auto save = [&] { write(statePath, QJsonDocument(QJsonObject{{"HarkenStudio", state}}).toJson()); };
    save();
    run({"updates", "apply", "--product", "studio"}, 3);
    for (const auto* busy : {"AwaitingBridge", "AwaitingElevation", "Downloading", "Verifying"}) {
        state["state"] = busy;
        save();
        const auto bytes = read(statePath);
        for (const auto* operation : {"apply", "download"}) {
            const auto result = run({"updates", operation, "--product", "studio"}, 3);
            EXPECT_EQ(result["error"].toObject()["code"].toString(), "UpdateBusy");
            EXPECT_EQ(read(statePath), bytes);
        }
    }
    state["state"] = "AwaitingUserInstall";
    save();
    write(packagePath, "tampered bytes");
    run({"updates", "apply", "--product", "studio"}, 3);
    write(packagePath, package);
    write(manifestPath, HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(QJsonDocument::Compact), HarkenHubTest::makeKeypair(1)));
    run({"updates", "apply", "--product", "studio"}, 3);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("user/consents.json")));
    payload["product"] = "HarkenHub";
    write(manifestPath, HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(QJsonDocument::Compact), keys));
    state["state"] = "AwaitingRestart";
    write(statePath, QJsonDocument(QJsonObject{{"HarkenHub", state}}).toJson());
    approve({"legal", "accept", "--product", "hub"});
    environment.insert("HARKEN_HUB_BRIDGE_DISABLE", "1");
    const auto previousRoot = qgetenv("HARKEN_HUB_USER_ROOT");
    qputenv("HARKEN_HUB_USER_ROOT", directory.filePath("user").toUtf8());
    const auto reset = qScopeGuard([&] {
        if (previousRoot.isNull()) qunsetenv("HARKEN_HUB_USER_ROOT");
        else qputenv("HARKEN_HUB_USER_ROOT", previousRoot);
    });
    harken::hub::SingleInstanceGuard background(harken::hub::SingleInstanceGuard::backgroundServerName());
    ASSERT_TRUE(background.isPrimary());
    int messages = 0;
    QObject::connect(&background, &harken::hub::SingleInstanceGuard::activationRequested, &background, [&](const QStringList&) { ++messages; });
    const QStringList apply{"updates", "apply", "--product", "hub"};
    const auto review = run(apply, 4);
    const auto failed = run(apply + QStringList{"--confirm", review["confirmation"].toString()}, 3);
    EXPECT_TRUE(failed["error"].toObject()["message"].toString().contains("bridge"));
    EXPECT_EQ(messages, 0);
}

TEST_F(HubCliTest, NetworkDownloadsRequireBothSignedIdentityAndValidArchiveContents)
{
    HarkenHubTest::RangeHttpServer server;
    ASSERT_TRUE(server.listen());
    environment.insert("HARKEN_CONTENT_BASE_URL", server.baseUrl().toString());
    HarkenHubTest::RangeHttpServer primary;
    ASSERT_TRUE(primary.listen());
    HarkenHubTest::RangeHttpServer::Script unavailable;
    unavailable.mode = HarkenHubTest::RangeHttpServer::Script::Mode::Status;
    unavailable.status = 503;
    primary.setDefaultScript(unavailable);
    environment.insert("HARKEN_HUB_GITHUB_BASE_URL", primary.baseUrl().toString());
    const auto keys = HarkenHubTest::makeKeypair();
    environment.insert("HARKEN_HUB_MANIFEST_PUBKEY", QString::fromLatin1(keys.publicKey.toHex()));
    const auto hash = [](const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const QByteArray binary("studio fixture");
    const QString version("1.4.0"), build("run12.abcd1234");
    const auto binaryPath = harken::hub::studioBinaryRelativePath().toUtf8();
    const QJsonObject manifest{{"product", "HarkenStudio"}, {"version", version}, {"buildId", build}, {"channel", "stable"},
        {"files", QJsonArray{QJsonObject{{"path", QString::fromUtf8(binaryPath)}, {"size", binary.size()}, {"sha256", hash(binary)}}}}};
    const QJsonObject capabilities{{"studioVersion", version}, {"buildId", build}, {"capabilitiesVersion", 2}};
    const auto source = directory.filePath("source.zip");
    ASSERT_TRUE(HarkenHubTest::writeArchive(source, {{binaryPath, binary},
        {"package-manifest.json", QJsonDocument(manifest).toJson()}, {"capabilities.json", QJsonDocument(capabilities).toJson()}}));
    const auto validBytes = read(source);
    auto payload = HarkenHubTest::baseManifestPayload();
    const auto publish = [&](const QByteArray& bytes, const HarkenHubTest::TestKeypair& signer) {
        payload["sha256"] = hash(bytes);
        payload["packageSize"] = bytes.size();
        const auto envelope = HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(QJsonDocument::Compact), signer);
        HarkenHubTest::RangeHttpServer::Script response;
        response.mode = HarkenHubTest::RangeHttpServer::Script::Mode::Status;
        response.status = 200;
        response.responseBody = HarkenHubTest::latestResponseJson(envelope);
        server.enqueue(response);
        server.setPayload(bytes, hash(bytes).toLatin1());
    };
    publish(validBytes, keys);
    const auto result = run({"versions", "download", "--build", build});
    EXPECT_EQ(read(result["data"].toObject()["package"].toString()), validBytes);
    const auto installation = run({"versions", "install", "--package", result["data"].toObject()["package"].toString(),
        "--manifest", result["data"].toObject()["manifest"].toString()}, 4)["plan"].toObject()["installation"].toObject();
    EXPECT_TRUE(installation["files"].toObject().contains(QString::fromUtf8(binaryPath)));
    EXPECT_TRUE(installation["destination"].toString().startsWith(directory.filePath("versions/")));
    publish(validBytes, HarkenHubTest::makeKeypair(7));
    const auto requests = server.requestCount();
    run({"versions", "download", "--build", build}, 3);
    EXPECT_EQ(server.requestCount(), requests + 1);
    publish("signed but not an archive", keys);
    run({"versions", "download", "--build", build}, 3);
    EXPECT_FALSE(QFileInfo::exists(directory.filePath("machine/install-index.json")));
}

TEST_F(HubCliTest, IntegrationLifecycleReplacesTheInstallationDirectory)
{
    const auto game = directory.filePath("Game");
    const auto source = directory.filePath("Source");
    for (const auto* folder : {"Assets", "Packages", "ProjectSettings"}) ASSERT_TRUE(QDir().mkpath(game + '/' + folder));
    write(source + "/package.json", R"({"name":"com.harken.core","version":"0.2.0-test"})");
    write(source + "/Runtime/Generated/HarkenAbi.g.cs", "fixture content");
    const auto project = directory.filePath("Audio.hsp");
    write(project, R"({"harkenDocument":{"type":"project.root"}})");
    approve({"integrations", "link", "--path", game, "--project", project, "--source", "./Source"});
    const QStringList install{"integrations", "install", "--path", game, "--source", "./Source"};
    const auto installed = game + "/Packages/com.harken.core";
    run(install, 4);
    EXPECT_FALSE(QFileInfo::exists(installed));
    approve(install);
    EXPECT_EQ(read(installed + "/Runtime/Generated/HarkenAbi.g.cs"), "fixture content");
    write(installed + "/ProjectOnly.txt", "my own content");
    write(source + "/package.json", R"({"name":"com.harken.core","version":"0.3.0-test"})");
    const auto upgrade = run(install, 4)["plan"].toObject();
    EXPECT_EQ(upgrade["installedVersion"].toString(), "0.2.0-test");
    EXPECT_EQ(upgrade["sourceVersion"].toString(), "0.3.0-test");
    approve(install);
    EXPECT_FALSE(QFileInfo::exists(installed + "/ProjectOnly.txt"));
    EXPECT_EQ(run({"integrations", "list"})["data"].toObject()["associations"].toArray()[0].toObject()["source"].toString(), source);
    EXPECT_EQ(run({"integrations", "locate", "--path", game})["data"].toObject()["installedVersion"].toString(), "0.3.0-test");
    run({"integrations", "locate", "--path", game, "--location", "Assets/Wrong"}, 3);
    run({"integrations", "restore", "--path", game}, 3);
    run({"integrations", "remove", "--path", game}, 4);
    approve({"integrations", "remove", "--path", game});
    EXPECT_FALSE(QFileInfo::exists(installed));
}

}
