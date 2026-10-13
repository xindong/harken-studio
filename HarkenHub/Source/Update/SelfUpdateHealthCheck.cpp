#include "Update/SelfUpdateHealthCheck.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRandomGenerator>
#include <QTimer>

namespace VNH {

namespace {

constexpr qint64 kMaxReportBytes = 64 * 1024;

}

QString SelfUpdateHealthCheck::newChannelName()
{
    QByteArray bytes(8, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32*>(bytes.data()),
        bytes.size() / static_cast<int>(sizeof(quint32)));
    return QStringLiteral("vnl-h-") + QString::fromLatin1(bytes.toHex());
}

HealthReport SelfUpdateHealthCheck::awaitReport(
    const QString& channelName,
    const std::function<bool(const QString&)>& launchReporter, int timeoutMs)
{
    HealthReport report;

    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(channelName);
    if (!server.listen(channelName)) {
        report.error = QStringLiteral("Cannot listen on the health channel: %1")
                           .arg(server.errorString());
        return report;
    }

    QEventLoop loop;
    bool finished = false;
    QByteArray buffer;
    QLocalSocket* client = nullptr;

    auto finish = [&](bool ok, const QString& error) {
        if (finished)
            return;
        finished = true;
        report.ok = ok;
        report.error = error;
        loop.quit();
    };

    auto consume = [&]() {
        const QJsonDocument doc = QJsonDocument::fromJson(buffer);
        if (!doc.isObject()) {
            finish(false, QStringLiteral("The health report is malformed"));
            return;
        }
        const QJsonObject object = doc.object();
        if (object.value(QStringLiteral("product")).toString() !=
            QStringLiteral("HarkenHub")) {
            finish(false, QStringLiteral(
                              "The health report is for another product"));
            return;
        }
        if (!object.value(QStringLiteral("healthy")).toBool()) {
            finish(false,
                   QStringLiteral("The hub reported an unhealthy state"));
            return;
        }
        report.version = object.value(QStringLiteral("version")).toString();
        report.buildId = object.value(QStringLiteral("buildId")).toString();
        finish(true, QString());
    };

    QObject::connect(&server, &QLocalServer::newConnection, &server, [&]() {
        while (QLocalSocket* pending = server.nextPendingConnection()) {
            if (client) {
                pending->abort();
                pending->deleteLater();
                continue;
            }
            client = pending;
            QObject::connect(client, &QLocalSocket::readyRead, client, [&]() {
                buffer.append(client->readAll());
                if (buffer.size() > kMaxReportBytes) {
                    finish(false,
                           QStringLiteral("The health report is oversized"));
                    return;
                }
                if (buffer.contains('\n'))
                    consume();
            });
            QObject::connect(client, &QLocalSocket::disconnected, client,
                             [&]() {
                                 buffer.append(client->readAll());
                                 if (!finished)
                                     consume();
                             });
        }
    });

    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &server, [&]() {
        finish(false, QStringLiteral(
                          "Timed out waiting for the hub health report"));
    });
    timeoutTimer.start(timeoutMs);

    if (!launchReporter(channelName)) {
        report.ok = false;
        report.error =
            QStringLiteral("Cannot start the hub health reporter");
        return report;
    }

    loop.exec();
    return report;
}

HealthReport SelfUpdateHealthCheck::runInstalledCheck(
    const QString& hubExePath, int timeoutMs)
{
    return awaitReport(
        newChannelName(),
        [&hubExePath](const QString& channel) {
            return QProcess::startDetached(
                hubExePath,
                {QStringLiteral("--health-handshake"), channel});
        },
        timeoutMs);
}

}
