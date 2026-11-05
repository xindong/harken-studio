#include "StudioSession.h"
#include "harken_buildinfo.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

#include <csignal>
#include <cstdio>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

volatile std::sig_atomic_t interrupted = 0;
bool verbose = false;

void interrupt(int)
{
    interrupted = 1;
}

void messageHandler(QtMsgType type, const QMessageLogContext&, const QString& message)
{
    if (type == QtDebugMsg && !verbose) return;
    const QByteArray bytes = message.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
    std::fputc('\n', stderr);
}

void print(const QByteArray& bytes, FILE* destination = stdout)
{
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), destination);
    std::fflush(destination);
}

int output(QJsonObject response, bool json, int errorExit = 1)
{
    response.insert(QStringLiteral("protocolVersion"), 1);
    print(QJsonDocument(response).toJson(json ? QJsonDocument::Compact : QJsonDocument::Indented));
    if (json) print("\n");
    if (HarkenStudioCli::succeeded(response)) return 0;
    const QString code = response.value(QStringLiteral("code")).toString();
    if (!json) print((code + QStringLiteral(": ") + response.value(QStringLiteral("message")).toString()
                     + QLatin1Char('\n')).toUtf8(), stderr);
    if (code == QStringLiteral("CANCELED")) return 130;
    if (code == QStringLiteral("TIMEOUT")) return 124;
    return errorExit;
}

bool loadJson(const QCommandLineParser& parser, QJsonDocument* document, QString* error)
{
    QByteArray bytes;
    if (parser.isSet(QStringLiteral("args")) && parser.isSet(QStringLiteral("input"))) {
        *error = QStringLiteral("Use either --args or --input, not both.");
        return false;
    }
    if (parser.isSet(QStringLiteral("input"))) {
        const QString path = parser.value(QStringLiteral("input"));
        QFile file;
        if (path == QStringLiteral("-")) {
            if (!file.open(stdin, QIODevice::ReadOnly)) {
                *error = QStringLiteral("Cannot read standard input.");
                return false;
            }
        } else {
            file.setFileName(path);
            if (!file.open(QIODevice::ReadOnly)) {
                *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
                return false;
            }
        }
        bytes = file.readAll();
    } else {
        bytes = parser.isSet(QStringLiteral("args"))
            ? parser.value(QStringLiteral("args")).toUtf8() : QByteArray("{}");
    }
    QJsonParseError parseError;
    *document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("Invalid JSON at byte %1: %2").arg(parseError.offset).arg(parseError.errorString());
        return false;
    }
    return true;
}

}

