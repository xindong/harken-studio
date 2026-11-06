#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QStyleFactory>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <memory>

#include "harken_buildinfo.h"
#include "Common/WheelGuard.h"
#include "Core/StartupArguments.h"
#include "Core/HubLauncher.h"
#include "Core/MigrationManager.h"
#include "Core/ProjectPreflight/ProjectPreflight.h"
#include "Core/ProjectOpenProtocol.h"
#include "Core/ProjectLocator.h"
#include "Core/ProjectSchemaGenerated.h"
#include "Core/ProjectTemplateLoader.h"
#include "Core/SchemaArchive.h"
#include "Core/SchemaBuildGenerated.h"
#include "Core/SchemaCompatibilityProbe.h"
#include "Core/StartupCoordinator.h"
#include "Core/ThemeManager.h"
#include "Core/ToolTip.h"
#include "Core/HarkenStyle.h"
#include "Dialogs/StartupLoadingWindow.h"
#include "Widgets/TitleBar/FramelessChromeInstall.h"
#include "Tooling/StudioCli/Shell.h"
#include "Tooling/StudioCli/StudioSession.h"

int main(int argc, char* argv[])
{
    bool shell = false;
    bool help = false;
    bool showVersion = false;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        shell |= std::strcmp(argv[i], "--shell") == 0;
        help |= std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0;
        showVersion |= std::strcmp(argv[i], "--version") == 0;
        json |= std::strcmp(argv[i], "--json") == 0;
    }
    if (shell || help || showVersion) {
        QString error;
        if (!HarkenStudioCli::prepareDesktopShellStdio(&error, shell && !help && !showVersion)) {
            HarkenStudioCli::writeShellResponse(HarkenStudioCli::failure(QStringLiteral("SHELL_IO_UNAVAILABLE"), error));
            return 2;
        }
    }
    if (help || showVersion) {
        const auto version = QString::fromUtf8(Harken::BuildInfo::Version.data(), Harken::BuildInfo::Version.size());
        const auto text = showVersion ? QStringLiteral("HarkenStudio %1\n").arg(version) : QStringLiteral(
            "Usage: HarkenStudio [--project <path> | --last | --create-project <directory>] [--shell] [--json]\n\n"
            "  --shell       Accept commands on stdin in this GUI process after the project is ready.\n"
            "  --json        With --shell, use one compact UTF-8 JSON response per line.\n"
            "  --help        Show this help without starting the GUI.\n"
            "  --version     Show the software version.\n\n"
            "Shell input: command {\"argument\":\"value\"}; help; commands [name]; exit.\n"
            "Redirected input/output automatically uses JSON. No extra console window is created.\n"
            "exit or EOF ends shell input and keeps Studio and its unsaved edits open.\n"
            "Use save_project before switching or closing the project.\n"
            "--shell needs an existing terminal or redirected stdin/stdout and a project to open/create.\n"
            "To control a Studio that is already running, use HarkenStudioCLI instances and --instance <id>.\n");
        if (json) {
            HarkenStudioCli::writeShellResponse({{"status", "ok"},
                {"result", QJsonObject{{showVersion ? QStringLiteral("version") : QStringLiteral("help"),
                    showVersion ? version : text}}}});
        } else {
            const auto bytes = text.toUtf8();
            std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
            std::fflush(stdout);
        }
        return 0;
    }
    if (shell) {
        const QSet<QString> valueOptions{
            QStringLiteral("--project"), QStringLiteral("--project-name"),
            QStringLiteral("--project-location"), QStringLiteral("--project-template-id"),
            QStringLiteral("--project-template-file")};
        const QSet<QString> flags{
            QStringLiteral("--shell"), QStringLiteral("--json"), QStringLiteral("--last"),
            QStringLiteral("--create-project")};
        QSet<QString> supplied;
        QStringList positional;
        QString createDirectory;
        QString argumentError;
        for (int i = 1; i < argc && argumentError.isEmpty(); ++i) {
            const QString option = QString::fromLocal8Bit(argv[i]);
            if (valueOptions.contains(option) || flags.contains(option)) {
                if (supplied.contains(option)) {
                    argumentError = QStringLiteral("%1 may only be supplied once.").arg(option);
                    break;
                }
                supplied.insert(option);
                if (valueOptions.contains(option)) {
                    if (i + 1 >= argc || argv[i + 1][0] == '\0' || argv[i + 1][0] == '-')
                        argumentError = QStringLiteral("%1 requires a value.").arg(option);
                    else ++i;
                } else if (option == QStringLiteral("--create-project") && i + 1 < argc
                    && argv[i + 1][0] != '-') {
                    createDirectory = QString::fromLocal8Bit(argv[++i]);
                    if (createDirectory.isEmpty())
                        argumentError = QStringLiteral("--create-project requires a non-empty directory.");
                }
            } else if (option.startsWith(QLatin1Char('-'))) {
                argumentError = QStringLiteral("Unsupported option with --shell: %1. Use --shell --help.").arg(option);
            } else {
                positional.append(option);
            }
        }
        const bool creating = supplied.contains(QStringLiteral("--create-project"));
        const int startupModes = static_cast<int>(creating)
            + static_cast<int>(supplied.contains(QStringLiteral("--project")))
            + static_cast<int>(supplied.contains(QStringLiteral("--last")))
            + static_cast<int>(!positional.isEmpty());
        if (argumentError.isEmpty() && (startupModes != 1 || positional.size() > 1))
            argumentError = QStringLiteral("Choose exactly one project startup mode: --project <path>, a project path, --last, or --create-project <directory>.");
        for (const auto& option : valueOptions) {
            if (argumentError.isEmpty() && option != QStringLiteral("--project")
                && supplied.contains(option) && !creating)
                argumentError = QStringLiteral("%1 requires --create-project.").arg(option);
        }
        const bool namedCreation = supplied.contains(QStringLiteral("--project-name"))
            || supplied.contains(QStringLiteral("--project-location"));
        if (argumentError.isEmpty() && creating
            && ((!createDirectory.isEmpty() && namedCreation)
                || (createDirectory.isEmpty() && (!supplied.contains(QStringLiteral("--project-name"))
                    || !supplied.contains(QStringLiteral("--project-location"))))))
            argumentError = QStringLiteral("Use --create-project <directory>, or --create-project with both --project-name and --project-location.");
        if (argumentError.isEmpty() && supplied.contains(QStringLiteral("--project-template-id"))
            && !supplied.contains(QStringLiteral("--project-template-file")))
            argumentError = QStringLiteral("--project-template-id requires --project-template-file.");
        if (!argumentError.isEmpty()) {
            HarkenStudioCli::writeShellResponse(HarkenStudioCli::failure(QStringLiteral("INVALID_ARGUMENTS"), argumentError));
            return 2;
        }
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--schema-roundtrip") != 0) continue;
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QTemporaryDir settingsRoot;
        QApplication app(argc, argv);
        app.setOrganizationName(QStringLiteral("HarkenSchemaCompatibilityProbe"));
        app.setApplicationName(QStringLiteral("HarkenSchemaCompatibilityProbe"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
        qputenv("HARKEN_HUB_USER_ROOT", settingsRoot.filePath(QStringLiteral("hub")).toUtf8());
        QJsonObject request;
        QString requestError;
        if (!settingsRoot.isValid()) {
            requestError = QStringLiteral("Cannot isolate schema round-trip settings");
        } else if (i + 1 != argc - 1) {
            requestError = QStringLiteral("Schema round-trip requires one request file");
        } else {
            QFile file(QString::fromLocal8Bit(argv[i + 1]));
            if (!file.open(QIODevice::ReadOnly)) {
                requestError = QStringLiteral("Cannot read schema round-trip request");
            } else {
                QJsonParseError parseError;
                const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject())
                    requestError = QStringLiteral("Schema round-trip request is invalid JSON");
                else request = document.object();
            }
        }
        QJsonObject report = VNX::runSchemaCompatibilityProbe(request);
        if (!requestError.isEmpty()) report.insert(QStringLiteral("error"), requestError);
        const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Compact);
        std::fwrite(json.constData(), 1, static_cast<size_t>(json.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return report.value(QStringLiteral("ok")).toBool() ? 0 : 5;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            QCoreApplication app(argc, argv);
            QString schemaError;
            const bool schemaArchiveOk =
                VNX::SchemaArchive::instance().verifyBuildSnapshot(&schemaError);
            QJsonObject report;
            report[QStringLiteral("ok")] = schemaArchiveOk;
            report[QStringLiteral("product")] = QStringLiteral("HarkenStudio");
            report[QStringLiteral("version")] = QString::fromUtf8(
                Harken::BuildInfo::Version.data(),
                static_cast<int>(Harken::BuildInfo::Version.size()));
            report[QStringLiteral("buildId")] = QString::fromUtf8(
                Harken::BuildInfo::BuildId.data(),
                static_cast<int>(Harken::BuildInfo::BuildId.size()));
            report[QStringLiteral("gitCommit")] = QString::fromUtf8(
                Harken::BuildInfo::GitCommit.data(),
                static_cast<int>(Harken::BuildInfo::GitCommit.size()));
            report[QStringLiteral("gitDirty")] = Harken::BuildInfo::GitDirty;
            report[QStringLiteral("channel")] = QString::fromUtf8(
                Harken::BuildInfo::BuildChannel.data(),
                static_cast<int>(Harken::BuildInfo::BuildChannel.size()));
            report[QStringLiteral("buildType")] = QString::fromUtf8(
                Harken::BuildInfo::BuildType.data(),
                static_cast<int>(Harken::BuildInfo::BuildType.size()));
            report[QStringLiteral("capabilitiesVersion")] = 2;
            report[QStringLiteral("editorSchemaVersion")] =
                VNX::ProjectSchema::kProjectSchemaVersion;
            report[QStringLiteral("schemaArchiveOk")] = schemaArchiveOk;
            report[QStringLiteral("schemaSnapshotHash")] =
                QString::fromLatin1(VNX::SchemaBuild::kSnapshotHash);
            report[QStringLiteral("packSchemaVersion")] =
                HARKEN_PACK_SCHEMA_VERSION;
            if (!schemaError.isEmpty()) report[QStringLiteral("schemaError")] = schemaError;
            const QByteArray json =
                QJsonDocument(report).toJson(QJsonDocument::Compact);
            std::fwrite(json.constData(), 1,
                        static_cast<size_t>(json.size()), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
            return schemaArchiveOk ? 0 : 2;
        }
    }

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--analyze-project-open") != 0)
            continue;
        QCoreApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("HarkenStudio"));
        app.setOrganizationName(QStringLiteral("HarkenSound"));
        const QStringList arguments = app.arguments();
        const int projectArgument = arguments.indexOf(
            QStringLiteral("--analyze-project-open"));
        if (projectArgument < 0 || projectArgument + 1 >= arguments.size())
            return 4;
        QString requestId;
        const int requestArgument = arguments.indexOf(
            QStringLiteral("--project-open-request-id"));
        if (requestArgument >= 0 && requestArgument + 1 < arguments.size())
            requestId = arguments.at(requestArgument + 1);
        if (requestId.isEmpty())
            return 4;
        const QString projectPath = arguments.at(projectArgument + 1);
        const VNX::ProjectUpgradeAnalysis analysis =
            VNX::ProjectUpgradeManager().analyzeForProjectOpen(projectPath);
        const QByteArray json = QJsonDocument(
            VNX::projectOpenAnalysisReport(analysis, requestId))
                                    .toJson(QJsonDocument::Compact);
        std::fwrite(json.constData(), 1,
                    static_cast<size_t>(json.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return analysis.ok ? 0 : 4;
    }

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--preflight-project") != 0)
            continue;
        const QString projectPath = QString::fromLocal8Bit(argv[i + 1]);
        int targetSchema = 0;
        for (int j = 1; j + 1 < argc; ++j) {
            if (std::strcmp(argv[j], "--target-schema") == 0)
                targetSchema = std::atoi(argv[j + 1]);
        }
        const VNX::PreflightReport report =
            VNX::ProjectPreflight::run(projectPath, targetSchema);
        const QByteArray json =
            QJsonDocument(report.toJson()).toJson(QJsonDocument::Indented);
        std::fwrite(json.constData(), 1,
                    static_cast<size_t>(json.size()), stdout);
        std::fflush(stdout);
        return report.blockingErrors.isEmpty() ? 0 : 3;
    }

    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    QSurfaceFormat glFormat;
    glFormat.setVersion(3, 3);
    glFormat.setProfile(QSurfaceFormat::CompatibilityProfile);
    glFormat.setDepthBufferSize(24);
    glFormat.setStencilBufferSize(8);
    glFormat.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    QSurfaceFormat::setDefaultFormat(glFormat);

    QApplication app(argc, argv);
    app.installEventFilter(VNX::WheelGuard::instance());
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        QApplication::setStyle(new VNX::HarkenStyle(fusion));
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);

    VNX::installFramelessChrome();
    app.setApplicationName(QStringLiteral("HarkenStudio"));
    app.setApplicationVersion(QString::fromUtf8(
        Harken::BuildInfo::Version.data(),
        static_cast<int>(Harken::BuildInfo::Version.size())));
    app.setOrganizationName(QStringLiteral("HarkenSound"));
    app.setOrganizationDomain(QStringLiteral("harkensound.com"));
    app.setWindowIcon(QIcon(QStringLiteral(":/Resources/icons/HarkenStudio.ico")));

    VNX::ProjectTemplateLoader::ensureUserTemplatesSeeded();
    VNX::ThemeManager::instance();
    VNX::ToolTip::initialize();

    VNX::StartupLoadingWindow loading;
    loading.showAfterInitialFrame();
    const auto failStartup = [&loading, shell](const QString& message) {
        if (shell) {
            HarkenStudioCli::writeShellResponse(HarkenStudioCli::failure(QStringLiteral("STARTUP_FAILED"), message));
            return 3;
        }
        loading.showFailure(message);
        QMessageBox::critical(&loading, QObject::tr("Harken Studio"), message);
        return 3;
    };

    const QStringList args = app.arguments();
    const VNX::StartupArguments startupArguments =
        VNX::StartupArguments::parse(args);
    if (startupArguments.createProject
        && (startupArguments.projectName.isEmpty()
            || startupArguments.projectLocation.isEmpty())) {
        return failStartup(QObject::tr(
            "Project creation requires a project directory"));
    }

    QString projectFile;
    if (!startupArguments.createProject) {
        QString requestedProject = startupArguments.projectPath;
        if (requestedProject.isEmpty() && args.contains(QStringLiteral("--last"))) {
            QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                               QStringLiteral("HarkenSound"),
                               QStringLiteral("HarkenStudio"));
            const QStringList recent =
                settings.value(QStringLiteral("RecentProjects")).toStringList();
            if (!recent.isEmpty())
                requestedProject = recent.first();
        } else if (requestedProject.isEmpty()) {
            for (int i = 1; i < args.size(); ++i) {
                if (!args.at(i).startsWith(QStringLiteral("--"))) {
                    requestedProject = args.at(i);
                    requestedProject.remove(QLatin1Char('"'));
                    break;
                }
            }
        }

        if (!requestedProject.isEmpty()) {
            projectFile = VNX::ProjectLocator::locate(requestedProject).projectFilePath;
            if (projectFile.isEmpty()) {
                return failStartup(QObject::tr(
                    "Could not find a unique Harken project at: %1")
                                        .arg(requestedProject));
            }
        }
        if (projectFile.isEmpty()) {
            if (shell)
                return failStartup(QStringLiteral("--shell requires --project <path>, --create-project <directory>, or --last with a recent project."));
            QString error;
            if (!VNX::HubLauncher::open(VNX::HubPage::Projects, &error, true)) {
                return failStartup(error);
            }
            return 0;
        }
    }

    loading.setProjectName(startupArguments.createProject
                               ? startupArguments.projectName
                               : QFileInfo(projectFile).completeBaseName());
    auto coordinator = std::make_unique<VNX::StartupCoordinator>(
        app, loading, startupArguments, projectFile, &app);
    QTimer::singleShot(0, coordinator.get(), &VNX::StartupCoordinator::start);
    return app.exec();
}
