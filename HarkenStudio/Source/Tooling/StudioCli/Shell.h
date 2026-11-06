#pragma once

#include <QJsonObject>

#include <csignal>
#include <functional>

namespace VNX { class IPCServer; }

namespace HarkenStudioCli {

class StudioSession;

enum class ShellExitRequest { None, Close, Discard, EndOfInput };

struct DesktopShellResult {
    int exitCode = 0;
    ShellExitRequest exitRequest = ShellExitRequest::None;
};

bool prepareDesktopShellStdio(QString* error, bool requireInput = true);
bool shellUsesJson();
void writeShellResponse(QJsonObject response, bool json = true);
DesktopShellResult runDesktopShell(VNX::IPCServer& dispatcher, bool json = false);

int runShell(StudioSession& session, int timeoutMs, bool json,
             volatile std::sig_atomic_t& interrupted,
             const std::function<void(QJsonObject)>& writeResponse,
             ShellExitRequest* exitRequest = nullptr);

}
