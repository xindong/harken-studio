#include "HubClient.h"

#include "harken_buildinfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace VNX {

HubClient::Arguments HubClient::parseArguments(const QStringList& args)
{
    Arguments arguments;
    arguments.createProject = args.contains(QStringLiteral("--create-project"));
    for (int i = 1; i + 1 < args.size(); ++i) {
        const QString& key = args.at(i);
        const QString& value = args.at(i + 1);
        if (key == QStringLiteral("--hub-session"))
            arguments.serverName = value;
        else if (key == QStringLiteral("--hub-token"))
            arguments.sessionToken = value;
        else if (key == QStringLiteral("--expected-build-id"))
            arguments.expectedBuildId = value;
        else if (key == QStringLiteral("--project"))
            arguments.projectPath = value;
        else if (key == QStringLiteral("--launch-intent"))
            arguments.launchIntent = value;
        else if (key == QStringLiteral("--project-open-request-id"))
            arguments.projectOpenRequestId = value;
        else if (key == QStringLiteral("--project-open-approval"))
            arguments.projectOpenApproval = value;
        else if (key == QStringLiteral("--project-name"))
            arguments.projectName = value;
        else if (key == QStringLiteral("--project-location"))
            arguments.projectLocation = value;
        else if (key == QStringLiteral("--project-template-id"))
            arguments.projectTemplateId = value;
        else if (key == QStringLiteral("--project-template-file"))
            arguments.projectTemplatePath = value;
    }

    if (arguments.createProject && arguments.projectName.isEmpty()
        && arguments.projectLocation.isEmpty()) {
        const int createIndex = args.indexOf(QStringLiteral("--create-project"));
        if (createIndex + 1 < args.size()) {
            const QString path = QDir::cleanPath(args.at(createIndex + 1));
            if (!path.startsWith(QStringLiteral("--"))) {
                const QFileInfo projectDirectory(path);
                arguments.projectName = projectDirectory.fileName();
                arguments.projectLocation = projectDirectory.absoluteDir().absolutePath();
            }
        }
    }

    arguments.present = !arguments.serverName.isEmpty() &&
                        !arguments.sessionToken.isEmpty()
        && (arguments.launchIntent == QStringLiteral("DirectOpenOnly")
            || arguments.launchIntent
                == QStringLiteral("UpgradeApprovedByHub"));
    return arguments;
}

HubClient::HubClient(const Arguments& arguments, QObject* parent)
    : QObject(parent)
    , arguments_(arguments)
{
    connectionTimer_ = new QTimer(this);
    connectionTimer_->setSingleShot(true);
    forwardingTimer_ = new QTimer(this);
    forwardingTimer_->setSingleShot(true);
    connect(forwardingTimer_, &QTimer::timeout, this,
            [this]() { completeForwarding(false); });
    connect(connectionTimer_, &QTimer::timeout, this, [this]() {
        if (!connectionPending_)
            return;
        connectionPending_ = false;
        socket_.abort();
        emit helloCompleted(false);
    });
    connect(&socket_, &QLocalSocket::connected, this, [this]() {
        if (!connectionPending_)
            return;
        connectionPending_ = false;
        connectionTimer_->stop();
        sendHello();
        emit helloCompleted(true);
    });
    connect(&socket_, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) {
        if (connectionPending_) {
            connectionPending_ = false;
            connectionTimer_->stop();
            emit helloCompleted(false);
        }
        readForwardingResponse();
        completeForwarding(false);
    });
    connect(&socket_, &QLocalSocket::readyRead, this,
            &HubClient::readForwardingResponse);
    connect(&socket_, &QLocalSocket::disconnected, this, [this]() {
        readForwardingResponse();
        completeForwarding(false);
    });
}

