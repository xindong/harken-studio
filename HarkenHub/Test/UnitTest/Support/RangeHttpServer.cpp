#include "RangeHttpServer.h"

#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

namespace HarkenHubTest {

namespace {

QByteArray headerValue(const QList<QByteArray>& lines, const QByteArray& name)
{
    for (const QByteArray& line : lines) {
        const qsizetype colon = line.indexOf(':');
        if (colon > 0 &&
            line.left(colon).trimmed().compare(name, Qt::CaseInsensitive) == 0)
            return line.mid(colon + 1).trimmed();
    }
    return {};
}

qint64 rangeStart(const QByteArray& value)
{
    if (!value.startsWith("bytes="))
        return -1;
    const QByteArray range = value.mid(6);
    const qsizetype dash = range.indexOf('-');
    if (dash <= 0)
        return -1;
    bool parsed = false;
    const qint64 start = range.left(dash).toLongLong(&parsed);
    return parsed ? start : -1;
}

}

RangeHttpServer::RangeHttpServer()
{
    QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this]() {
        while (QTcpSocket* socket = server_.nextPendingConnection()) {
            QObject::connect(socket, &QAbstractSocket::disconnected, socket,
                             &QObject::deleteLater);
            QObject::connect(socket, &QObject::destroyed, &server_,
                             [this, socket]() { buffers_.remove(socket); });
            QObject::connect(socket, &QTcpSocket::readyRead, &server_,
                             [this, socket]() {
                                 buffers_[socket] += socket->readAll();
                                 const QByteArray& data = buffers_[socket];
                                 const qsizetype end = data.indexOf("\r\n\r\n");
                                 if (end < 0)
                                     return;
                                 QList<QByteArray> lines = data.left(end).split('\n');
                                 for (QByteArray& line : lines)
                                     line = line.trimmed();
                                 bool lengthParsed = false;
                                 const qint64 contentLength =
                                     headerValue(lines,
                                                 QByteArrayLiteral(
                                                     "Content-Length"))
                                         .toLongLong(&lengthParsed);
                                 const qsizetype bodyStart = end + 4;
                                 if (lengthParsed &&
                                     data.size() - bodyStart < contentLength)
                                     return;
                                 const QByteArray body = lengthParsed
                                     ? data.mid(bodyStart,
                                                static_cast<qsizetype>(
                                                    contentLength))
                                     : QByteArray();
                                 QObject::disconnect(socket,
                                                     &QTcpSocket::readyRead,
                                                     nullptr, nullptr);
                                 handleRequest(socket, lines, body);
                             });
        }
    });
}

bool RangeHttpServer::listen()
{
    return server_.listen(QHostAddress::LocalHost, 0);
}

QUrl RangeHttpServer::packageUrl() const
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1/studio.pkg")
                    .arg(server_.serverPort()));
}

QUrl RangeHttpServer::baseUrl() const
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1")
                    .arg(server_.serverPort()));
}

void RangeHttpServer::setPayload(const QByteArray& payload, const QByteArray& etag,
                                 const QByteArray& lastModified)
{
    payload_ = payload;
    etag_ = etag;
    lastModified_ = lastModified;
}

void RangeHttpServer::enqueue(const Script& script)
{
    scripts_.append(script);
}

