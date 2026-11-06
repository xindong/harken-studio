#include "Shell.h"
#include "StudioSession.h"
#include "Core/ProjectManager.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>

#include <cerrno>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <qt_windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace HarkenStudioCli {
namespace {

enum class InputState { Line, End, Interrupted, Error };

struct Input {
    InputState state = InputState::End;
    QByteArray line;
};

bool terminalInput()
{
#ifdef Q_OS_WIN
    const int descriptor = _fileno(stdin);
    DWORD mode = 0;
    return descriptor >= 0 && GetConsoleMode(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)), &mode);
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

Input readInput(bool terminal, QByteArray& pending, const std::atomic_bool& stopping)
{
#ifdef Q_OS_WIN
    const int descriptor = _fileno(stdin);
    if (descriptor < 0) return {InputState::Error, {}};
    const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
    if (terminal) {
        QString line;
        while (!stopping.load()) {
            wchar_t buffer[256];
            DWORD count = 0;
            if (!ReadConsoleW(handle, buffer, 256, &count, nullptr))
                return {GetLastError() == ERROR_OPERATION_ABORTED
                    ? InputState::Interrupted : InputState::Error, {}};
            if (count == 0) return {InputState::Interrupted, {}};
            line.append(QString::fromWCharArray(buffer, static_cast<qsizetype>(count)));
            if (line.startsWith(QChar(26))) return {};
            if (line.endsWith(QLatin1Char('\n')))
                return {InputState::Line, line.toUtf8()};
        }
        return {};
    }
#else
    Q_UNUSED(terminal);
#endif
    const auto endOfInput = [&]() -> Input {
        if (pending.isEmpty()) return {};
        return {InputState::Line, std::exchange(pending, {})};
    };
    while (!stopping.load()) {
        const auto newline = pending.indexOf('\n');
        if (newline >= 0) {
            const auto line = pending.left(newline + 1);
            pending.remove(0, newline + 1);
            return {InputState::Line, line};
        }
        char buffer[4096];
#ifdef Q_OS_WIN
        DWORD available = sizeof(buffer);
        if (GetFileType(handle) == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr))
                return GetLastError() == ERROR_BROKEN_PIPE ? endOfInput() : Input{InputState::Error, {}};
            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }
        DWORD count = 0;
        if (!ReadFile(handle, buffer, qMin<DWORD>(available, sizeof(buffer)), &count, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) return endOfInput();
            return {error == ERROR_OPERATION_ABORTED ? InputState::Interrupted : InputState::Error, {}};
        }
#else
        pollfd input{fileno(stdin), POLLIN, 0};
        const int ready = poll(&input, 1, 25);
        if (ready < 0) return {errno == EINTR ? InputState::Interrupted : InputState::Error, {}};
        if (ready == 0) continue;
        if ((input.revents & (POLLERR | POLLNVAL)) != 0) return {InputState::Error, {}};
        const auto count = read(input.fd, buffer, sizeof(buffer));
        if (count < 0) return {errno == EINTR ? InputState::Interrupted : InputState::Error, {}};
#endif
        if (count == 0) return endOfInput();
        pending.append(buffer, static_cast<qsizetype>(count));
    }
    return {};
}

volatile std::sig_atomic_t desktopInterrupted = 0;

void interruptDesktopShell(int signal)
{
    std::signal(signal, interruptDesktopShell);
    desktopInterrupted = 1;
}

#ifdef Q_OS_WIN
bool validHandle(HANDLE handle)
{
    return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

bool bindStream(FILE* stream, DWORD standardHandle, bool input)
{
    const HANDLE handle = GetStdHandle(standardHandle);
    if (!validHandle(handle)) return false;
    int descriptor = _fileno(stream);
    if (descriptor >= 0 && validHandle(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)))) return true;
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return false;
    const int source = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate), input ? _O_RDONLY : _O_WRONLY);
    if (source < 0) {
        CloseHandle(duplicate);
        return false;
    }
    FILE* rebound = nullptr;
    const bool opened = freopen_s(&rebound, "NUL", input ? "rb" : "wb", stream) == 0;
    const bool bound = opened && _dup2(source, _fileno(stream)) == 0;
    _close(source);
    SetStdHandle(standardHandle, handle);
    return bound;
}
#endif

}