void HubClient::connectAndHelloAsync(int timeoutMs)
{
    if (!arguments_.present || connectionPending_ || isConnected()) {
        QTimer::singleShot(0, this, [this]() {
            emit helloCompleted(isConnected() && helloSent_);
        });
        return;
    }
    connectionPending_ = true;
    connectionTimer_->start(qMax(1, timeoutMs));
    socket_.connectToServer(arguments_.serverName);
}

bool HubClient::isConnected() const
{
    return socket_.state() == QLocalSocket::ConnectedState;
}

void HubClient::sendHello()
{
    QJsonObject hello;
    hello[QStringLiteral("type")] = QStringLiteral("hello");
    hello[QStringLiteral("token")] = arguments_.sessionToken;
    hello[QStringLiteral("protocolVersion")] = 2;
    hello[QStringLiteral("pid")] =
        static_cast<double>(QCoreApplication::applicationPid());
    hello[QStringLiteral("buildId")] = QString::fromUtf8(
        Harken::BuildInfo::BuildId.data(),
        static_cast<int>(Harken::BuildInfo::BuildId.size()));
    hello[QStringLiteral("projectPath")] = arguments_.projectPath;
    hello[QStringLiteral("operation")] =
        arguments_.createProject ? QStringLiteral("createProject")
                                 : QStringLiteral("openProject");
    hello[QStringLiteral("launchIntent")] = arguments_.launchIntent;
    send(hello);
    helloSent_ = true;
}

void HubClient::reportStage(const QString& stage)
{
    if (!helloSent_ || !isConnected())
        return;
    QJsonObject message;
    message[QStringLiteral("type")] = QStringLiteral("stage");
    message[QStringLiteral("token")] = arguments_.sessionToken;
    message[QStringLiteral("stage")] = stage;
    send(message);
}

void HubClient::reportFailed(const QString& errorCode, const QString& error)
{
    if (!helloSent_ || !isConnected())
        return;
    QJsonObject message;
    message[QStringLiteral("type")] = QStringLiteral("stage");
    message[QStringLiteral("token")] = arguments_.sessionToken;
    message[QStringLiteral("stage")] = QStringLiteral("failed");
    message[QStringLiteral("errorCode")] = errorCode;
    message[QStringLiteral("error")] = error;
    send(message);
}

void HubClient::reportForwarded(int timeoutMs)
{
    if (forwardingPending_)
        return;
    forwardingPending_ = true;
    if (!helloSent_ || !isConnected() || arguments_.createProject) {
        QTimer::singleShot(0, this, [this]() { completeForwarding(false); });
        return;
    }
    responseBuffer_.clear();
    forwardingTimer_->start(qMax(1, timeoutMs));
    reportStage(QStringLiteral("forwarded"));
}

void HubClient::readForwardingResponse()
{
    if (socket_.bytesAvailable() == 0)
        return;
    responseBuffer_ += socket_.readAll();
    if (!forwardingPending_ || responseBuffer_.size() > 64 * 1024) {
        responseBuffer_.clear();
        if (forwardingPending_)
            completeForwarding(false);
        return;
    }
    int newline;
    while ((newline = responseBuffer_.indexOf('\n')) >= 0) {
        const auto message = QJsonDocument::fromJson(responseBuffer_.left(newline)).object();
        responseBuffer_.remove(0, newline + 1);
        if (message.value(QStringLiteral("type")).toString() == QStringLiteral("forwardedAccepted")
            && message.value(QStringLiteral("token")).toString() == arguments_.sessionToken) {
            completeForwarding(true);
            return;
        }
    }
}

void HubClient::completeForwarding(bool acknowledged)
{
    if (!forwardingPending_)
        return;
    forwardingPending_ = false;
    forwardingTimer_->stop();
    responseBuffer_.clear();
    emit forwardedCompleted(acknowledged);
}

void HubClient::send(const QJsonObject& message)
{
    const QByteArray line =
        QJsonDocument(message).toJson(QJsonDocument::Compact) +
        QByteArrayLiteral("\n");
    socket_.write(line);
    socket_.flush();
}

}
