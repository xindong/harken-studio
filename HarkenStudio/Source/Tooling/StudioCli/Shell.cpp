#include "Shell.h"
#include "StudioSession.h"
#include "CommandHelp.h"
#include "Core/ProjectManager.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
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

QString shellHelpText(const QString& topic, const StudioSession& session)
{
    if (topic == QStringLiteral("exit")) {
        if (session.isRemote())
            return QStringLiteral("exit - disconnect from Studio and exit this CLI process.\n"
                "Studio and its unsaved edits stay open. EOF also disconnects.\n"
                "exit --discard is unavailable for a connected instance.\n");
        return QStringLiteral("exit - exit this %1 when there are no unsaved edits.\n"
            "Unsaved edits return UNSAVED_CHANGES and keep the shell open; save with save_project.\n"
            "exit --discard - discard unsaved edits and exit.\n").arg(
                session.isAttached() ? QStringLiteral("Studio process, including the GUI") : QStringLiteral("CLI process"))
            + (session.isAttached()
                ? QStringLiteral("EOF requests closing Studio; unsaved edits use the GUI confirmation.\n")
                : QStringLiteral("EOF exits with an error if edits are unsaved.\n"));
    }
    if (topic == QStringLiteral("commands"))
        return QStringLiteral("commands [name] - list available commands or show parameters.\n"
            "commands --search <text> --category <name> --read-only\n"
            "Filters combine; quote text containing spaces. Categories appear in the directory.\n"
            "--search matches command name, description and category; --category matches a category.\n"
            "--read-only shows queries without state changes.\n"
            "Use help with the same filters, or help <name> / <name> --help for details.\n"
            "Examples:\n  commands --search audio\n  commands --search \"project\" --read-only\n");
    return QStringLiteral("Shell command guide\n\n"
        "  help                         Show this guide and all available commands.\n"
        "  help <name>                  Show a command's parameters and constraints.\n"
        "  <name> --help                Show help without executing the command.\n"
        "  commands [name]              List commands or describe one.\n"
        "  help --search <text>         Search names, descriptions and categories.\n"
        "  help --category <name>       Filter by category; quote names containing spaces.\n"
        "  help --read-only             Show only queries. Filters can be combined.\n"
        "  help commands / help exit    Explain filters or session exit behavior.\n\n"
        "Run commands directly: command {\"argument\":\"value\"}\n"
        "Omit the JSON object when no arguments are required. Use save_project to save edits.\n"
        "Examples:\n  get_project_info\n  help create_action\n  help --search audio --read-only\n\n")
        + shellHelpText(QStringLiteral("exit"), session);
}

