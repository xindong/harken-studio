#include "Cli/HubCli.h"

#include "App/FileLog.h"
#include "App/SingleInstanceGuard.h"
#include "Legal/ConsentStore.h"
#include "Legal/LegalDocuments.h"
#include "Localization/HubLocalization.h"
#include "Maintenance/LogExporter.h"
#include "Platform/PlatformPaths.h"
#include "ReleaseNotes/ReleaseNotesCatalog.h"
#include "Settings/HubSettings.h"
#include "Transactions/TransactionDiagnostics.h"
#include "harken_buildinfo.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <cstdio>

namespace VNH::Cli {
namespace {

struct Command
{
    const char* name;
    const char* options;
    const char* description;
};

const Command commands[] = {
    {"projects list", "", "List registered projects and current on-disk names."},
    {"projects add", "path!", "Register an existing project."},
    {"projects remove", "path! confirm", "Remove a project registration; keep project files."},
    {"projects select-version", "path! build!", "Set the Studio version for a project."},
    {"projects open", "path! build", "Run launch preflight and open the project in Studio."},
    {"projects create", "build! name! location! template", "Create a project through the selected Studio."},
    {"projects templates", "build!", "List the project templates of the selected Studio."},
    {"versions list", "", "List installed packages and local builds."},
    {"versions server", "", "List available server releases."},
    {"versions download", "build!", "Download and verify a signed server package."},
    {"versions install", "package! manifest allow-expired confirm", "Review and install a signed local package."},
    {"versions add-local", "path!", "Probe and register a local Studio build."},
    {"versions refresh-local", "path!", "Recheck metadata for a registered local build."},
    {"versions remove-local", "path! confirm", "Remove a local build registration without deleting it."},
    {"versions verify", "build!", "Check installed version integrity."},
    {"versions repair", "build! confirm", "Repair a managed installation from its verified package."},
    {"versions uninstall", "build! confirm", "Review and uninstall a managed version."},
    {"versions notes", "build!", "Return Studio and integration release-note URLs."},
    {"integrations list", "", "List engine projects and Harken project associations."},
    {"integrations add-target", "engine path!", "Register a Unity project."},
    {"integrations link", "engine path! project! source confirm", "Associate an engine project with a Harken project."},
    {"integrations unlink", "id! confirm", "Remove an association without removing integration files."},
    {"integrations locate", "engine path! location", "Rediscover and save the installed integration location and version."},
    {"integrations install", "engine path! source! location confirm", "Review or apply an integration installation/update; the installation directory is replaced as a whole."},
    {"integrations remove", "engine path! location confirm", "Review or remove the installed integration directory."},
    {"integrations restore", "engine path! confirm", "Restore the integration version that was installed before an unfinished install or removal."},
    {"settings get", "", "Read Hub settings and data locations."},
    {"settings set", "key! value!", "Set theme, language, project-location, auto-hub-update or auto-studio-download."},
    {"updates check", "product!", "Check the signed latest release for hub or studio."},
    {"updates download", "product!", "Download the latest signed release for hub or studio."},
    {"updates apply", "product! confirm", "Apply a downloaded update without starting the Hub GUI."},
    {"updates status", "", "Read persisted Hub and Studio update states."},
    {"diagnostics paths", "", "Return data, installation and log paths."},
    {"diagnostics logs", "lines", "Read the tail of the current activity log."},
    {"diagnostics export", "path! confirm", "Export diagnostic logs."},
    {"diagnostics transactions", "", "Read installation transaction diagnostics."},
    {"legal show", "product", "Read current license/privacy documents before accepting them."},
    {"legal accept", "product confirm", "Record explicit acceptance of the displayed documents."},
    {"legal status", "", "Read current Hub and Studio consent status."},
    {"about", "", "Read Hub build identity and release notes."},
};

QJsonObject parameters(const Command& command)
{
    static const QMap<QString, QString> descriptions{
        {"path", "Filesystem path to the project, executable, template or output archive named by the command."},
        {"build", "Exact buildId returned by versions list or versions server."},
        {"name", "New project folder name, without path separators."},
        {"location", "Project parent directory for projects create; integration path relative to the Unity project for integrations commands."},
        {"template", "Template id returned by projects templates."},
        {"package", "Downloaded Studio ZIP path."},
        {"manifest", "Signed release manifest path returned by versions download; defaults to a sibling .manifest file."},
        {"allow-expired", "Explicitly allow an expired signed manifest when installing a local Studio package."},
        {"engine", "Engine type supported by the public integration workflow."},
        {"project", "Harken .hsp project file to associate with the Unity project."},
        {"source", "Unity integration source folder."},
        {"id", "Association id returned by integrations list."},
        {"key", "Setting name. theme: system/light/dark; language: system/en/zh_CN; project-location: directory; auto options: true/false."},
        {"value", "New value for the selected setting."},
        {"product", "hub or studio."},
        {"lines", "Maximum lines per GUI/background log, from 1 to 10000."},
        {"confirm", "Opaque token returned by a previous review of the identical command and current plan."}
    };
    QJsonObject result;
    for (auto key : QString::fromLatin1(command.options).split(' ', Qt::SkipEmptyParts)) {
        const bool required = key.endsWith('!');
        if (required) key.chop(1);
        QJsonObject option{{"type", "string"}, {"required", required}, {"description", descriptions.value(key)}};
        if (key == "allow-expired") {
            option["enum"] = QJsonArray{"true", "false"};
            option["default"] = "false";
        }
        if (key == "engine") { option["enum"] = QJsonArray{"unity"}; option["default"] = "unity"; }
        if (key == "product") { option["enum"] = QJsonArray{"hub", "studio"}; if (!required) option["default"] = "hub"; }
        if (key == "key") option["enum"] = QJsonArray{"theme", "language", "project-location", "auto-hub-update", "auto-studio-download"};
        if (key == "lines") option["default"] = "100";
        result[key] = option;
    }
    return result;
}

QJsonObject legalDocument(const QString& product)
{
    require(product == "hub" || product == "studio", "--product must be hub or studio", "InvalidArguments");
    const auto documents = LegalDocuments::texts();
    require(!documents.isEmpty(), "The license agreement and privacy policy could not be loaded");
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(documents.join("\n\n").toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString id = product + "-eula";
    const QString version = LegalDocuments::version();
    return {{"product", product}, {"id", id}, {"version", version}, {"sha256", hash},
        {"documents", QJsonArray::fromStringList(documents)}, {"accepted", ConsentStore::hasConsent(id, version, hash)}};
}

QJsonObject settings()
{
    return {{"theme", HubSettings::themePreference()}, {"language", HubSettings::languagePreference()},
        {"project-location", HubSettings::projectLocation()}, {"auto-hub-update", HubSettings::autoSilentlyUpdateHub()},
        {"auto-studio-download", HubSettings::autoDownloadStudio()},
        {"file", HubSettings::filePath()}};
}

Reply execute(const Arguments& args)
{
    if (args.group == "projects") return projects(args);
    if (args.group == "versions") return versions(args);
    if (args.group == "integrations") return integrations(args);
    if (args.group == "updates") return updates(args);
    if (args.group == "settings") {
        if (args.operation == "get") return success(settings());
        const auto key = args.required("key");
        const auto value = args.required("value");
        const bool automatic = key == "auto-hub-update" || key == "auto-studio-download";
        if (automatic && value == "true") requireConsent("hub");
        bool saved = false;
        if (key == "theme") {
            require(QStringList{"system", "light", "dark"}.contains(value), "Theme must be system, light or dark", "InvalidArguments");
            saved = HubSettings::setThemePreference(value);
        } else if (key == "language") {
            require(QStringList{"system", "en", "zh_CN"}.contains(value), "Language must be system, en or zh_CN", "InvalidArguments");
            saved = HubSettings::setLanguagePreference(value);
        } else if (key == "project-location") saved = HubSettings::setProjectLocation(value);
        else {
            require(value == "true" || value == "false", "Boolean settings require true or false", "InvalidArguments");
            const bool enabled = value == "true";
            if (key == "auto-hub-update") saved = HubSettings::setAutoSilentlyUpdateHub(enabled);
            else if (key == "auto-studio-download") saved = HubSettings::setAutoDownloadStudio(enabled);
            else require(false, "Unknown setting: " + key, "InvalidArguments");
        }
        require(saved, "Could not save the setting");
        auto result = settings();
        if (automatic || key == "language") {
            const bool notified = SingleInstanceGuard::sendTo(SingleInstanceGuard::backgroundServerName(), {"--settings-changed"});
            if (automatic && value == "true" && !notified)
                require(QProcess::startDetached(QCoreApplication::applicationFilePath(), {"--background"}),
                    "The setting was saved, but the background service could not be started");
            result["backgroundNotified"] = notified;
        }
        return success(result);
    }
    if (args.group == "legal") {
        if (args.operation == "status") return success({{"hub", legalDocument("hub")["accepted"]}, {"studio", legalDocument("studio")["accepted"]}});
        auto document = legalDocument(args.value("product", "hub"));
        if (args.operation == "accept") {
            auto plan = document;
            plan.remove("accepted");
            confirm(args, plan);
            require(ConsentStore::recordConsent(document["id"].toString(), document["version"].toString(), document["sha256"].toString(), QCoreApplication::applicationVersion(), QDateTime::currentDateTimeUtc()), "Could not save consent");
            document["accepted"] = true;
        }
        return success(document);
    }
    if (args.group == "diagnostics") {
        if (args.operation == "paths") return success({{"user", userDataDir()}, {"machine", machineDataDir()}, {"versions", studioVersionsRoot()}, {"hub", hubInstallDir()}, {"log", FileLog::filePathFor("gui")}});
        if (args.operation == "transactions") return success({{"transactions", TransactionDiagnostics::toJson(TransactionDiagnostics::summarize())}});
        if (args.operation == "logs") {
            bool valid = false;
            const int count = args.value("lines", "100").toInt(&valid);
            require(valid && count > 0 && count <= 10000, "--lines must be between 1 and 10000", "InvalidArguments");
            QJsonArray logs;
            for (const auto* role : {"gui", "background"}) {
                QFile file(FileLog::filePathFor(role));
                QStringList lines;
                const bool exists = file.exists();
                if (exists) {
                    const bool opened = file.open(QIODevice::ReadOnly);
                    require(opened, file.errorString());
                    file.seek(qMax(qint64(0), file.size() - 4 * 1024 * 1024));
                    lines = QString::fromUtf8(file.readAll()).split('\n');
                    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
                }
                logs.append(QJsonObject{{"role", role}, {"path", file.fileName()}, {"exists", exists},
                    {"lines", QJsonArray::fromStringList(lines.mid(qMax(0, int(lines.size()) - count)))}});
            }
            return success({{"logs", logs}});
        }
        const QString path = QFileInfo(args.required("path")).absoluteFilePath();
        confirm(args, {{"path", path}, {"existingSha256", fileHash(path)}});
        QString error;
        require(LogExporter::exportDiagnostics(path, &error), error);
        return success({{"path", path}});
    }
    return success({{"product", "HarkenHub"}, {"version", QCoreApplication::applicationVersion()},
        {"buildId", QString::fromUtf8(Harken::BuildInfo::BuildId.data(), int(Harken::BuildInfo::BuildId.size()))},
        {"releaseNotes", QString::fromUtf8(readFile(":/Resources/release-notes.json"))}});
}

}

QJsonObject commandReference()
{
    QJsonArray catalog;
    for (const auto& command : commands) {
        const auto options = parameters(command);
        catalog.append(QJsonObject{{"command", QLatin1String(command.name)}, {"parameters", options},
            {"reviewBeforeApply", options.contains("confirm")}, {"description", QLatin1String(command.description)}});
    }
    return {{"usage", "HarkenHub --cli <group> <operation> [--option value]"}, {"commands", catalog},
        {"schemaVersion", 1}, {"output", "One UTF-8 JSON object on stdout. Progress and diagnostics use stderr. No Hub window is created."},
        {"confirmation", "Exit 4 returns the current plan and an opaque confirmation token. Review it and repeat the identical command with --confirm TOKEN. Any changed arguments or plan require a new review."},
        {"consent", "Exit 5 requires explicit consent. Read legal show, preview legal accept and submit its token only after the user accepts the documents."},
        {"exitCodes", QJsonObject{{"0", "success"}, {"2", "invalid arguments"}, {"3", "operation failed"}, {"4", "review required"}, {"5", "consent required"}}}};
}

QString Arguments::value(const QString& key, const QString& fallback) const { return options.value(key, fallback); }
QString Arguments::required(const QString& key) const
{
    require(!value(key).trimmed().isEmpty(), "Missing --" + key, "InvalidArguments");
    return value(key);
}
bool Arguments::flag(const QString& key) const
{
    const auto text = value(key, "false");
    require(text == "true" || text == "false", "--" + key + " requires true or false", "InvalidArguments");
    return text == "true";
}
Reply success(const QJsonObject& data) { return {0, {{"ok", true}, {"data", data}}}; }
void require(bool condition, const QString& message, const QString& code)
{
    if (!condition) throw Reply{code == "InvalidArguments" ? 2 : code == "ConsentRequired" ? 5 : 3,
        {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}}};
}
QByteArray readFile(const QString& path)
{
    QFile file(path);
    const bool opened = file.open(QIODevice::ReadOnly);
    require(opened, path + ": " + file.errorString());
    return file.readAll();
}
QString fileHash(const QString& path)
{
    QFile file(path);
    if (!file.exists()) return {};
    const bool opened = file.open(QIODevice::ReadOnly);
    require(opened, path + ": " + file.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Could not hash " + path);
    return QString::fromLatin1(hash.result().toHex());
}
void confirm(const Arguments& args, QJsonObject plan)
{
    QJsonObject request;
    for (auto it = args.options.begin(); it != args.options.end(); ++it)
        if (it.key() != "confirm") request[it.key()] = it.value();
    plan["command"] = args.group + ' ' + args.operation;
    plan["arguments"] = request;
    const auto token = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(plan).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    if (args.value("confirm") != token)
        throw Reply{4, {{"ok", false}, {"confirmationRequired", true}, {"confirmation", token}, {"plan", plan},
            {"message", args.value("confirm").isEmpty() ? "Review the plan, then repeat this command with --confirm and this token." : "The plan changed or the confirmation token is invalid. Review again."}}};
}
void requireConsent(const QString& product)
{
    require(legalDocument(product)["accepted"].toBool(), "Read and accept the current documents with legal show and legal accept --product " + product, "ConsentRequired");
}

int run(const QStringList& arguments)
{
    Reply reply;
    try {
        require(arguments.indexOf("--cli") == 1, "--cli must be the first argument", "InvalidArguments");
        const auto args = arguments.mid(arguments.indexOf("--cli") + 1);
        if (args.isEmpty() || args.contains("--help") || args.first() == "help") {
            reply = success(commandReference());
        } else {
            Arguments parsed;
            parsed.group = args.first();
            const int offset = parsed.group == "about" ? 1 : 2;
            require(args.size() >= offset, "A command group and operation are required", "InvalidArguments");
            if (offset == 2) parsed.operation = args[1];
            const QString name = parsed.group + (parsed.operation.isEmpty() ? QString() : ' ' + parsed.operation);
            const Command* selected = nullptr;
            for (const auto& command : commands) if (name == QLatin1String(command.name)) selected = &command;
            require(selected != nullptr, "Unknown command: " + name, "InvalidArguments");
            const auto allowed = parameters(*selected);
            for (int i = offset; i < args.size(); ++i) {
                const auto option = args[i];
                require(option.startsWith("--"), "Expected an option: " + option, "InvalidArguments");
                const int equals = option.indexOf('=');
                const auto key = option.mid(2, equals < 0 ? -1 : equals - 2);
                require(allowed.contains(key) && !parsed.options.contains(key), "Unknown or repeated option: --" + key, "InvalidArguments");
                if (equals >= 0) parsed.options[key] = option.mid(equals + 1);
                else {
                    require(i + 1 < args.size() && !args[i + 1].startsWith("--"), "Missing value for --" + key, "InvalidArguments");
                    parsed.options[key] = args[++i];
                }
            }
            for (auto it = allowed.begin(); it != allowed.end(); ++it) {
                const auto spec = it.value().toObject();
                if (spec["required"].toBool()) parsed.required(it.key());
                if (parsed.options.contains(it.key()) && spec.contains("enum"))
                    require(spec["enum"].toArray().contains(parsed.value(it.key())), "Invalid value for --" + it.key(), "InvalidArguments");
            }
            reply = execute(parsed);
        }
    } catch (const Reply& error) { reply = error; }
    reply.body["schemaVersion"] = 1;
    const auto output = QJsonDocument(reply.body).toJson(QJsonDocument::Indented);
    fwrite(output.constData(), 1, size_t(output.size()), stdout);
    fflush(stdout);
    return reply.exitCode;
}

}
