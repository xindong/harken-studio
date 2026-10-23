#include "Launch/StudioLaunchMonitor.h"

#include "Platform/ProcessScan.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QRandomGenerator>

namespace VNH {

namespace {

QString hubText(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

QString randomHexToken()
{
    QString token;
    token.reserve(32);
    for (int i = 0; i < 4; ++i)
        token += QStringLiteral("%1").arg(
            QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
    return token;
}

const QStringList kKnownStages = {
    QStringLiteral("processStarted"), QStringLiteral("projectRead"),
    QStringLiteral("projectCreated"), QStringLiteral("cacheRebuildStarted"),
};

}

StudioLaunchMonitor::StudioLaunchMonitor(QObject* parent)
    : QObject(parent)
{
    connect(&server_, &QLocalServer::newConnection, this,
            &StudioLaunchMonitor::handleNewConnection);
}

bool StudioLaunchMonitor::listen()
{
    sessionToken_ = randomHexToken();
    serverName_ = QStringLiteral("vnl-m-") + randomHexToken().left(16);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    return server_.listen(serverName_);
}

void StudioLaunchMonitor::close()
{
    if (client_) {
        client_->disconnect(this);
        client_->close();
        client_ = nullptr;
    }
    server_.close();
}

void StudioLaunchMonitor::setExpectedBuildId(const QString& buildId)
{
    expectedBuildId_ = buildId;
}

void StudioLaunchMonitor::setExpectedInstallPath(const QString& installPath)
{
    expectedInstallPath_ = QFileInfo(installPath).canonicalFilePath();
}

void StudioLaunchMonitor::setExpectedProcess(qint64 pid, const QString& executablePath)
{
    expectedPid_ = pid;
    expectedExecutablePath_ = QFileInfo(executablePath).canonicalFilePath();
}

void StudioLaunchMonitor::setExpectedOperation(const QString& operation)
{
    expectedOperation_ = operation;
}

void StudioLaunchMonitor::setExpectedLaunchIntent(const QString& intent)
{
    expectedLaunchIntent_ = intent;
}

void StudioLaunchMonitor::handleNewConnection()
{
    QLocalSocket* connection = server_.nextPendingConnection();
    if (!connection)
        return;
    if (client_) {
        connection->close();
        connection->deleteLater();
        return;
    }
    client_ = connection;
    client_->setParent(this);
    connect(client_, &QLocalSocket::readyRead, this,
            &StudioLaunchMonitor::handleReadyRead);
    connect(client_, &QLocalSocket::disconnected, this,
            [this, connection]() {
                if (client_ != connection)
                    return;
                if (connection->bytesAvailable() > 0)
                    handleReadyRead();
                if (client_ != connection)
                    return;
                client_ = nullptr;
                connection->deleteLater();
                if (!finished_) {
                    finished_ = true;
                    emit failed(
                        helloAccepted_
                            ? hubText("Connection to HarkenStudio was lost "
                                      "before launch finished")
                            : hubText("HarkenStudio disconnected before "
                                      "completing the launch handshake"));
                }
            });
}

void StudioLaunchMonitor::rejectClient(const QString& error)
{
    if (client_) {
        client_->disconnect(this);
        client_->close();
        client_->deleteLater();
        client_ = nullptr;
    }
    if (!finished_) {
        finished_ = true;
        emit failed(error);
    }
}

void StudioLaunchMonitor::handleReadyRead()
{
    if (!client_)
        return;
    buffer_ += client_->readAll();
    if (buffer_.size() > kMaxMessageBytes) {
        rejectClient(hubText("Launch report exceeded the size limit"));
        return;
    }
    int newline;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline);
        buffer_.remove(0, newline + 1);
        if (!line.trimmed().isEmpty())
            processMessage(line);
        if (!client_)
            return;
    }
}

void StudioLaunchMonitor::processMessage(const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        rejectClient(hubText("Launch report is not valid JSON"));
        return;
    }
    const QJsonObject message = doc.object();
    if (message[QStringLiteral("token")].toString() != sessionToken_) {
        rejectClient(hubText("Launch report token mismatch"));
        return;
    }

