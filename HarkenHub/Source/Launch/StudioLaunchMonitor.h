#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>

class QLocalSocket;

namespace VNH {

class StudioLaunchMonitor : public QObject
{
    Q_OBJECT

public:
    static constexpr int kProtocolVersion = 2;
    static constexpr qint64 kMaxMessageBytes = 64 * 1024;

    explicit StudioLaunchMonitor(QObject* parent = nullptr);

    bool listen();
    void close();
    QString serverName() const { return serverName_; }
    QString sessionToken() const { return sessionToken_; }
    void setExpectedBuildId(const QString& buildId);
    void setExpectedInstallPath(const QString& installPath);
    void setExpectedProcess(qint64 pid, const QString& executablePath);
    void setExpectedOperation(const QString& operation);
    void setExpectedLaunchIntent(const QString& intent);
    bool helloAccepted() const { return helloAccepted_; }
    QString lastStage() const { return lastStage_; }
    QString failureCode() const { return failureCode_; }

signals:
    void stageChanged(const QString& stage);
    void ready();
    void failed(const QString& error);

private:
    void handleNewConnection();
    void handleReadyRead();
    void rejectClient(const QString& error);
    void processMessage(const QByteArray& line);

    QLocalServer server_;
    QLocalSocket* client_ = nullptr;
    QByteArray buffer_;
    QString serverName_;
    QString sessionToken_;
    QString expectedBuildId_;
    QString expectedInstallPath_;
    QString expectedExecutablePath_;
    qint64 expectedPid_ = 0;
    QString expectedOperation_;
    QString expectedLaunchIntent_;
    QString lastStage_;
    QString failureCode_;
    bool helloAccepted_ = false;
    bool finished_ = false;
};

}