int main(int argc, char* argv[])
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("HarkenStudioCLI"));
    app.setOrganizationName(QStringLiteral("HarkenSound"));
    app.setApplicationVersion(QString::fromUtf8(Harken::BuildInfo::Version.data(),
        static_cast<qsizetype>(Harken::BuildInfo::Version.size())));
    qInstallMessageHandler(messageHandler);
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Headless Harken Studio authoring. No desktop window or running editor is required.\n\n"
        "Commands:\n"
        "  commands [name]    Discover editor commands or describe one command's parameters.\n"
        "  init <directory>   Create a new project in a directory that does not exist.\n"
        "  exec <name>        Execute a registered editor command.\n"
        "  run                Execute a JSON array of {cmd,args} requests in order.\n\n"
        "Use --save to commit project edits before exiting; otherwise edits remain in memory.\n"
        "Creation and external operations such as export write their own outputs.\n"
        "A batch stops on its first error and is not an atomic transaction.\n\n"
        "Examples:\n"
        "  HarkenStudioCLI commands set_item_property --json\n"
        "  HarkenStudioCLI init ./GameAudio --json\n"
        "  HarkenStudioCLI exec list_items --project ./GameAudio/GameAudio.hsp --json\n"
        "  HarkenStudioCLI run --input workflow.json --save --json\n"
        "  HarkenStudioCLI exec export_pack --project ./GameAudio/GameAudio.hsp --input export.json --json\n\n"
        "Exit codes: 0 success, 1 execution failure, 2 invalid usage/input, 124 timeout, 130 interruption."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("commands, init, exec or run"));
    parser.addPositionalArgument(QStringLiteral("name"), QStringLiteral("Editor command name or project directory"), QStringLiteral("[name]"));
    parser.addOption({QStringLiteral("project"), QStringLiteral("Open this .hsp project before execution."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("args"), QStringLiteral("Command arguments as a JSON object."), QStringLiteral("json")});
    parser.addOption({QStringList{QStringLiteral("i"), QStringLiteral("input")}, QStringLiteral("Read UTF-8 JSON from a file, or - for stdin."), QStringLiteral("file")});
    parser.addOption({QStringLiteral("json"), QStringLiteral("Write compact JSON without human-oriented formatting.")});
    parser.addOption({QStringLiteral("save"), QStringLiteral("Save the active project after successful execution.")});
    parser.addOption({QStringLiteral("timeout"), QStringLiteral("Maximum wait for an asynchronous operation, in seconds."), QStringLiteral("seconds"), QStringLiteral("300")});
    parser.addOption({QStringLiteral("verbose"), QStringLiteral("Include debug diagnostics on stderr.")});

    const bool wantsJson = app.arguments().contains(QStringLiteral("--json"));
    if (!parser.parse(app.arguments()))
        return output(HarkenStudioCli::failure(QStringLiteral("USAGE"), parser.errorText()), wantsJson, 2);
    verbose = parser.isSet(QStringLiteral("verbose"));
    if (parser.isSet(QStringLiteral("version"))) {
        if (wantsJson) return output({{"status", "ok"}, {"result", QJsonObject{{"version", app.applicationVersion()}}}}, true);
        print((app.applicationName() + QLatin1Char(' ') + app.applicationVersion() + QLatin1Char('\n')).toUtf8());
        return 0;
    }
    const QStringList positional = parser.positionalArguments();
    if (parser.isSet(QStringLiteral("help")) || parser.isSet(QStringLiteral("help-all")) || positional.isEmpty()) {
        print(parser.helpText().toUtf8());
        return 0;
    }
    const auto usage = [&](const QString& message) {
        return output(HarkenStudioCli::failure(QStringLiteral("USAGE"), message), wantsJson, 2);
    };
    const QString command = positional.front();
    if (command != QStringLiteral("commands") && command != QStringLiteral("init")
        && command != QStringLiteral("exec") && command != QStringLiteral("run"))
        return usage(QStringLiteral("Unknown command '%1'. Run HarkenStudioCLI --help.").arg(command));
    if (positional.size() > 2 || ((command == QStringLiteral("init") || command == QStringLiteral("exec")) && positional.size() != 2)
        || (command == QStringLiteral("run") && positional.size() != 1))
        return usage(QStringLiteral("Invalid positional arguments. Run HarkenStudioCLI --help."));
    bool validTimeout = false;
    const int seconds = parser.value(QStringLiteral("timeout")).toInt(&validTimeout);
    if (!validTimeout || seconds < 1 || seconds > 86400)
        return usage(QStringLiteral("--timeout must be an integer from 1 to 86400."));

    HarkenStudioCli::StudioSession session;
    if (command == QStringLiteral("commands")) {
        const auto response = positional.size() == 2 ? session.describe(positional.at(1)) : session.commands();
        if (wantsJson || positional.size() == 2) return output(response, wantsJson);
        for (const auto& value : response.value("result").toObject().value("commands").toArray()) {
            const auto entry = value.toObject();
            print((entry.value("command").toString() + (entry.value("requiresDesktop").toBool() ? QStringLiteral(" [desktop]") : QString())
                + QStringLiteral("  ") + entry.value("description").toString() + QLatin1Char('\n')).toUtf8());
        }
        return 0;
    }
    if (command == QStringLiteral("init")) {
        if (parser.isSet(QStringLiteral("project")) || parser.isSet(QStringLiteral("input")) || parser.isSet(QStringLiteral("args")))
            return usage(QStringLiteral("init takes a new directory; use exec create_project for advanced project arguments."));
        const QFileInfo directory(QDir::cleanPath(positional.at(1)));
        return output(session.execute(QStringLiteral("create_project"),
            {{QStringLiteral("name"), directory.fileName()}, {QStringLiteral("location"), directory.absolutePath()}},
            seconds * 1000, [] { return interrupted != 0; }), wantsJson);
    }
    QJsonDocument input;
    QString inputError;
    if (!loadJson(parser, &input, &inputError)) return usage(inputError);
    if (command == QStringLiteral("exec") && !input.isObject()) return usage(QStringLiteral("exec requires a JSON object."));
    if (command == QStringLiteral("run") && !input.isArray()) return usage(QStringLiteral("run requires a JSON array of {cmd,args} requests."));
    if (command == QStringLiteral("run")) {
        for (const auto& value : input.array()) {
            const auto request = value.toObject();
            if (!value.isObject() || !request.value(QStringLiteral("cmd")).isString()
                || request.value(QStringLiteral("cmd")).toString().isEmpty()
                || (request.contains(QStringLiteral("args")) && !request.value(QStringLiteral("args")).isObject()))
                return usage(QStringLiteral("Each request must contain a nonempty cmd string and an optional args object."));
        }
    }
    if (parser.isSet(QStringLiteral("project"))) {
        const auto opened = session.open(parser.value(QStringLiteral("project")));
        if (!HarkenStudioCli::succeeded(opened)) return output(opened, wantsJson);
    }
    QJsonObject response;
    if (command == QStringLiteral("exec")) {
        response = session.execute(positional.at(1), input.object(), seconds * 1000, [] { return interrupted != 0; });
    } else {
        QJsonArray results;
        response = {{QStringLiteral("status"), QStringLiteral("ok")}};
        for (const auto& value : input.array()) {
            const auto request = value.toObject();
            auto result = session.execute(request.value("cmd").toString(), request.value("args").toObject(),
                seconds * 1000, [] { return interrupted != 0; });
            result.insert(QStringLiteral("command"), request.value("cmd"));
            result.insert(QStringLiteral("index"), results.size());
            results.append(result);
            if (!HarkenStudioCli::succeeded(result)) {
                response = result;
                break;
            }
        }
        response.insert(QStringLiteral("results"), results);
    }
    if (HarkenStudioCli::succeeded(response) && parser.isSet(QStringLiteral("save"))) {
        const auto saved = session.save();
        if (!HarkenStudioCli::succeeded(saved)) {
            response.insert(QStringLiteral("status"), QStringLiteral("error"));
            response.insert(QStringLiteral("code"), QStringLiteral("SAVE_FAILED"));
            response.insert(QStringLiteral("message"), QStringLiteral("Execution completed, but saving the project failed."));
            response.insert(QStringLiteral("save"), saved);
        } else {
            response.insert(QStringLiteral("saved"), true);
        }
    }
    return output(response, wantsJson);
}
