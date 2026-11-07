#include "StudioSession.h"
#include "SavedProjectBuild.h"
#include "ResourceWorker.h"
#include "Shell.h"
#include "CommandHelp.h"
#include "Core/IPC/StudioInstanceTransport.h"
#include "harken_buildinfo.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>

#include <csignal>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

volatile std::sig_atomic_t interrupted = 0;
bool verbose = false;

void interrupt(int signal)
{
    std::signal(signal, interrupt);
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
    if (!json && response.contains(QStringLiteral("suggestion")))
        print((response.value(QStringLiteral("suggestion")).toString() + QLatin1Char('\n')).toUtf8(), stderr);
    if (code == QStringLiteral("CANCELED")) return 130;
    if (code == QStringLiteral("TIMEOUT")) return 124;
    if (code == QStringLiteral("INVALID_ARGUMENTS")) return 2;
    return errorExit;
}

int describeCommand(const QJsonObject& response, bool json)
{
    if (json || !HarkenStudioCli::succeeded(response)) return output(response, json);
    print(HarkenStudioCli::commandDescriptionText(response.value(QStringLiteral("result")).toObject()).toUtf8());
    return 0;
}

void printCommandDirectory(const QJsonArray& entries)
{
    print(HarkenStudioCli::commandDirectoryText(entries).toUtf8());
}

