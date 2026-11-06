#pragma once

#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <memory>

namespace VNX { class IPCServer; }

namespace HarkenStudioCli {

class StudioInstanceClient;

QJsonObject failure(const QString& code, const QString& message);
bool succeeded(const QJsonObject& response);

class StudioSession {
public:
    explicit StudioSession(const QString& instanceId = {}, int timeoutMs = 300000,
                           std::function<bool()> canceled = {});
    explicit StudioSession(VNX::IPCServer& dispatcher, int timeoutMs = 300000,
                           std::function<bool()> canceled = {});
    ~StudioSession();
    bool isAttached() const { return !state_; }
    bool isRemote() const { return instance_ != nullptr; }
    QJsonObject open(const QString& path, int timeoutMs, const std::function<bool()>& canceled);
    QJsonObject commands() const;
    QJsonObject describe(const QString& name) const;
    QJsonObject validate(const QString& name, const QJsonObject& args) const;
    QJsonObject execute(const QString& name, const QJsonObject& args,
                        int timeoutMs, const std::function<bool()>& canceled);
    QJsonObject save();

private:
    QJsonObject dispatchDesktop(const QString& name, const QJsonObject& args,
                                int timeoutMs, const std::function<bool()>& canceled) const;
    QJsonObject finishOperation(QJsonObject response, int timeoutMs,
                                const std::function<bool()>& canceled);
    QJsonObject waitForProjectClose(int timeoutMs, const std::function<bool()>& canceled);
    QJsonObject waitForCurrentWorld(int timeoutMs, const std::function<bool()>& canceled);
    struct State;
    std::unique_ptr<State> state_;
    std::unique_ptr<StudioInstanceClient> instance_;
    QPointer<VNX::IPCServer> desktopDispatcher_;
    int timeoutMs_;
    std::function<bool()> canceled_;
};

}