void RangeHttpServer::handleRequest(QTcpSocket* socket,
                                    const QList<QByteArray>& headerLines,
                                    const QByteArray& body)
{
    ++requestCount_;
    const QByteArray method = headerLines.value(0).split(' ').value(0);
    const QByteArray range = headerValue(headerLines, QByteArrayLiteral("Range"));
    const QByteArray ifRange = headerValue(headerLines, QByteArrayLiteral("If-Range"));
    const QByteArray ifNoneMatch =
        headerValue(headerLines, QByteArrayLiteral("If-None-Match"));
    methodHistory_.append(method);
    rangeHistory_.append(range);
    ifRangeHistory_.append(ifRange);
    ifNoneMatchHistory_.append(ifNoneMatch);
    authorizationHistory_.append(
        headerValue(headerLines, QByteArrayLiteral("Authorization")));
    bodyHistory_.append(body);

    const Script script = scripts_.isEmpty() ? defaultScript_ : scripts_.takeFirst();

    const QByteArray effectiveLastModified =
        script.lastModifiedOverride.isEmpty() ? lastModified_
                                              : script.lastModifiedOverride;

    if (method == QByteArrayLiteral("HEAD")) {
        if (script.mode == Script::Mode::Status) {
            respondHeadOnly(socket, script.status, {}, 0);
            return;
        }
        QByteArray headers = QByteArrayLiteral("ETag: ") + etag_ +
                             QByteArrayLiteral("\r\n");
        if (!effectiveLastModified.isEmpty())
            headers += QByteArrayLiteral("Last-Modified: ") +
                       effectiveLastModified + QByteArrayLiteral("\r\n");
        respondHeadOnly(socket, 200, headers, payload_.size());
        return;
    }

    const auto sendScripted = [this, script, ifNoneMatch,
                               effectiveLastModified](
                                  QTcpSocket* target,
                                  const QByteArray& rangeValue,
                                  const QByteArray& ifRangeValue) {
        if (script.mode == Script::Mode::Status) {
            const QByteArray statusBody = script.responseBody.isEmpty()
                                              ? QByteArrayLiteral("error")
                                              : script.responseBody;
            respond(target, script.status, {}, statusBody, statusBody.size(),
                    statusBody.size());
            return;
        }

        if (script.mode == Script::Mode::Redirect) {
            respond(target, 302,
                    QByteArrayLiteral("Location: ") + script.redirectLocation +
                        QByteArrayLiteral("\r\n"),
                    QByteArray(), 0, 0);
            return;
        }

        if (script.mode == Script::Mode::ForceNotModified ||
            (script.mode == Script::Mode::Normal && !ifNoneMatch.isEmpty() &&
             ifNoneMatch == etag_)) {
            respond(target, 304,
                    QByteArrayLiteral("ETag: ") + etag_ + QByteArrayLiteral("\r\n"),
                    QByteArray(), 0, 0);
            return;
        }

        qint64 offset = 0;
        qint64 end = payload_.size() - 1;
        bool partial = false;
        if (script.mode != Script::Mode::IgnoreRange) {
            const qint64 start = rangeStart(rangeValue);
            if (start >= 0 && start < payload_.size() &&
                (ifRangeValue.isEmpty() || ifRangeValue == etag_)) {
                offset = start;
                partial = true;
                const auto requestedEnd = rangeValue.mid(rangeValue.indexOf('-') + 1);
                if (!requestedEnd.isEmpty())
                    end = qMin(end, requestedEnd.toLongLong());
            }
        }

        const QByteArray body = payload_.mid(offset, end - offset + 1);
        QByteArray headers = QByteArrayLiteral("ETag: ") + etag_ +
                             QByteArrayLiteral("\r\n");
        if (!effectiveLastModified.isEmpty())
            headers += QByteArrayLiteral("Last-Modified: ") +
                       effectiveLastModified + QByteArrayLiteral("\r\n");
        int status = 200;
        if (partial) {
            status = 206;
            const QByteArray contentRange = script.contentRangeOverride.isEmpty()
                ? QByteArrayLiteral("bytes ") +
                       QByteArray::number(offset) + QByteArrayLiteral("-") +
                       QByteArray::number(end) +
                       QByteArrayLiteral("/") +
                       QByteArray::number(payload_.size())
                : script.contentRangeOverride;
            headers += QByteArrayLiteral("Content-Range: ") + contentRange + QByteArrayLiteral("\r\n");
        }

        const qint64 writeLimit = script.mode == Script::Mode::AbortAfterPrefix
                                      ? qMin<qint64>(script.prefixBytes, body.size())
                                      : body.size();
        respond(target, status, headers, body, body.size(), writeLimit,
                script.chunkBytes, script.chunkIntervalMs);
    };

    if (script.delayMs > 0) {
        const QPointer<QTcpSocket> guarded(socket);
        QTimer::singleShot(script.delayMs, socket,
                           [guarded, sendScripted, range, ifRange]() {
                               if (guarded)
                                   sendScripted(guarded, range, ifRange);
                           });
        return;
    }
    sendScripted(socket, range, ifRange);
}

void RangeHttpServer::respondHeadOnly(QTcpSocket* socket, int status,
                                      const QByteArray& headers,
                                      qint64 claimedLength)
{
    if (!socket || socket->state() == QAbstractSocket::UnconnectedState)
        return;

    const QByteArray data =
        QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) +
        QByteArrayLiteral(" Synthetic\r\n") +
        QByteArrayLiteral("Content-Type: application/octet-stream\r\n") +
        headers + QByteArrayLiteral("Content-Length: ") +
        QByteArray::number(claimedLength) +
        QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
    socket->write(data);
    socket->disconnectFromHost();
}

void RangeHttpServer::respond(QTcpSocket* socket, int status,
                              const QByteArray& headers, const QByteArray& body,
                              qint64 claimedLength, qint64 writeLimit,
                              int chunkBytes, int chunkIntervalMs)
{
    if (!socket || socket->state() == QAbstractSocket::UnconnectedState)
        return;

    QByteArray data = QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) +
                      QByteArrayLiteral(" Synthetic\r\n") +
                      QByteArrayLiteral("Content-Type: application/octet-stream\r\n") +
                      headers + QByteArrayLiteral("Content-Length: ") +
                      QByteArray::number(claimedLength) +
                      QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
    socket->write(data);
    if (chunkBytes > 0 && chunkIntervalMs > 0) {
        auto* timer = new QTimer(socket);
        QObject::connect(timer, &QTimer::timeout, socket,
                         [this, socket, timer, body, writeLimit, claimedLength,
                          chunkBytes, sent = qint64(0)]() mutable {
            if (socket->state() != QAbstractSocket::ConnectedState) {
                timer->stop();
                return;
            }
            const auto chunk = body.mid(sent, qMin<qint64>(chunkBytes, writeLimit - sent));
            const auto written = socket->write(chunk);
            if (written > 0) {
                sent += written;
                bodyBytesSent_ += written;
            }
            if (sent >= writeLimit) {
                timer->stop();
                socket->flush();
                if (writeLimit < claimedLength)
                    QTimer::singleShot(20, socket, [socket]() { socket->abort(); });
                else
                    socket->disconnectFromHost();
            }
        });
        timer->start(chunkIntervalMs);
        return;
    }
    bodyBytesSent_ += socket->write(body.left(writeLimit));

    if (writeLimit < claimedLength) {
        socket->flush();
        const QPointer<QTcpSocket> guarded(socket);
        QTimer::singleShot(20, socket, [guarded]() {
            if (guarded)
                guarded->abort();
        });
        return;
    }
    socket->disconnectFromHost();
}

}
