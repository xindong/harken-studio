#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

namespace VNH {

class SourceProbe : public QObject
{
    Q_OBJECT

public:
    explicit SourceProbe(QObject* parent = nullptr);
    ~SourceProbe() override;
    void start(const QList<QUrl>& sources, qint64 expectedSize);
    void cancel();
    bool isActive() const { return active_; }

signals:
    void finished(const QList<QUrl>& rankedSources, const QString& error);

private:
    struct Sample
    {
        QUrl source;
        QNetworkReply* reply = nullptr;
        QElapsedTimer elapsed;
        qint64 bytes = 0;
        qint64 received = 0;
        double rate = 0;
        int redirects = 0;
        bool done = false;
    };

    void request(int index, const QUrl& url);
    void read(int index);
    void completeSample(int index, bool usable);
    void finish(const QString& error = {});
    void stopReply(Sample& sample);

    QNetworkAccessManager manager_;
    QTimer deadline_;
    QTimer decision_;
    QElapsedTimer elapsed_;
    QList<Sample> samples_;
    qint64 expectedSize_ = -1;
    qint64 sampleSize_ = 0;
    bool active_ = false;
};

}