QString commandHelp(const QString& command)
{
    static const QMap<QString, QString> help{
        {QStringLiteral("instances"), QStringLiteral(
            "Usage: HarkenStudioCLI instances [--json]\n\n"
            "List live Studio instances of all software versions.\n"
            "Each entry includes its id, project, process, executable, version and availability.\n"
            "Only instances with the same software version as this CLI can execute commands.\n"
            "Use --instance <id> with commands, exec, run or shell to target that session.\n")},
        {QStringLiteral("commands"), QStringLiteral(
            "Usage: HarkenStudioCLI commands [name] [--json]\n"
            "       HarkenStudioCLI commands [--search <text>] [--category <name>] [--read-only] [--json]\n\n"
            "List commands or explain a command's parameters and constraints. Filters combine.\n"
            "  --instance <id>   Discover the selected Studio's shared and GUI commands.\n"
            "  --search <text>    Match command name, description or category (case insensitive).\n"
            "  --category <name> Match the category shown in command details.\n"
            "  --read-only       Show queries without state changes.\n"
            "  --json            Return the complete machine-readable contract.\n\n"
            "Examples:\n  HarkenStudioCLI commands --search audio\n"
            "  HarkenStudioCLI commands set_item_property\n")},
        {QStringLiteral("exec"), QStringLiteral(
            "Usage: HarkenStudioCLI exec <name> [--project <path>] [--input <file|-> | --args <json>] [--save] [--json]\n\n"
            "Execute one command with a JSON argument object; omitted arguments mean {}.\n"
            "  --project <path>  Open a project file or directory before execution.\n"
            "  --instance <id>   Operate on a running Studio instead; incompatible with --project.\n"
            "  --input <file|->  Read UTF-8 JSON from a file or stdin (-).\n"
            "  --args <json>     Inline JSON; shell quoting rules apply.\n"
            "  --save            Save the active project only after successful execution.\n"
            "  --timeout <sec>   Per-operation wait, 1-86400 seconds (default 300).\n"
            "  --json            Write one compact JSON response to stdout.\n"
            "  --verbose         Include debug diagnostics on stderr.\n\n"
            "Use exec <name> --help to see its parameters.\n"
            "Independent sessions discard unsaved edits on exit; instance edits remain in Studio.\n"
            "Exports and other external writes still take effect.\n\n"
            "Example: HarkenStudioCLI exec list_items --project ./GameAudio --json\n")},
        {QStringLiteral("run"), QStringLiteral(
            "Usage: HarkenStudioCLI run --input <file|-> [--project <path>] [--save] [--json]\n\n"
            "Read a JSON array of {\"cmd\":\"name\",\"args\":{...}} requests.\n"
            "All command names and argument contracts are checked before opening or modifying a project.\n"
            "Requests then run in order in one session; runtime errors stop execution.\n"
            "  --args <json>     Supply the request array inline instead of --input.\n"
            "  --project <path>  Open a project file or directory after validation.\n"
            "  --instance <id>   Run in a selected Studio instead; edits remain after CLI exit.\n"
            "  --save            Save only if every request succeeds.\n"
            "  --timeout <sec>   Per-operation wait, 1-86400 seconds (default 300).\n"
            "  --json            Write one compact response with per-request results.\n"
            "  --verbose         Include debug diagnostics on stderr.\n\n"
            "This is not an atomic transaction: earlier saves and exports survive later runtime failures.\n"
            "Example: HarkenStudioCLI run --project ./GameAudio --input workflow.json --save --json\n")},
        {QStringLiteral("init"), QStringLiteral(
            "Usage: HarkenStudioCLI init <directory> [--timeout <sec>] [--json] [--verbose]\n\n"
            "Create and save a project in a directory that does not exist.\n"
            "The directory name becomes the project name. Existing directories are never overwritten.\n"
            "--timeout is the per-operation wait, 1-86400 seconds (default 300).\n\n"
            "Example: HarkenStudioCLI init ./GameAudio --json\n")},
        {QStringLiteral("shell"), QStringLiteral(
            "Usage: HarkenStudioCLI shell [--project <path>] [--timeout <sec>] [--json] [--verbose]\n\n"
            "Keep a project and runtime session open, or use --instance <id> to connect to Studio.\n"
            "--instance and --project are mutually exclusive. Enter one command per line:\n"
            "  command {\"argument\":\"value\"}\n"
            "  help              Show shell usage and the current command directory.\n"
            "  help <name>       Explain a command's parameters and constraints.\n"
            "  <name> --help     Explain the command without executing it.\n"
            "  commands [name]   List commands or describe one.\n"
            "  commands --search <text> [--category <name>] [--read-only]\n"
            "                    Filter the directory; help accepts the same filters.\n"
            "  save_project      Save changes explicitly.\n"
            "  exit              Exit when no unsaved changes remain.\n"
            "  exit --discard    Discard unsaved edits and exit.\n\n"
            "Redirected input or output automatically uses one JSON response per line.\n"
            "A command error leaves the session open; inspect each response's status.\n"
            "Independent sessions: EOF fails if edits are unsaved.\n"
            "Instance sessions: exit or EOF disconnects; Studio and its edits stay open; --discard is rejected.\n"
            "Ctrl+C stops waiting when supported; submitted instance commands may still complete.\n"
            "--timeout is the per-operation wait, 1-86400 seconds (default 300).\n")},
        {QStringLiteral("list-targets"), QStringLiteral(
            "Usage: HarkenStudioCLI list-targets --project <project.hsp> [--json] [--verbose]\n\n"
            "Read platform UUIDs, languages and Actions from a saved project without editing it.\n"
            "The project must be closed in other Studio and CLI sessions.\n"
            "Use the returned platform UUIDs with build --platform-id.\n")},
        {QStringLiteral("build"), QStringLiteral(
            "Usage: HarkenStudioCLI build --project <project.hsp> --all --platform-id <UUID> --self-clean <on|off>\n"
            "                             [--output <directory>] [--force] [--json] [--verbose]\n\n"
            "Build all Actions from a saved project without changing authored project files.\n"
            "  --platform-id <UUID>  Required; repeat for multiple project platforms.\n"
            "  --self-clean on|off   Required; remove obsolete outputs only after successful builds.\n"
            "  --output <directory> Output directory; relative paths use the project directory.\n"
            "  --force              Rebuild instead of reusing valid outputs.\n"
            "  --json               Write one compact JSON response.\n"
            "  --verbose            Include debug diagnostics on stderr.\n\n"
            "Discover UUIDs with list-targets --project <project.hsp>.\n"
            "The project must be closed in other sessions. Build writes packs, caches and temporary locks.\n")}
    };
    return help.value(command);
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
    if (argc == 2 && std::strcmp(argv[1], "--resource-worker") == 0)
        return Harkenizer::runResourceWorker();
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
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsCompactedShortOptions);
    parser.setApplicationDescription(QStringLiteral(
        "Harken Studio commands: author independently or control a running Studio instance.\n\n"
        "Commands:\n"
        "  commands [name]    Discover CLI commands or describe one command's parameters.\n"
        "  instances          Find running Studio instances and report version compatibility.\n"
        "  init <directory>   Create a new project in a directory that does not exist.\n"
        "  exec <name>        Execute a registered editor command.\n"
        "  run                Execute a JSON array of {cmd,args} requests in order.\n"
        "  shell              Keep an interactive authoring session open.\n"
        "  list-targets       List saved project platforms, languages and Actions.\n"
        "  build              Build packs from a saved project without editing it.\n\n"
        "Use --instance <id> with commands, exec, run or shell to operate a running Studio.\n"
        "Instance edits remain in Studio after CLI exit; --save persists them to disk.\n"
        "Without --instance, exec/run discard unsaved edits on exit; use --save to persist them.\n"
        "Creation and external operations such as export write their own outputs.\n"
        "A batch stops on its first error and is not an atomic transaction.\n"
        "build and list-targets require --project and read the saved project without upgrading or recovering it.\n"
        "build requires --all, one or more --platform-id values, and --self-clean on|off.\n\n"
        "Examples:\n"
        "  HarkenStudioCLI instances --json\n"
        "  HarkenStudioCLI --instance <id> exec get_project_info --json\n"
        "  HarkenStudioCLI commands set_item_property\n"
        "  HarkenStudioCLI commands --search audio\n"
        "  HarkenStudioCLI exec set_item_property --help\n"
        "  HarkenStudioCLI init ./GameAudio --json\n"
        "  HarkenStudioCLI exec list_items --project ./GameAudio/GameAudio.hsp --json\n"
        "  HarkenStudioCLI run --input workflow.json --save --json\n"
        "  HarkenStudioCLI shell --project ./GameAudio/GameAudio.hsp\n"
        "  HarkenStudioCLI exec export_pack --project ./GameAudio/GameAudio.hsp --input export.json --json\n"
        "  HarkenStudioCLI list-targets --project ./GameAudio/GameAudio.hsp --json\n"
        "  HarkenStudioCLI build --project ./GameAudio/GameAudio.hsp --all --platform-id <UUID> --self-clean on --json\n\n"
        "Exit codes: 0 success, 1 execution failure, 2 invalid usage/input, 124 timeout, 130 interruption."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("instances, commands, init, exec, run, shell, list-targets or build"));
    parser.addPositionalArgument(QStringLiteral("name"), QStringLiteral("Editor command name or project directory"), QStringLiteral("[name]"));
    parser.addOption({QStringLiteral("project"), QStringLiteral("Open this .hsp project before execution."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("instance"), QStringLiteral("Execute in this running Studio instance instead of an independent session."), QStringLiteral("id")});
    parser.addOption({QStringLiteral("args"), QStringLiteral("Inline JSON: an object for exec or a request array for run."), QStringLiteral("json")});
    parser.addOption({QStringList{QStringLiteral("i"), QStringLiteral("input")}, QStringLiteral("Read UTF-8 JSON from a file, or - for stdin."), QStringLiteral("file")});
    parser.addOption({QStringLiteral("json"), QStringLiteral("Write compact JSON without human-oriented formatting.")});
    parser.addOption({QStringLiteral("save"), QStringLiteral("Save the active project after successful execution.")});
    parser.addOption({QStringLiteral("timeout"), QStringLiteral("Maximum wait for an asynchronous operation, in seconds."), QStringLiteral("seconds"), QStringLiteral("300")});
    parser.addOption({QStringLiteral("verbose"), QStringLiteral("Include debug diagnostics on stderr.")});
    parser.addOption({QStringLiteral("all"), QStringLiteral("Build every Action for the requested platforms.")});
    parser.addOption({QStringLiteral("platform-id"), QStringLiteral("Build project platform UUID; may be repeated."), QStringLiteral("UUID")});
    parser.addOption({QStringLiteral("self-clean"), QStringLiteral("Remove outputs outside this build: on or off."), QStringLiteral("on|off")});
    parser.addOption({QStringLiteral("output"), QStringLiteral("Build output directory; relative paths use the project directory."), QStringLiteral("directory")});
    parser.addOption({QStringLiteral("force"), QStringLiteral("Force rebuilding packs.")});
    parser.addOption({QStringLiteral("search"), QStringLiteral("Filter commands by name, description or category."), QStringLiteral("text")});
    parser.addOption({QStringLiteral("category"), QStringLiteral("Filter commands by their category."), QStringLiteral("name")});
    parser.addOption({QStringLiteral("read-only"), QStringLiteral("List only commands that do not change state.")});

    bool wantsJson = app.arguments().contains(QStringLiteral("--json"));
    if (!parser.parse(app.arguments()))
        return output(HarkenStudioCli::failure(QStringLiteral("USAGE"), parser.errorText()), wantsJson, 2);
    wantsJson = parser.isSet(QStringLiteral("json"));
    verbose = parser.isSet(QStringLiteral("verbose"));
    if (parser.isSet(QStringLiteral("version"))) {
        if (wantsJson) return output({{"status", "ok"}, {"result", QJsonObject{{"version", app.applicationVersion()}}}}, true);
        print((app.applicationName() + QLatin1Char(' ') + app.applicationVersion() + QLatin1Char('\n')).toUtf8());
        return 0;
    }
    const QStringList positional = parser.positionalArguments();
    const auto usage = [&](const QString& message) {
        return output(HarkenStudioCli::failure(QStringLiteral("USAGE"), message), wantsJson, 2);
    };
    if (!positional.isEmpty() && positional.front() == QStringLiteral("commands")) {
        const auto arguments = app.arguments();
        const QStringList options{QStringLiteral("--search"), QStringLiteral("--category"),
            QStringLiteral("--read-only"), QStringLiteral("--instance"), QStringLiteral("--json"),
            QStringLiteral("--verbose"), QStringLiteral("--help"), QStringLiteral("--help-all"),
            QStringLiteral("--version"), QStringLiteral("-h"), QStringLiteral("-v")};
        for (const auto& option : {QStringLiteral("search"), QStringLiteral("category")}) {
            if (parser.isSet(option) && parser.value(option).trimmed().isEmpty())
                return usage(QStringLiteral("--%1 requires a nonempty value.").arg(option));
            const auto index = arguments.indexOf(QStringLiteral("--") + option);
            if (index >= 0 && index + 1 < arguments.size()
                && options.contains(arguments.at(index + 1).section(QLatin1Char('='), 0, 0)))
                return usage(QStringLiteral("--%1 requires a value before the next option; use --%1=<text> for literal option text.").arg(option));
        }
    }
    if (parser.values(QStringLiteral("instance")).size() > 1)
        return usage(QStringLiteral("--instance may only be specified once."));
    if (parser.isSet(QStringLiteral("instance")) && parser.value(QStringLiteral("instance")).trimmed().isEmpty())
        return usage(QStringLiteral("--instance requires a nonempty instance ID. Run HarkenStudioCLI instances."));
    if (parser.isSet(QStringLiteral("instance")) && parser.isSet(QStringLiteral("project")))
        return usage(QStringLiteral("Use either --instance or --project. Instance commands operate on the project already open in Studio."));
    bool validTimeout = false;
    const int seconds = parser.value(QStringLiteral("timeout")).toInt(&validTimeout);
    if (!validTimeout || seconds < 1 || seconds > 86400)
        return usage(QStringLiteral("--timeout must be an integer from 1 to 86400."));
    if (parser.isSet(QStringLiteral("help")) || parser.isSet(QStringLiteral("help-all")) || app.arguments().size() == 1) {
        QString help = positional.isEmpty() ? parser.helpText() : commandHelp(positional.front());
        if (help.isEmpty()) return usage(QStringLiteral("Unknown command '%1'. Run HarkenStudioCLI --help.").arg(positional.front()));
        if (positional.size() == 2
            && (positional.front() == QStringLiteral("exec") || positional.front() == QStringLiteral("commands"))) {
            HarkenStudioCli::StudioSession session(parser.value(QStringLiteral("instance")), seconds * 1000,
                [] { return interrupted != 0; });
            return describeCommand(session.describe(positional.at(1)), wantsJson);
        }
        if (wantsJson)
            return output({{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("result"), QJsonObject{{QStringLiteral("help"), help}}}}, true);
        print(help.toUtf8());
        return 0;
    }
    if (positional.isEmpty())
        return usage(QStringLiteral("A command is required. Run HarkenStudioCLI --help."));
    const QString command = positional.front();
    for (const auto& option : {QStringLiteral("project"), QStringLiteral("args"), QStringLiteral("input"),
                              QStringLiteral("timeout"), QStringLiteral("self-clean"), QStringLiteral("output"),
                              QStringLiteral("search"), QStringLiteral("category")}) {
        if (parser.values(option).size() > 1)
            return usage(QStringLiteral("--%1 may only be specified once.").arg(option));
    }
    if (command == QStringLiteral("build") || command == QStringLiteral("list-targets")) {
        const auto response = HarkenStudioCli::runSavedProjectBuild(parser, [] { return interrupted != 0; });
        return output(response, wantsJson, response.value(QStringLiteral("code")) == QStringLiteral("USAGE") ? 2 : 1);
    }
    if (command != QStringLiteral("instances") && command != QStringLiteral("commands") && command != QStringLiteral("init")
        && command != QStringLiteral("exec") && command != QStringLiteral("run")
        && command != QStringLiteral("shell"))
        return usage(QStringLiteral("Unknown command '%1'. Run HarkenStudioCLI --help.").arg(command));
    QStringList allowed{QStringLiteral("json"), QStringLiteral("verbose")};
    if (command == QStringLiteral("commands") || command == QStringLiteral("exec")
        || command == QStringLiteral("run") || command == QStringLiteral("shell"))
        allowed.append(QStringLiteral("instance"));
    if (command == QStringLiteral("commands")) {
        allowed.append({QStringLiteral("search"), QStringLiteral("category"), QStringLiteral("read-only")});
    } else if (command == QStringLiteral("init")) {
        allowed.append(QStringLiteral("timeout"));
    } else if (command != QStringLiteral("instances")) {
        allowed.append({QStringLiteral("project"), QStringLiteral("timeout")});
        if (command != QStringLiteral("shell"))
            allowed.append({QStringLiteral("args"), QStringLiteral("input"), QStringLiteral("i"), QStringLiteral("save")});
    }
    for (const auto& option : parser.optionNames()) {
        if (!allowed.contains(option))
            return usage(QStringLiteral("--%1 is not supported by %2.").arg(option, command));
    }
    if (positional.size() > 2 || ((command == QStringLiteral("init") || command == QStringLiteral("exec")) && positional.size() != 2)
        || ((command == QStringLiteral("instances") || command == QStringLiteral("run") || command == QStringLiteral("shell")) && positional.size() != 1))
        return usage(QStringLiteral("Invalid positional arguments. Run HarkenStudioCLI --help."));
    if (command == QStringLiteral("commands") && positional.size() == 2
        && (parser.isSet(QStringLiteral("search")) || parser.isSet(QStringLiteral("category"))
            || parser.isSet(QStringLiteral("read-only"))))
        return usage(QStringLiteral("Command filters apply to the directory; omit the command name."));
    if (parser.isSet(QStringLiteral("project")) && parser.value(QStringLiteral("project")).trimmed().isEmpty())
        return usage(QStringLiteral("--project requires a nonempty project path."));
    if (command == QStringLiteral("run") && !parser.isSet(QStringLiteral("args")) && !parser.isSet(QStringLiteral("input")))
        return usage(QStringLiteral("run requires a JSON request array from --input <file|-> or --args <json>."));

    if (command == QStringLiteral("instances")) {
        const auto response = HarkenStudioCli::StudioInstanceClient::discover(1000,
            [] { return interrupted != 0; });
        if (wantsJson || !HarkenStudioCli::succeeded(response)) return output(response, wantsJson);
        const auto instances = response.value(QStringLiteral("result")).toObject().value(QStringLiteral("instances")).toArray();
        for (const auto& value : instances) {
            const auto instance = value.toObject();
            print((QStringLiteral("%1  %2  PID %3  %4  %5\n").arg(instance.value(QStringLiteral("id")).toString(),
                instance.value(QStringLiteral("version")).toString(),
                QString::number(instance.value(QStringLiteral("pid")).toInteger()),
                instance.value(QStringLiteral("project")).toObject().value(QStringLiteral("path")).toString(),
                instance.value(QStringLiteral("available")).toBool() ? QStringLiteral("available")
                    : QStringLiteral("unavailable: ") + instance.value(QStringLiteral("unavailableReason")).toString())).toUtf8());
        }
        print((QStringLiteral("%1 running Studio instances.\n").arg(instances.size())).toUtf8());
        return 0;
    }
    HarkenStudioCli::StudioSession session(parser.value(QStringLiteral("instance")), seconds * 1000,
        [] { return interrupted != 0; });
    if (session.isAttached()) {
        const auto connected = session.execute(QStringLiteral("ping"), {}, seconds * 1000,
            [] { return interrupted != 0; });
        if (!HarkenStudioCli::succeeded(connected)) return output(connected, wantsJson);
    }
    if (command == QStringLiteral("shell")) {
        if (parser.isSet(QStringLiteral("project"))) {
            const auto opened = session.open(parser.value(QStringLiteral("project")), seconds * 1000,
                [] { return interrupted != 0; });
            if (!HarkenStudioCli::succeeded(opened)) return output(opened, wantsJson);
        }
#ifdef Q_OS_WIN
        const bool shellJson = wantsJson || _isatty(_fileno(stdin)) == 0 || _isatty(_fileno(stdout)) == 0;
#else
        const bool shellJson = wantsJson || isatty(fileno(stdin)) == 0 || isatty(fileno(stdout)) == 0;
#endif
        return HarkenStudioCli::runShell(session, seconds * 1000, shellJson, interrupted,
            [&](QJsonObject response) {
                HarkenStudioCli::writeShellResponse(std::move(response), shellJson);
            });
    }
    if (command == QStringLiteral("commands")) {
        if (positional.size() == 2) return describeCommand(session.describe(positional.at(1)), wantsJson);
        const auto response = HarkenStudioCli::filterCommandDirectory(session.commands(),
            parser.value(QStringLiteral("search")), parser.value(QStringLiteral("category")),
            parser.isSet(QStringLiteral("read-only")));
        if (!HarkenStudioCli::succeeded(response)) return output(response, wantsJson);
        const auto entries = response.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("commands")).toArray();
        if (wantsJson) return output(response, true);
        printCommandDirectory(entries);
        return 0;
    }
    if (command == QStringLiteral("init")) {
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
        int index = 0;
        for (const auto& value : input.array()) {
            const auto request = value.toObject();
            const auto invalidRequest = [&](const QString& message) {
                auto response = HarkenStudioCli::failure(QStringLiteral("USAGE"), message);
                response.insert(QStringLiteral("index"), index);
                response.insert(QStringLiteral("results"), QJsonArray{});
                return output(response, wantsJson, 2);
            };
            if (!value.isObject() || !request.value(QStringLiteral("cmd")).isString()
                || request.value(QStringLiteral("cmd")).toString().isEmpty()
                || (request.contains(QStringLiteral("args")) && !request.value(QStringLiteral("args")).isObject()))
                return invalidRequest(QStringLiteral("Each request must contain a nonempty cmd string and an optional args object."));
            for (auto it = request.begin(); it != request.end(); ++it)
                if (it.key() != QLatin1String("cmd") && it.key() != QLatin1String("args"))
                    return invalidRequest(QStringLiteral("Unknown request field '%1'; use cmd and args.").arg(it.key()));
            auto validated = session.validate(request.value(QStringLiteral("cmd")).toString(),
                request.value(QStringLiteral("args")).toObject());
            if (!HarkenStudioCli::succeeded(validated)) {
                validated.insert(QStringLiteral("command"), request.value(QStringLiteral("cmd")));
                validated.insert(QStringLiteral("index"), index);
                validated.insert(QStringLiteral("results"), QJsonArray{});
                return output(validated, wantsJson);
            }
            ++index;
        }
    } else {
        const auto validated = session.validate(positional.at(1), input.object());
        if (!HarkenStudioCli::succeeded(validated)) return output(validated, wantsJson);
    }
    if (parser.isSet(QStringLiteral("project"))) {
        const auto opened = session.open(parser.value(QStringLiteral("project")), seconds * 1000,
            [] { return interrupted != 0; });
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
