#pragma once

#include <QJsonObject>

#include <csignal>
#include <functional>

namespace HarkenStudioCli {

class StudioSession;

int runShell(StudioSession& session, int timeoutMs, bool json,
             volatile std::sig_atomic_t& interrupted,
             const std::function<void(QJsonObject)>& writeResponse);

}