QJsonObject shellHelp(StudioSession& session, const QString& command, const QString& arguments)
{
    QStringList tokens{command};
    QString token;
    QChar quote;
    bool started = false;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QChar ch = arguments.at(i);
        if (ch == QLatin1Char('\\') && i + 1 < arguments.size()
            && (arguments.at(i + 1) == QLatin1Char('\\')
                || arguments.at(i + 1) == QLatin1Char('"') || arguments.at(i + 1) == QLatin1Char('\''))) {
            token.append(arguments.at(++i));
            started = true;
        } else if (!quote.isNull()) {
            if (ch == quote) quote = QChar();
            else token.append(ch);
        } else if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            quote = ch;
            started = true;
        } else if (ch.isSpace()) {
            if (started) tokens.append(std::exchange(token, {}));
            started = false;
        } else {
            token.append(ch);
            started = true;
        }
    }
    const auto usage = [](const QString& message) {
        auto response = failure(QStringLiteral("USAGE"), message);
        response.insert(QStringLiteral("suggestion"), QStringLiteral("Use help commands for filter syntax, or help <name> for parameters."));
        return response;
    };
    if (!quote.isNull()) return usage(QStringLiteral("Unterminated quote in help arguments."));
    if (started) tokens.append(token);
    const QStringList options{QStringLiteral("--search"), QStringLiteral("--category"),
        QStringLiteral("--read-only"), QStringLiteral("--help"), QStringLiteral("-h")};
    for (qsizetype i = 1; i + 1 < tokens.size(); ++i) {
        if ((tokens.at(i) == QStringLiteral("--search") || tokens.at(i) == QStringLiteral("--category"))
            && options.contains(tokens.at(i + 1).section(QLatin1Char('='), 0, 0)))
            return usage(QStringLiteral("%1 requires a value before %2. Use --option=value for literal option text.")
                .arg(tokens.at(i), tokens.at(i + 1)));
    }
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("search"), QStringLiteral("Search commands."), QStringLiteral("text")});
    parser.addOption({QStringLiteral("category"), QStringLiteral("Filter category."), QStringLiteral("name")});
    parser.addOption({QStringLiteral("read-only"), QStringLiteral("Show queries only.")});
    if (!parser.parse(tokens)) return usage(parser.errorText());
    for (const auto& option : {QStringLiteral("search"), QStringLiteral("category")}) {
        if (parser.values(option).size() > 1) return usage(QStringLiteral("--%1 may only be specified once.").arg(option));
        if (parser.isSet(option) && parser.value(option).trimmed().isEmpty())
            return usage(QStringLiteral("--%1 requires a nonempty value.").arg(option));
    }
    const auto names = parser.positionalArguments();
    const bool filtered = parser.isSet(QStringLiteral("search")) || parser.isSet(QStringLiteral("category"))
        || parser.isSet(QStringLiteral("read-only"));
    if (names.size() > 1 || (!names.isEmpty() && filtered))
        return usage(QStringLiteral("Use one command name, or directory filters without a command name."));
    const QString topic = names.isEmpty() ? command : names.front();
    if ((names.isEmpty() && parser.isSet(QStringLiteral("help"))) || (!names.isEmpty()
        && (topic == QStringLiteral("help") || topic == QStringLiteral("commands") || topic == QStringLiteral("exit"))))
        return {{QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("result"), QJsonObject{{QStringLiteral("help"), shellHelpText(topic, session)}}}};
    if (!names.isEmpty()) return session.describe(topic);
    auto response = filterCommandDirectory(session.commands(), parser.value(QStringLiteral("search")),
        parser.value(QStringLiteral("category")), parser.isSet(QStringLiteral("read-only")));
    if (succeeded(response) && command == QStringLiteral("help") && !filtered) {
        auto result = response.value(QStringLiteral("result")).toObject();
        result.insert(QStringLiteral("help"), shellHelpText(QStringLiteral("help"), session));
        result.insert(QStringLiteral("syntax"), QStringLiteral("command {\"argument\":\"value\"}"));
        response.insert(QStringLiteral("result"), result);
    }
    return response;
}

QByteArray shellResponseBytes(QJsonObject response, bool json)
{
    response.insert(QStringLiteral("protocolVersion"), 1);
    QByteArray bytes;
    const auto result = response.value(QStringLiteral("result")).toObject();
    if (!json && succeeded(response)) {
        if (result.value(QStringLiteral("help")).isString())
            bytes.append(result.value(QStringLiteral("help")).toString().toUtf8());
        if (result.contains(QStringLiteral("inputSchema")))
            bytes.append(commandDescriptionText(result, true).toUtf8());
        else if (result.contains(QStringLiteral("host")) && result.value(QStringLiteral("commands")).isArray())
            bytes.append(commandDirectoryText(result.value(QStringLiteral("commands")).toArray(), true).toUtf8());
    }
    if (bytes.isEmpty()) {
        bytes = QJsonDocument(response).toJson(json ? QJsonDocument::Compact : QJsonDocument::Indented);
        if (json) bytes.append('\n');
    }
    return bytes;
}

