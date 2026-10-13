#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QSslConfiguration>
#include <QSslServer>
#include <QUrl>

class QTcpSocket;

namespace VNX::TestSupport {

class SyntheticHttpServer : public QObject
{
    Q_OBJECT

public:
    struct Response
    {
        int status = 200;
        QByteArray body = QByteArrayLiteral("{\"choices\":[]}");
        QList<QPair<QByteArray, QByteArray>> headers;
        int delayMs = 0;
        bool abortWithoutResponse = false;
    };

    explicit SyntheticHttpServer(QObject* parent = nullptr);
    ~SyntheticHttpServer() override;

    bool listen(bool tls = false);
    QUrl baseUrl() const;
    void enqueueResponse(const Response& response);

    int connectionCount() const { return connectionCount_; }
    int requestCount() const { return requestCount_; }
    int delayedResponseAttemptCount() const { return delayedResponseAttemptCount_; }
    QByteArray lastMethod() const { return lastMethod_; }
    QByteArray lastPath() const { return lastPath_; }
    QByteArray lastBody() const { return lastBody_; }
    QByteArray lastHeader(const QByteArray& name) const;

private:
    struct Connection
    {
        QTcpSocket* socket = nullptr;
        QByteArray data;
        qsizetype expectedSize = -1;
    };

    void readRequest(Connection* connection);
    void sendResponse(QTcpSocket* socket, const Response& response);

    QTcpServer server_;
    QSslServer tlsServer_;
    QSslConfiguration originalTlsConfiguration_;
    bool tls_ = false;
    QList<Response> responses_;
    QList<Connection*> connections_;
    QList<QPair<QByteArray, QByteArray>> lastHeaders_;
    int connectionCount_ = 0;
    int requestCount_ = 0;
    int delayedResponseAttemptCount_ = 0;
    QByteArray lastMethod_;
    QByteArray lastPath_;
    QByteArray lastBody_;
};

}
