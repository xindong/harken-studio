#pragma once

#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

namespace VNX {

class HubClient : public QObject
{
    Q_OBJECT

public:
    struct Arguments
    {
        bool present = false;
        QString serverName;
        QString sessionToken;
        QString expectedBuildId;
        QString projectPath;
        QString launchIntent;
        QString projectOpenRequestId;
        QString projectOpenApproval;
        bool createProject = false;
        QString projectName;
        QString projectLocation;
        QString projectTemplateId;
        QString projectTemplatePath;
    };

    static Arguments parseArguments(const QStringList& args);

    explicit HubClient(const Arguments& arguments,
                       QObject* parent = nullptr);

    void connectAndHelloAsync(int timeoutMs = 5000);
    bool isConnected() const;
    void reportStage(const QString& stage);
    void reportFailed(const QString& errorCode, const QString& error);

signals:
    void helloCompleted(bool connected);

private:
    void sendHello();
    void send(const QJsonObject& message);

    QLocalSocket socket_;
    QTimer* connectionTimer_ = nullptr;
    Arguments arguments_;
    bool helloSent_ = false;
    bool connectionPending_ = false;
};

}