    const QString type = message[QStringLiteral("type")].toString();
    if (!helloAccepted_) {
        if (type != QStringLiteral("hello")) {
            rejectClient(hubText("Launch report arrived before hello"));
            return;
        }
        if (message[QStringLiteral("protocolVersion")] != kProtocolVersion) {
            rejectClient(hubText("Launch report protocol version mismatch"));
            return;
        }
        const QString reportedBuildId =
            message[QStringLiteral("buildId")].toString();
        if (!expectedBuildId_.isEmpty() &&
            reportedBuildId != expectedBuildId_) {
            rejectClient(hubText("HarkenStudio reported build id %1, expected %2")
                             .arg(reportedBuildId, expectedBuildId_));
            return;
        }
        const QString reportedOperation =
            message[QStringLiteral("operation")].toString();
        if (!expectedOperation_.isEmpty()) {
            if (reportedOperation != expectedOperation_) {
                rejectClient(expectedOperation_ ==
                                     QStringLiteral("createProject")
                                 ? hubText("This HarkenStudio version cannot "
                                           "create projects from Harken Studio Hub. "
                                           "Update HarkenStudio and try again.")
                                 : hubText("HarkenStudio reported an unexpected "
                                           "launch operation"));
                return;
            }
        }
        const QString reportedIntent =
            message[QStringLiteral("launchIntent")].toString();
        if (reportedIntent.isEmpty()
            || (!expectedLaunchIntent_.isEmpty()
                && reportedIntent != expectedLaunchIntent_)) {
            rejectClient(hubText("HarkenStudio reported an invalid launch intent"));
            return;
        }
        if (expectedPid_ > 0 || !expectedInstallPath_.isEmpty()) {
            const qint64 pid =
                message[QStringLiteral("pid")].toInteger(-1);
            const QString processPath = QFileInfo(executablePathForPid(pid)).canonicalFilePath();
            if (pid <= 0 || processPath.isEmpty()
                || (expectedPid_ > 0 && (pid != expectedPid_
                    || QFileInfo(processPath) != QFileInfo(expectedExecutablePath_)))) {
                rejectClient(hubText("Launch report does not match the selected Studio process"));
                return;
            }
            const QString prefix =
                QDir::cleanPath(QDir(expectedInstallPath_).absolutePath()) +
                QStringLiteral("/");
            if (!expectedInstallPath_.isEmpty() &&
                !processPath.startsWith(prefix, Qt::CaseInsensitive)) {
                rejectClient(hubText("HarkenStudio process is not running from "
                                    "the selected version directory"));
                return;
            }
        }
        reportedOperation_ = reportedOperation;
        helloAccepted_ = true;
        return;
    }

    if (type != QStringLiteral("stage")) {
        rejectClient(hubText("Unknown launch report message"));
        return;
    }
    const QString stage = message[QStringLiteral("stage")].toString();
    if (finished_)
        return;
    if (stage == QStringLiteral("ready")) {
        finished_ = true;
        lastStage_ = stage;
        emit stageChanged(stage);
        emit ready();
        return;
    }
    if (stage == QStringLiteral("forwarded")) {
        if (reportedOperation_ != QStringLiteral("openProject")) {
            rejectClient(hubText("HarkenStudio reported an unexpected launch operation"));
            return;
        }
        finished_ = true;
        lastStage_ = stage;
        client_->write(QJsonDocument(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("forwardedAccepted")},
            {QStringLiteral("token"), sessionToken_},
        }).toJson(QJsonDocument::Compact) + '\n');
        client_->flush();
        emit stageChanged(stage);
        emit forwarded();
        return;
    }
    if (stage == QStringLiteral("failed")) {
        const QString error = message[QStringLiteral("error")].toString();
        failureCode_ = message[QStringLiteral("errorCode")].toString();
        failureDetails_ = message[QStringLiteral("technicalDetails")].toString();
        if (failureCode_.isEmpty()) {
            rejectClient(hubText(
                "HarkenStudio reported a failure without an error code"));
            return;
        }
        finished_ = true;
        lastStage_ = stage;
        emit stageChanged(stage);
        emit failed(error.isEmpty()
                        ? hubText("HarkenStudio reported a failure")
                        : error);
        return;
    }
    if (kKnownStages.contains(stage)) {
        lastStage_ = stage;
        emit stageChanged(stage);
        return;
    }
    rejectClient(hubText("Unknown launch stage: %1").arg(stage));
}

}
