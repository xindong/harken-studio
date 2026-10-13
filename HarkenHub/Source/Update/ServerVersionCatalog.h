#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QList>

#include <vector>

class QNetworkReply;

namespace VNH {

struct ServerVersionEntry
{
    QString version;
    QString changelog;
    QByteArray updateManifest;
    QString createdAt;
    QString fileName;
    QString objectPath;
    QString releaseTag;
    qint64 fileSize = -1;
};

struct ServerVersionList
{
    bool ok = false;
    QString error;
    std::vector<ServerVersionEntry> entries;
};

class ServerVersionCatalog : public QObject
{
    Q_OBJECT

public:
    explicit ServerVersionCatalog(QObject* parent = nullptr);

    bool fetch(const QUrl& versionsUrl, const QString& product,
               const QString& channel);
    bool fetch(const QList<QUrl>& versionsUrls, const QString& product,
               const QString& channel);
    bool isFetching() const;

    static ServerVersionList parse(const QByteArray& json,
                                   const QString& product,
                                   const QString& channel);
    static QByteArray matchEnvelopeForBuild(
        const std::vector<ServerVersionEntry>& entries,
        const QByteArray& publicKey, const QString& buildId,
        const QDateTime& now);

signals:
    void finished(const VNH::ServerVersionList& result);

private:
    void handleReplyFinished();
    void beginRequest();

    QNetworkAccessManager manager_;
    QNetworkReply* reply_ = nullptr;
    QString product_;
    QString channel_;
    QList<QUrl> urls_;
    QUrl requestUrl_;
    int sourceIndex_ = 0;
    int redirectCount_ = 0;
};

}
