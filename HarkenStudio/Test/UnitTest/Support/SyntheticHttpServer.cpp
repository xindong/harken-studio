#include "SyntheticHttpServer.h"
#include "SyntheticTlsIdentity.h"

#include <QPointer>
#include <QTcpSocket>
#include <QTimer>
#include <QSslCertificate>
#include <QSslKey>

namespace VNX::TestSupport {
namespace {

QByteArray reasonPhrase(int status)
{
    switch (status)
    {
    case 200: return QByteArrayLiteral("OK");
    case 302: return QByteArrayLiteral("Found");
    case 400: return QByteArrayLiteral("Bad Request");
    case 401: return QByteArrayLiteral("Unauthorized");
    case 403: return QByteArrayLiteral("Forbidden");
    case 404: return QByteArrayLiteral("Not Found");
    case 500: return QByteArrayLiteral("Internal Server Error");
    case 503: return QByteArrayLiteral("Service Unavailable");
    default: return QByteArrayLiteral("Synthetic");
    }
}

}

SyntheticHttpServer::SyntheticHttpServer(QObject* parent)
    : QObject(parent)
{
    auto accept = [this]() {
        QTcpServer& active = tls_ ? tlsServer_ : server_;
        while (QTcpSocket* socket = active.nextPendingConnection())
        {
            ++connectionCount_;
            auto* connection = new Connection{socket};
            connections_.append(connection);
            connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, connection]() {
                readRequest(connection);
            });
            connect(socket, &QObject::destroyed, this, [this, connection]() {
                connections_.removeOne(connection);
                delete connection;
            });
        }
    };
    connect(&server_, &QTcpServer::newConnection, this, accept);
    connect(&tlsServer_, &QTcpServer::pendingConnectionAvailable, this, accept);
}

SyntheticHttpServer::~SyntheticHttpServer()
{
    while (!connections_.isEmpty())
        delete connections_.last()->socket;
    if (tls_)
        QSslConfiguration::setDefaultConfiguration(originalTlsConfiguration_);
}

bool SyntheticHttpServer::listen(bool tls)
{
    tls_ = tls;
    if (!tls_)
        return server_.listen(QHostAddress::LocalHost, 0);
    originalTlsConfiguration_ = QSslConfiguration::defaultConfiguration();
    auto client = originalTlsConfiguration_;
    client.addCaCertificate(QSslCertificate(QByteArray(kTestCertificate)));
    QSslConfiguration::setDefaultConfiguration(client);
    auto server = client;
    server.setLocalCertificate(QSslCertificate(QByteArray(kTestCertificate)));
    server.setPrivateKey(QSslKey(QByteArray::fromBase64(kTestPrivateKeyDer), QSsl::Rsa, QSsl::Der));
    server.setPeerVerifyMode(QSslSocket::VerifyNone);
    tlsServer_.setSslConfiguration(server);
    return tlsServer_.listen(QHostAddress::LocalHost, 0);
}

QUrl SyntheticHttpServer::baseUrl() const
{
    return QUrl(QStringLiteral("%1://127.0.0.1:%2/v1")
                    .arg(tls_ ? QStringLiteral("https") : QStringLiteral("http"))
                    .arg(tls_ ? tlsServer_.serverPort() : server_.serverPort()));
}

void SyntheticHttpServer::enqueueResponse(const Response& response)
{
    responses_.append(response);
}

QByteArray SyntheticHttpServer::lastHeader(const QByteArray& name) const
{
    for (const auto& header : lastHeaders_)
    {
        if (header.first.compare(name, Qt::CaseInsensitive) == 0)
            return header.second;
    }
    return {};
}

void SyntheticHttpServer::readRequest(Connection* connection)
{
    if (!connection || !connection->socket)
        return;

    QTcpSocket* socket = connection->socket;
    connection->data += socket->readAll();
    const qsizetype headerEnd = connection->data.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return;

    if (connection->expectedSize < 0)
    {
        qsizetype contentLength = 0;
        const QList<QByteArray> lines = connection->data.left(headerEnd).split('\n');
        for (QByteArray line : lines)
        {
            line = line.trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon > 0 && line.left(colon).compare("Content-Length", Qt::CaseInsensitive) == 0)
                contentLength = line.mid(colon + 1).trimmed().toLongLong();
        }
        connection->expectedSize = headerEnd + 4 + contentLength;
    }
    if (connection->data.size() < connection->expectedSize)
        return;

    const QByteArray requestData = connection->data.left(connection->expectedSize);
    connection->data.remove(0, connection->expectedSize);
    connection->expectedSize = -1;
    const QList<QByteArray> lines = requestData.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    lastMethod_ = requestLine.value(0);
    lastPath_ = requestLine.value(1);
    lastHeaders_.clear();
    for (qsizetype i = 1; i < lines.size(); ++i)
    {
        const QByteArray line = lines[i].trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon > 0)
            lastHeaders_.append({line.left(colon), line.mid(colon + 1).trimmed()});
    }
    lastBody_ = requestData.mid(headerEnd + 4);
    ++requestCount_;

    const Response response = responses_.isEmpty() ? Response{} : responses_.takeFirst();
    if (response.abortWithoutResponse)
    {
        const QPointer<QTcpSocket> guardedSocket(socket);
        socket->write(QByteArrayLiteral("HTTP/1."));
        socket->flush();
        QTimer::singleShot(10, this, [guardedSocket]() {
            if (guardedSocket)
                guardedSocket->abort();
        });
        return;
    }
    if (response.delayMs > 0)
    {
        const QPointer<QTcpSocket> guardedSocket(socket);
        QTimer::singleShot(response.delayMs, this, [this, guardedSocket, response]() {
            ++delayedResponseAttemptCount_;
            if (guardedSocket)
                sendResponse(guardedSocket, response);
        });
    }
    else
    {
        sendResponse(socket, response);
    }
}

void SyntheticHttpServer::sendResponse(QTcpSocket* socket, const Response& response)
{
    if (!socket || socket->state() == QAbstractSocket::UnconnectedState)
        return;

    QByteArray data = QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(response.status)
        + QByteArrayLiteral(" ") + reasonPhrase(response.status) + QByteArrayLiteral("\r\n");
    data += QByteArrayLiteral("Content-Type: application/json\r\n");
    for (const auto& header : response.headers)
        data += header.first + QByteArrayLiteral(": ") + header.second + QByteArrayLiteral("\r\n");
    data += QByteArrayLiteral("Content-Length: ") + QByteArray::number(response.body.size())
        + QByteArrayLiteral("\r\nConnection: keep-alive\r\n\r\n") + response.body;
    socket->write(data);
}

}
