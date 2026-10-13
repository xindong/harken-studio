#include "App/SingleInstanceGuard.h"

#include <QDir>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QThread>
#include <QTimer>

#include <memory>

namespace VNH {

namespace {

constexpr int kConnectTimeoutMs = 300;
constexpr int kConnectAttempts = 5;
constexpr int kConnectRetryDelayMs = 50;
constexpr int kWriteTimeoutMs = 5000;
constexpr qint64 kMaxMessageBytes = 1 * 1024 * 1024;
constexpr int kMaxPendingClients = 32;
constexpr int kClientIdleTimeoutMs = 5000;

QString lockFilePath(const QString& key)
{
    return QDir::temp().filePath(key + QStringLiteral(".lock"));
}

bool sendToServer(const QString& key, const QStringList& arguments,
                  int connectAttempts)
{
    QLocalSocket socket;
    bool connected = false;
    for (int attempt = 0; attempt < connectAttempts; ++attempt) {
        socket.connectToServer(key);
        if (socket.waitForConnected(kConnectTimeoutMs)) {
            connected = true;
            break;
        }
        socket.abort();
        if (attempt + 1 < connectAttempts)
            QThread::msleep(kConnectRetryDelayMs);
    }
    if (!connected)
        return false;

    QJsonArray array;
    for (const QString& argument : arguments)
        array.append(argument);
    const QByteArray payload =
        QJsonDocument(array).toJson(QJsonDocument::Compact);
    if (socket.write(payload) != payload.size()) {
        socket.abort();
        return false;
    }
    socket.flush();
    QDeadlineTimer deadline(kWriteTimeoutMs);
    while (socket.bytesToWrite() > 0) {
        const qint64 remaining = deadline.remainingTime();
        if (remaining <= 0 ||
            (!socket.waitForBytesWritten(static_cast<int>(remaining)) &&
             socket.bytesToWrite() > 0)) {
            socket.abort();
            return false;
        }
    }
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState &&
        !socket.waitForDisconnected(kWriteTimeoutMs)) {
        socket.abort();
        return false;
    }
    return true;
}

}

SingleInstanceGuard::SingleInstanceGuard(const QString& key, QObject* parent)
    : QObject(parent)
    , key_(key)
    , lock_(std::make_unique<QLockFile>(lockFilePath(key)))
{
    primary_ = lock_->tryLock(0);
    if (primary_)
        startServer();
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    if (primary_)
        lock_->unlock();
}

qint64 SingleInstanceGuard::primaryProcessId() const
{
    qint64 processId = 0;
    QString hostName;
    QString applicationName;
    if (!lock_->getLockInfo(&processId, &hostName, &applicationName))
        return 0;
    return processId;
}

bool SingleInstanceGuard::retryPrimary()
{
    if (primary_)
        return true;
    primary_ = lock_->tryLock(0);
    if (primary_)
        startServer();
    return primary_;
}

void SingleInstanceGuard::startServer()
{
    QLocalServer::removeServer(key_);
    server_ = new QLocalServer(this);
    server_->setMaxPendingConnections(kMaxPendingClients);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(key_)) {
        lock_->unlock();
        primary_ = false;
        return;
    }

    connect(server_, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket* client = server_->nextPendingConnection()) {
            if (activeClients_ >= kMaxPendingClients) {
                client->abort();
                client->deleteLater();
                continue;
            }
            ++activeClients_;
            connect(client, &QObject::destroyed, this,
                    [this]() { --activeClients_; });

            QTimer::singleShot(kClientIdleTimeoutMs, client, [client]() {
                client->abort();
                client->deleteLater();
            });

            auto buffer = std::make_shared<QByteArray>();
            connect(client, &QLocalSocket::readyRead, client, [client, buffer]() {
                buffer->append(client->readAll());
                if (buffer->size() > kMaxMessageBytes) {
                    buffer->clear();
                    client->abort();
                    client->deleteLater();
                }
            });
            connect(client, &QLocalSocket::disconnected, this, [this, client, buffer]() {
                buffer->append(client->readAll());
                client->deleteLater();
                if (buffer->isEmpty() || buffer->size() > kMaxMessageBytes)
                    return;
                const QJsonDocument doc = QJsonDocument::fromJson(*buffer);
                if (!doc.isArray())
                    return;
                QStringList arguments;
                for (const auto& value : doc.array())
                    arguments.append(value.toString());
                emit activationRequested(arguments);
            });
        }
    });
}

QString SingleInstanceGuard::guiServerName()
{
    return QStringLiteral("HarkenSound.HarkenHub");
}

QString SingleInstanceGuard::backgroundServerName()
{
    return QStringLiteral("HarkenSound.HarkenHub.Background");
}

bool SingleInstanceGuard::notifyPrimary(const QStringList& arguments)
{
    return sendToServer(key_, arguments, kConnectAttempts);
}

bool SingleInstanceGuard::isServerActive(const QString& key)
{
    QLocalSocket socket;
    socket.connectToServer(key);
    const bool connected = socket.waitForConnected(kConnectTimeoutMs);
    socket.abort();
    return connected;
}

bool SingleInstanceGuard::sendTo(const QString& key, const QStringList& arguments)
{
    return sendToServer(key, arguments, 1);
}

}
