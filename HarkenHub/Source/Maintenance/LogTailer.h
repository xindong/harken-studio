#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace VNH {

class LogTailer : public QObject
{
    Q_OBJECT

public:
    explicit LogTailer(QObject* parent = nullptr);

    void setFiles(const QStringList& filePaths);
    void start(int intervalMs);
    void stop();
    void poll();

signals:
    void appended(const QString& text);

private:
    struct TailState
    {
        qint64 offset = 0;
        QByteArray head;
    };

    QStringList files_;
    QHash<QString, TailState> states_;
    QTimer timer_;
};

}
