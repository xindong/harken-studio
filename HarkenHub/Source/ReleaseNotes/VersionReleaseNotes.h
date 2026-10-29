#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QUrl>

namespace VNH {

class VersionReleaseNotes : public QObject
{
    Q_OBJECT

public:
    enum class Product { Studio, UnityIntegration };

    explicit VersionReleaseNotes(QObject* parent = nullptr);

    void fetch(const QString& version);
    QUrl url(const QString& version, Product product) const;

signals:
    void finished(const QString& version);

private:
    QNetworkAccessManager manager_;
    QHash<QString, QHash<Product, QHash<QString, QUrl>>> links_;
    QSet<QString> pending_;
};

}