bool writeDesktopOutput(const QByteArray& bytes, int timeoutMs)
{
    QEventLoop loop;
    std::atomic_bool stopping = false;
    std::atomic_bool finished = false;
    bool succeeded = false;
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
        &loop, [&] { stopping.store(true); loop.quit(); });
    std::thread writer([&] {
        const auto write = [&]() {
#ifdef Q_OS_WIN
            const int descriptor = _fileno(stdout);
            if (descriptor < 0) return false;
            const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
            DWORD mode = 0;
            if (GetConsoleMode(handle, &mode)) {
                const QString text = QString::fromUtf8(bytes);
                qsizetype offset = 0;
                while (offset < text.size() && !stopping.load()) {
                    DWORD written = 0;
                    if (!WriteConsoleW(handle, text.utf16() + offset,
                            static_cast<DWORD>(qMin<qsizetype>(4096, text.size() - offset)), &written, nullptr)
                        || written == 0) return false;
                    offset += written;
                }
                return offset == text.size();
            }
#else
            const int descriptor = fileno(stdout);
            const int flags = fcntl(descriptor, F_GETFL);
            if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) return false;
            struct RestoreFlags {
                int descriptor;
                int flags;
                ~RestoreFlags() { fcntl(descriptor, F_SETFL, flags); }
            } restore{descriptor, flags};
#endif
            qsizetype offset = 0;
            while (offset < bytes.size() && !stopping.load()) {
#ifdef Q_OS_WIN
                DWORD written = 0;
                if (!WriteFile(handle, bytes.constData() + offset,
                        static_cast<DWORD>(qMin<qsizetype>(4096, bytes.size() - offset)), &written, nullptr)
                    || written == 0) return false;
#else
                pollfd output{descriptor, POLLOUT, 0};
                const int ready = poll(&output, 1, 25);
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0 || (output.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;
                if (ready == 0) continue;
                const auto written = ::write(descriptor, bytes.constData() + offset,
                    static_cast<size_t>(qMin<qsizetype>(4096, bytes.size() - offset)));
                if (written < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if (written <= 0) return false;
#endif
                offset += static_cast<qsizetype>(written);
            }
            return offset == bytes.size();
        };
        succeeded = write();
        finished.store(true);
    });
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (desktopInterrupted || elapsed.elapsed() >= timeoutMs) stopping.store(true);
#ifdef Q_OS_WIN
        if (stopping.load()) CancelSynchronousIo(writer.native_handle());
#endif
        if (finished.load()) loop.quit();
    });
    poll.start(10);
    if (!finished.load()) loop.exec();
    stopping.store(true);
#ifdef Q_OS_WIN
    while (!finished.load()) {
        CancelSynchronousIo(writer.native_handle());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
    writer.join();
    return succeeded;
}

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
    const auto bytes = shellResponseBytes(std::move(response), json);
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
#ifndef Q_OS_WIN
    const auto previousPipe = std::signal(SIGPIPE, SIG_IGN);
#endif
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fflush(stdout);
#ifndef Q_OS_WIN
    if (previousPipe != SIG_ERR) std::signal(SIGPIPE, previousPipe);
#endif
}

DesktopShellResult runDesktopShell(VNX::IPCServer& dispatcher, bool json,
    const std::function<QJsonObject(ShellExitRequest)>& requestClose)
{
    desktopInterrupted = 0;
    const auto previous = std::signal(SIGINT, interruptDesktopShell);
#ifndef Q_OS_WIN
    const auto previousPipe = std::signal(SIGPIPE, SIG_IGN);
#endif
    bool quitting = false;
    QObject lifetime;
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
        &lifetime, [&] { quitting = true; });
    StudioSession session(dispatcher, 300000, [] { return desktopInterrupted != 0; });
    const bool jsonOutput = json || shellUsesJson();
    bool outputFailed = false;
    DesktopShellResult result;
    result.exitCode = runShell(session, 300000, jsonOutput, desktopInterrupted,
        [&](QJsonObject response) {
            if (quitting) {
                outputFailed = true;
                return;
            }
            auto bytes = shellResponseBytes(response, jsonOutput);
            if (bytes.size() > 64 * 1024 * 1024) {
                auto error = failure(QStringLiteral("RESPONSE_TOO_LARGE"),
                    QStringLiteral("Shell response exceeds 64 MiB. Narrow the query."));
                error.insert(QStringLiteral("command"), response.value(QStringLiteral("command")));
                bytes = shellResponseBytes(error, jsonOutput);
            }
            outputFailed = !writeDesktopOutput(bytes, result.exitRequest == ShellExitRequest::None ? 300000 : 5000);
        }, &result.exitRequest, requestClose, [&] { return outputFailed; });
#ifndef Q_OS_WIN
    if (previousPipe != SIG_ERR) std::signal(SIGPIPE, previousPipe);
#endif
    if (previous != SIG_ERR) std::signal(SIGINT, previous);
    return result;
}