bool prepareDesktopShellStdio(QString* error, bool requireInput)
{
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
#ifdef Q_OS_WIN
    const DWORD kinds[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    HANDLE inherited[] = {GetStdHandle(kinds[0]), GetStdHandle(kinds[1]), GetStdHandle(kinds[2])};
    if (!validHandle(inherited[1]) || (requireInput && !validHandle(inherited[0]))) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        for (int index = 0; index < 3; ++index)
            if (validHandle(inherited[index])) SetStdHandle(kinds[index], inherited[index]);
    }
    const bool outputReady = bindStream(stdout, STD_OUTPUT_HANDLE, false);
    bindStream(stderr, STD_ERROR_HANDLE, false);
    if (!outputReady)
        return fail(QStringLiteral("Studio --shell requires standard output. Launch it from a terminal or provide an output pipe."));
    _setmode(_fileno(stdout), _O_BINARY);
    if (requireInput) {
        DWORD mode = 0;
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (!validHandle(input) || (GetFileType(input) == FILE_TYPE_CHAR && !GetConsoleMode(input, &mode))
            || !bindStream(stdin, STD_INPUT_HANDLE, true))
            return fail(QStringLiteral("Studio --shell requires readable standard input. Launch it from a terminal or provide an input pipe."));
        _setmode(_fileno(stdin), _O_BINARY);
    }
#else
    if (fcntl(fileno(stdout), F_GETFL) < 0)
        return fail(QStringLiteral("Studio --shell requires standard output."));
    if (requireInput) {
        struct stat info{};
        const int flags = fcntl(fileno(stdin), F_GETFL);
        if (flags < 0 || (flags & O_ACCMODE) == O_WRONLY || fstat(fileno(stdin), &info) != 0
            || (S_ISCHR(info.st_mode) && !terminalInput()))
            return fail(QStringLiteral("Studio --shell requires readable standard input. Launch it from a terminal or provide an input pipe."));
    }
#endif
    return true;
}

bool shellUsesJson()
{
#ifdef Q_OS_WIN
    DWORD mode = 0;
    const int descriptor = _fileno(stdout);
    return !terminalInput() || descriptor < 0
        || !GetConsoleMode(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)), &mode);
#else
    return !terminalInput() || isatty(fileno(stdout)) == 0;
#endif
}

void writeShellResponse(QJsonObject response, bool json)
{
#ifdef Q_OS_WIN
    const int descriptor = _fileno(stdout);
    if (descriptor < 0 || !validHandle(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)))) return;
#else
    if (fileno(stdout) < 0 || fcntl(fileno(stdout), F_GETFL) < 0) return;
#endif
    response.insert(QStringLiteral("protocolVersion"), 1);
    QByteArray bytes = QJsonDocument(response).toJson(json ? QJsonDocument::Compact : QJsonDocument::Indented);
    if (json) bytes.append('\n');
