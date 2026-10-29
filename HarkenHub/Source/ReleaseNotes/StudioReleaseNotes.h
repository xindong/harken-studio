#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QUrl>

namespace VNH {

class StudioReleaseNotes : public QObject
{
    Q_OBJECT

public:
    explicit StudioReleaseNotes(QObject* parent = nullptr);

    void fetch(const QString& version);
    QUrl url(const QString& version) const;

signals:
    void finished(const QString& version);

private:
    QNetworkAccessManager manager_;
    QHash<QString, QHash<QString, QUrl>> links_;
    QSet<QString> pending_;
};

}