int runShell(StudioSession& session, int timeoutMs, bool json,
             volatile std::sig_atomic_t& interrupted,
             const std::function<void(QJsonObject)>& writeResponse,
             ShellExitRequest* exitRequest,
             const std::function<QJsonObject(ShellExitRequest)>& requestClose,
             const std::function<bool()>& outputFailed)
{
    if (exitRequest) *exitRequest = ShellExitRequest::None;
    QEventLoop loop;
    const bool terminal = terminalInput();
#ifdef Q_OS_WIN
    const HANDLE consoleInput = terminal
        ? reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdin))) : INVALID_HANDLE_VALUE;
    DWORD consoleMode = 0;
    if (terminal && (!GetConsoleMode(consoleInput, &consoleMode)
        || !SetConsoleMode(consoleInput,
            (consoleMode | ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)
                & ~ENABLE_VIRTUAL_TERMINAL_INPUT))) {
        writeResponse(failure(QStringLiteral("INPUT_FAILED"),
            QStringLiteral("Cannot configure the terminal for command input.")));
        return 1;
    }
#endif
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
    const auto dirty = [&] { return !session.isRemote() && VNX::ProjectManager::instance().hasUnsavedChanges(); };
    const auto unsaved = [] {
        return failure(QStringLiteral("UNSAVED_CHANGES"),
            QStringLiteral("Save with save_project before exiting, or use exit --discard."));
    };
    const auto respond = [&](QJsonObject response, const QString& command) {
        response.insert(QStringLiteral("command"), command);
        if (response.value(QStringLiteral("code")) == QStringLiteral("UNKNOWN_COMMAND"))
            response.insert(QStringLiteral("suggestion"), QStringLiteral("Use help to list commands, or help --search <text> to find one."));
        writeResponse(std::move(response));
        if (std::ferror(stdout) || (outputFailed && outputFailed())) {
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
            else if (dirty() && !exitRequest)
                writeResponse(unsaved());
            if (input.state == InputState::End && exitRequest) {
                const auto response = requestClose(ShellExitRequest::EndOfInput);
                if (succeeded(response))
                    *exitRequest = ShellExitRequest::EndOfInput;
                else
                    respond(response, QStringLiteral("exit"));
            }
            finish(input.state == InputState::Error || (!exitRequest && dirty()) ? 1 : 0);
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
        if (command == QStringLiteral("help") || command == QStringLiteral("commands")) {
            response = shellHelp(session, command, arguments);
        } else if (arguments == QStringLiteral("--help") || arguments == QStringLiteral("-h")) {
            response = command == QStringLiteral("exit")
                ? shellHelp(session, QStringLiteral("help"), command) : session.describe(command);
        } else if (command == QStringLiteral("exit")) {
            if (session.isRemote() && !arguments.isEmpty()) {
                response = failure(QStringLiteral("USAGE"),
                    QStringLiteral("Use exit to disconnect. Edits stay in Studio; --discard is only available when the shell owns the project."));
            } else if (!arguments.isEmpty() && arguments != QStringLiteral("--discard")) {
                response = failure(QStringLiteral("USAGE"), QStringLiteral("Use exit or exit --discard."));
            } else if (arguments.isEmpty() && dirty()) {
                response = unsaved();
            } else {
                if (exitRequest) {
                    const auto request = arguments == QStringLiteral("--discard")
                        ? ShellExitRequest::Discard : ShellExitRequest::Close;
                    response = requestClose(request);
                    if (!succeeded(response)) {
                        respond(std::move(response), command);
                        executing = false;
                        interrupted = 0;
                        requestInput();
                        return;
                    }
                    *exitRequest = request;
                    respond(std::move(response), command);
                } else {
                    respond({{QStringLiteral("status"), QStringLiteral("ok")},
                        {QStringLiteral("result"), QJsonObject{{QStringLiteral("exited"), true},
                            {QStringLiteral("discarded"), dirty()}}}}, command);
                }
                executing = false;
                finish(0);
                return;
            }
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
#ifdef Q_OS_WIN
    if (terminal && !SetConsoleMode(consoleInput, consoleMode))
        std::fputs("Cannot restore the terminal input mode.\n", stderr);
#endif
    return exitCode;
}

}
