#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QUrl>

class QTcpSocket;

namespace HarkenHubTest {

class RangeHttpServer
{
public:
    struct Script
    {
        enum class Mode
        {
            Normal,
            AbortAfterPrefix,
            Status,
            IgnoreRange,
            ForceNotModified,
            Redirect
        };

        Mode mode = Mode::Normal;
        int status = 500;
        qint64 prefixBytes = 0;
        int delayMs = 0;
        QByteArray redirectLocation;
        QByteArray lastModifiedOverride;
        QByteArray responseBody;
    };

    RangeHttpServer();

    bool listen();
    QUrl packageUrl() const;
    QUrl baseUrl() const;

    void setPayload(const QByteArray& payload, const QByteArray& etag,
                    const QByteArray& lastModified = QByteArrayLiteral(
                        "Fri, 24 Jul 2026 08:00:00 GMT"));
    void enqueue(const Script& script);

    int requestCount() const { return requestCount_; }
    QList<QByteArray> methodHistory() const { return methodHistory_; }
    QList<QByteArray> rangeHistory() const { return rangeHistory_; }
    QList<QByteArray> ifRangeHistory() const { return ifRangeHistory_; }
    QList<QByteArray> ifNoneMatchHistory() const { return ifNoneMatchHistory_; }
    QList<QByteArray> authorizationHistory() const
    {
        return authorizationHistory_;
    }
    QList<QByteArray> bodyHistory() const { return bodyHistory_; }

private:
    void handleRequest(QTcpSocket* socket, const QList<QByteArray>& headerLines,
                       const QByteArray& body);
    void respond(QTcpSocket* socket, int status, const QByteArray& headers,
                 const QByteArray& body, qint64 claimedLength, qint64 writeLimit);
    void respondHeadOnly(QTcpSocket* socket, int status,
                         const QByteArray& headers, qint64 claimedLength);

    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QByteArray payload_;
    QByteArray etag_;
    QByteArray lastModified_;
    QList<Script> scripts_;
    QList<QByteArray> methodHistory_;
    QList<QByteArray> rangeHistory_;
    QList<QByteArray> ifRangeHistory_;
    QList<QByteArray> ifNoneMatchHistory_;
    QList<QByteArray> authorizationHistory_;
    QList<QByteArray> bodyHistory_;
    int requestCount_ = 0;
};

}
