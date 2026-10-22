#include "StudioProtocolTestUtils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QTextStream>
#include <QThread>
#include <QTimer>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    const auto argument = [&arguments](const QString& key) {
        const int index = arguments.indexOf(key);
        return index < 0 ? QString() : arguments.value(index + 1);
    };
    if (arguments.contains(QStringLiteral("--hold-lock"))) {
        QLockFile lock(argument(QStringLiteral("--hold-lock")));
        if (!lock.tryLock())
            return 2;
        QTextStream(stdout) << "locked" << Qt::endl;
        return app.exec();
    }
    if (arguments.contains(QStringLiteral("--self-test"))) {
        QTextStream(stdout) << QJsonDocument(HarkenHubTest::studioSelfTest()).toJson(QJsonDocument::Compact);
        return 0;
    }
    const bool analyze = arguments.contains(QStringLiteral("--analyze-project-open"));
    const bool create = arguments.contains(QStringLiteral("--create-project"));
    const QString projectName = argument(QStringLiteral("--project-name"));
    const QDir projectLocation(argument(QStringLiteral("--project-location")));
    const auto projectPath = create
        ? projectLocation.filePath(projectName + "/" + projectName + ".hsp")
        : argument(analyze ? QStringLiteral("--analyze-project-open")
                                              : QStringLiteral("--project"));
    QFile configFile(create ? projectLocation.filePath(projectName + ".scenario.json")
                            : projectPath + QStringLiteral(".scenario.json"));
    if (!configFile.open(QIODevice::ReadOnly))
        return 2;
    const auto config = QJsonDocument::fromJson(configFile.readAll()).object();
    const bool upgrade = config.value(QStringLiteral("upgrade")).toBool();
    if (analyze) {
        if (config.value(QStringLiteral("legacy")).toBool()) {
            QFile unexpected(projectPath + QStringLiteral(".unexpected-analysis"));
            unexpected.open(QIODevice::WriteOnly);
            return 4;
        }
        QThread::msleep(config.value(QStringLiteral("analysisDelayMs")).toInt());
        const auto mode = config.value(QStringLiteral("analysis")).toString();
        if (mode == QStringLiteral("invalid")) {
            QTextStream(stdout) << "not a report";
            return 0;
        }
        QJsonObject report{
            {QStringLiteral("protocolVersion"), 1},
            {QStringLiteral("product"), QStringLiteral("HarkenStudio")},
            {QStringLiteral("version"), QStringLiteral("1.4.0")},
            {QStringLiteral("buildId"), QStringLiteral("run1.aaaa1111")},
            {QStringLiteral("requestId"), argument(QStringLiteral("--project-open-request-id"))},
            {QStringLiteral("projectPath"), projectPath},
            {QStringLiteral("projectId"), QStringLiteral("11111111-2222-3333-4444-555555555555")},
            {QStringLiteral("ok"), mode != QStringLiteral("failed")},
            {QStringLiteral("errorCode"), mode == QStringLiteral("failed")
                ? config.value(QStringLiteral("errorCode")).toString(QStringLiteral("PROJECT_REQUIRES_NEWER_STUDIO")) : QString()},
            {QStringLiteral("technicalDetails"), config.value(QStringLiteral("technicalDetails")).toString()},
            {QStringLiteral("currentSchema"), upgrade ? 1 : 2},
            {QStringLiteral("targetSchema"), 2},
            {QStringLiteral("upgradeRequired"), upgrade},
            {QStringLiteral("migrationStepCount"), upgrade ? 1 : 0},
            {QStringLiteral("affectedFileCount"), upgrade ? 1 : 0},
            {QStringLiteral("requiredProjectBytes"), 1024},
            {QStringLiteral("requiredRecoveryBytes"), 0},
            {QStringLiteral("recoveryRoot"), QString()},
            {QStringLiteral("protection"), QStringLiteral("atomicInPlace")},
            {QStringLiteral("approvalDigest"), QString(64, QLatin1Char('a'))},
        };
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact);
        return mode == QStringLiteral("failed") ? 1 : 0;
    }
    if (create && !QDir().mkpath(QFileInfo(projectPath).absolutePath()))
        return 3;
    QFile launchLog(projectPath + QStringLiteral(".launch.json"));
    if (!launchLog.open(QIODevice::WriteOnly))
        return 3;
    launchLog.write(QJsonDocument(QJsonObject{
        {QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)},
        {QStringLiteral("pid"), app.applicationPid()},
    }).toJson());
    launchLog.close();
    QLocalSocket socket;
    const auto send = [&](QJsonObject message) {
        message[QStringLiteral("token")] = argument(QStringLiteral("--hub-token"));
        socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
    };
    const auto stage = [&](const QString& name, const QString& error = {}) {
        send({{QStringLiteral("type"), QStringLiteral("stage")},
              {QStringLiteral("stage"), name},
              {QStringLiteral("errorCode"), error},
              {QStringLiteral("technicalDetails"), config.value(QStringLiteral("failureDetails")).toString()},
              {QStringLiteral("error"), config.value(QStringLiteral("errorMessage")).toString(error)}});
    };
    QObject::connect(&socket, &QLocalSocket::connected, &app, [&]() {
        send({{QStringLiteral("type"), QStringLiteral("hello")},
              {QStringLiteral("protocolVersion"), 2},
              {QStringLiteral("pid"), app.applicationPid()},
              {QStringLiteral("buildId"), QStringLiteral("run1.aaaa1111")},
              {QStringLiteral("operation"), create ? QStringLiteral("createProject") : QStringLiteral("openProject")},
              {QStringLiteral("launchIntent"), argument(QStringLiteral("--launch-intent"))}});
        stage(QStringLiteral("processStarted"));
        const auto mode = config.value(QStringLiteral("launch")).toString();
        if (mode == QStringLiteral("disconnect")) {
            QTimer::singleShot(100, &socket, &QLocalSocket::disconnectFromServer);
            return;
        }
        QTimer::singleShot(config.value(QStringLiteral("readyDelayMs")).toInt(100), &app, [&, mode]() {
            if (mode == QStringLiteral("cancel"))
                stage(QStringLiteral("failed"), QStringLiteral("PROJECT_OPEN_CANCELLED"));
            else if (mode == QStringLiteral("failed"))
                stage(QStringLiteral("failed"), config.value(QStringLiteral("errorCode"))
                    .toString(QStringLiteral("PROJECT_REQUIRES_NEWER_STUDIO")));
            else if (mode == QStringLiteral("forwarded")) {
                stage(QStringLiteral("forwarded"));
                return;
            } else {
                if (create && !config.value(QStringLiteral("missingProjectFile")).toBool()) {
                    QFile project(projectPath);
                    if (!project.open(QIODevice::WriteOnly))
                        return;
                    project.write(QJsonDocument(QJsonObject{{"harkenDocument", QJsonObject{
                        {"type", "project.root"}, {"schemaVersion", 2},
                        {"id", "11111111-2222-3333-4444-555555555555"},
                        {"projectId", "11111111-2222-3333-4444-555555555555"},
                        {"savedStudioVersion", "1.4.0"}}}}).toJson());
                }
                stage(QStringLiteral("ready"));
            }
            if (config.value(QStringLiteral("exitAfterResult")).toBool()) {
                socket.waitForBytesWritten(1000);
                app.quit();
            }
        });
    });
    QObject::connect(&socket, &QLocalSocket::readyRead, &app, [&]() {
        while (socket.canReadLine()) {
            const auto message = QJsonDocument::fromJson(socket.readLine()).object();
            if (message.value(QStringLiteral("type")).toString() == QStringLiteral("forwardedAccepted")
                && message.value(QStringLiteral("token")).toString() == argument(QStringLiteral("--hub-token")))
                app.quit();
        }
    });
    QTimer::singleShot(config.value(QStringLiteral("helloDelayMs")).toInt(), &socket, [&]() {
        socket.connectToServer(argument(QStringLiteral("--hub-session")));
    });
    QTimer::singleShot(config.value(QStringLiteral("readyDelayMs")).toInt(100) + 10000, &app, &QCoreApplication::quit);
    return app.exec();
}