#ifdef Q_OS_WIN
    DWORD mode = 0;
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(handle, &mode)) {
        const QString text = QString::fromUtf8(bytes);
        DWORD written = 0;
        WriteConsoleW(handle, text.utf16(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
#endif
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fflush(stdout);
}

int runDesktopShell(VNX::IPCServer& dispatcher, bool json)
{
    desktopInterrupted = 0;
    const auto previous = std::signal(SIGINT, interruptDesktopShell);
#ifndef Q_OS_WIN
    const auto previousPipe = std::signal(SIGPIPE, SIG_IGN);
#endif
    StudioSession session(dispatcher, 300000, [] { return desktopInterrupted != 0; });
    const bool jsonOutput = json || shellUsesJson();
    const int result = runShell(session, 300000, jsonOutput, desktopInterrupted,
        [jsonOutput](QJsonObject response) { writeShellResponse(std::move(response), jsonOutput); });
    if (previous != SIG_ERR) std::signal(SIGINT, previous);
#ifndef Q_OS_WIN
    if (previousPipe != SIG_ERR) std::signal(SIGPIPE, previousPipe);
#endif
    return result;
}

int runShell(StudioSession& session, int timeoutMs, bool json,
             volatile std::sig_atomic_t& interrupted,
             const std::function<void(QJsonObject)>& writeResponse)
{
    QEventLoop loop;
    const bool terminal = terminalInput();
    const bool prompts = terminal && !json;
    if (prompts)
        std::fputs("Harken Studio CLI session. Type help for commands, save_project to save, exit to leave.\n", stderr);
    bool executing = false;
    std::atomic_bool stopping = false;
    std::atomic_bool readerFinished = false;
    bool requested = false;
    int exitCode = 0;
    std::mutex inputMutex;
    std::condition_variable inputReady;

    const auto prompt = [&] {
        if (prompts) {
            std::fputs("harken> ", stderr);
            std::fflush(stderr);
        }
    };
    const auto requestInput = [&] {
        if (stopping.load()) return;
        prompt();
        {
            std::lock_guard lock(inputMutex);
            requested = true;
        }
        inputReady.notify_one();
    };
    const auto finish = [&](int code) {
        exitCode = code;
        loop.quit();
    };
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, [&] {
        stopping.store(true);
        inputReady.notify_one();
        finish(0);
    });
    const auto dirty = [&] { return !session.isAttached() && VNX::ProjectManager::instance().hasUnsavedChanges(); };
    const auto unsaved = [] {
        return failure(QStringLiteral("UNSAVED_CHANGES"),
            QStringLiteral("Save with save_project before exiting, or use exit --discard."));
    };
    const auto respond = [&](QJsonObject response, const QString& command) {
        response.insert(QStringLiteral("command"), command);
        writeResponse(std::move(response));
        if (std::ferror(stdout)) {
            stopping.store(true);
            inputReady.notify_one();
            finish(1);
        }
    };
    const auto acceptInput = [&](const Input& input) {
        if (stopping.load()) return;
        if (input.state == InputState::End || input.state == InputState::Error) {
            if (input.state == InputState::Error)
                writeResponse(failure(QStringLiteral("INPUT_FAILED"), QStringLiteral("Cannot read standard input.")));
            else if (dirty())
                writeResponse(unsaved());
            finish(input.state == InputState::Error || dirty() ? 1 : 0);
            return;
        }
        if (input.state == InputState::Interrupted) {
            interrupted = 0;
            requestInput();
            return;
        }
        const QString line = QString::fromUtf8(input.line).trimmed();
        if (line.isEmpty()) {
            requestInput();
            return;
        }
        qsizetype split = 0;
        while (split < line.size() && !line.at(split).isSpace()) ++split;
        const QString command = line.left(split);
        const QString arguments = line.mid(split).trimmed();
        interrupted = 0;
        executing = true;
        QJsonObject response;
        if (command == QStringLiteral("exit")) {
            if (session.isAttached() && !arguments.isEmpty()) {
                response = failure(QStringLiteral("USAGE"),
                    QStringLiteral("Use exit to disconnect. Edits stay in Studio; --discard is only available in independent sessions."));
            } else if (!arguments.isEmpty() && arguments != QStringLiteral("--discard")) {
                response = failure(QStringLiteral("USAGE"), QStringLiteral("Use exit or exit --discard."));
            } else if (arguments.isEmpty() && dirty()) {
                response = unsaved();
            } else {
                respond({{QStringLiteral("status"), QStringLiteral("ok")},
                    {QStringLiteral("result"), QJsonObject{{QStringLiteral("exited"), true},
                        {QStringLiteral("discarded"), dirty()}}}}, command);
                executing = false;
                finish(0);
                return;
            }
        } else if (command == QStringLiteral("help") && arguments.isEmpty()) {
            response = {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("result"), QJsonObject{
                    {QStringLiteral("syntax"), QStringLiteral("command {\"argument\":\"value\"}")},
                    {QStringLiteral("commands"), QJsonArray{
                        QStringLiteral("commands [name] - list commands or show parameters"),
                        QStringLiteral("save_project - save current edits"),
                        session.isAttached()
                            ? QStringLiteral("exit / EOF - disconnect, keeping Studio and its edits open")
                            : QStringLiteral("exit - leave after saving; exit --discard - discard unsaved edits and leave")}}}}};
        } else if (command == QStringLiteral("commands") || command == QStringLiteral("help")) {
            response = arguments.isEmpty() ? session.commands() : session.describe(arguments);
        } else {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(
                arguments.isEmpty() ? QByteArray("{}") : arguments.toUtf8(), &error);
            if (error.error != QJsonParseError::NoError)
                response = failure(QStringLiteral("INVALID_JSON"),
                    QStringLiteral("Invalid JSON at byte %1: %2").arg(error.offset).arg(error.errorString()));
            else if (!document.isObject())
                response = failure(QStringLiteral("INVALID_ARGS"), QStringLiteral("Command arguments must be a JSON object."));
            else if ((command == QStringLiteral("open_project") || command == QStringLiteral("close_project")
                      || command == QStringLiteral("create_project")) && dirty())
                response = failure(QStringLiteral("UNSAVED_CHANGES"),
                    QStringLiteral("Save with save_project before switching projects, or use exit --discard."));
            else
                response = session.execute(command, document.object(), timeoutMs,
                    [&] { return interrupted != 0 || stopping.load(); });
        }
        respond(std::move(response), command);
        executing = false;
        interrupted = 0;
        requestInput();
    };

    std::thread reader([&] {
        QByteArray pending;
        while (true) {
            {
                std::unique_lock lock(inputMutex);
                inputReady.wait(lock, [&] { return requested || stopping.load(); });
                if (stopping.load()) break;
                requested = false;
            }
            auto input = readInput(terminal, pending, stopping);
            if (stopping.load()) break;
            QMetaObject::invokeMethod(&loop, [&, input = std::move(input)] {
                acceptInput(input);
            }, Qt::QueuedConnection);
        }
        readerFinished.store(true);
        inputReady.notify_one();
    });
    QTimer cancellation;
    QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
        if (!executing && interrupted) {
            interrupted = 0;
#ifdef Q_OS_WIN
            if (terminal) return;
#endif
            if (prompts) std::fputc('\n', stderr);
            prompt();
        }
    });
    cancellation.start(25);
    requestInput();
    loop.exec();
    {
        std::lock_guard lock(inputMutex);
        stopping.store(true);
    }
    inputReady.notify_one();
#ifdef Q_OS_WIN
    while (!readerFinished.load()) {
        CancelSynchronousIo(reader.native_handle());
        std::unique_lock lock(inputMutex);
        inputReady.wait_for(lock, std::chrono::milliseconds(10), [&] { return readerFinished.load(); });
    }
#endif
    reader.join();
    return exitCode;
}

}
