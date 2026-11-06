#include "Shell.h"
#include "StudioSession.h"
#include "Core/ProjectManager.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#include <io.h>
#include <qt_windows.h>
#else
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
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

Input readInput(bool terminal)
{
#ifdef Q_OS_WIN
    if (terminal) {
        QString line;
        while (true) {
            wchar_t buffer[256];
            DWORD count = 0;
            if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), buffer, 256, &count, nullptr))
                return {GetLastError() == ERROR_OPERATION_ABORTED
                    ? InputState::Interrupted : InputState::Error, {}};
            if (count == 0) return {InputState::Interrupted, {}};
            line.append(QString::fromWCharArray(buffer, static_cast<qsizetype>(count)));
            if (line.startsWith(QChar(26))) return {};
            if (line.endsWith(QLatin1Char('\n')))
                return {InputState::Line, line.toUtf8()};
        }
    }
#else
    Q_UNUSED(terminal);
#endif
    std::string line;
    errno = 0;
    if (std::getline(std::cin, line))
        return {InputState::Line, QByteArray(line.data(), static_cast<qsizetype>(line.size()))};
    if (errno == EINTR) {
        std::cin.clear();
        return {InputState::Interrupted, {}};
    }
    return {std::cin.eof() ? InputState::End : InputState::Error, {}};
}

}

int runShell(StudioSession& session, int timeoutMs, bool json,
             volatile std::sig_atomic_t& interrupted,
             const std::function<void(QJsonObject)>& writeResponse)
{
    QEventLoop loop;
    const bool terminal = terminalInput();
    const bool prompts = terminal && !json;
    bool executing = false;
    bool stopping = false;
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
    const auto dirty = [] { return VNX::ProjectManager::instance().hasUnsavedChanges(); };
    const auto unsaved = [] {
        return failure(QStringLiteral("UNSAVED_CHANGES"),
            QStringLiteral("Save with save_project before exiting, or use exit --discard."));
    };
    const auto respond = [&](QJsonObject response, const QString& command) {
        response.insert(QStringLiteral("command"), command);
        writeResponse(std::move(response));
    };
    const auto acceptInput = [&](const Input& input) {
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
            if (!arguments.isEmpty() && arguments != QStringLiteral("--discard")) {
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
                        QStringLiteral("exit - leave after saving"),
                        QStringLiteral("exit --discard - discard unsaved edits and leave")}}}}};
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
                    [&] { return interrupted != 0; });
        }
        respond(std::move(response), command);
        executing = false;
        interrupted = 0;
        requestInput();
    };

    std::thread reader([&] {
        while (true) {
            {
                std::unique_lock lock(inputMutex);
                inputReady.wait(lock, [&] { return requested || stopping; });
                if (stopping) return;
                requested = false;
            }
            auto input = readInput(terminal);
            QMetaObject::invokeMethod(&loop, [&, input = std::move(input)] {
                acceptInput(input);
            }, Qt::QueuedConnection);
        }
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
        stopping = true;
    }
    inputReady.notify_one();
    reader.join();
    return exitCode;
}

}
