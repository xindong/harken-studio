#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

namespace HarkenStudioCli {

QJsonObject failure(const QString& code, const QString& message);
bool succeeded(const QJsonObject& response);

class StudioSession {
public:
    StudioSession();
    ~StudioSession();
    QJsonObject open(const QString& path);
    QJsonObject commands() const;
    QJsonObject describe(const QString& name) const;
    QJsonObject execute(const QString& name, const QJsonObject& args,
                        int timeoutMs, const std::function<bool()>& canceled);
    QJsonObject save();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
