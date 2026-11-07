#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

namespace VNX {

class IPCServer;

class StudioInstanceServer : public QObject {
public:
    explicit StudioInstanceServer(IPCServer* dispatcher, QObject* parent = nullptr);
    ~StudioInstanceServer() override;
    bool start(QString* error = nullptr);
    void stop();
    QJsonObject descriptor() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}

namespace HarkenStudioCli {

class StudioInstanceClient {
public:
    explicit StudioInstanceClient(QString instanceId);
    ~StudioInstanceClient();
    static QJsonObject discover(int timeoutMs = 1000,
                               const std::function<bool()>& canceled = {});
    QJsonObject execute(const QString& name, const QJsonObject& args,
                        int timeoutMs, const std::function<bool()>& canceled,
                        const QString& expectedProjectSession = {});
    QJsonObject commands(int timeoutMs, const std::function<bool()>& canceled);
    QJsonObject describe(const QString& name, int timeoutMs,
                         const std::function<bool()>& canceled);
    QJsonObject validate(const QString& name, const QJsonObject& args,
                         int timeoutMs, const std::function<bool()>& canceled);
    QJsonObject descriptor() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
