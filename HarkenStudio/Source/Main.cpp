#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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
#include "Core/HubClient/HubClient.h"
#include "Core/HubLauncher.h"
#include "Core/MigrationManager.h"
#include "Core/ProjectPreflight/ProjectPreflight.h"
#include "Core/ProjectOpenProtocol.h"
#include "Core/ProjectLocator.h"
#include "Core/PackSchemaGenerated.h"
#include "Core/ProjectSchemaGenerated.h"
#include "Core/ProjectTemplateLoader.h"
#include "Core/SchemaArchive.h"
#include "Core/SchemaBuildGenerated.h"
#include "Core/SchemaCompatibilityProbe.h"
#include "Core/StartupCoordinator.h"
#include "Core/ThemeManager.h"
#include "Core/HarkenStyle.h"
#include "Dialogs/StartupLoadingWindow.h"
#include "Widgets/TitleBar/FramelessChromeInstall.h"

int main(int argc, char* argv[])
{
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
                VNX::PackSchema::kPackManifestSchemaVersion;
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

    VNX::installFramelessChrome();
    app.setApplicationName(QStringLiteral("HarkenStudio"));
    app.setApplicationVersion(QString::fromUtf8(
        Harken::BuildInfo::Version.data(),
        static_cast<int>(Harken::BuildInfo::Version.size())));
    app.setOrganizationName(QStringLiteral("HarkenSound"));
    app.setOrganizationDomain(QStringLiteral("harkensound.com"));

    VNX::ProjectTemplateLoader::ensureUserTemplatesSeeded();
    VNX::ThemeManager::instance();

    VNX::StartupLoadingWindow loading;
    loading.showAfterInitialFrame();

    const QStringList args = app.arguments();
    const VNX::HubClient::Arguments hubArguments =
        VNX::HubClient::parseArguments(args);
    if (hubArguments.createProject
        && (hubArguments.projectName.isEmpty()
            || hubArguments.projectLocation.isEmpty())) {
        loading.showFailure(QObject::tr(
            "Project creation requires a project directory"));
        return 3;
    }

    QString projectFile;
    if (!hubArguments.createProject) {
        QString requestedProject = hubArguments.projectPath;
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
                loading.showFailure(QObject::tr(
                    "Could not find a unique Harken project at: %1")
                                        .arg(requestedProject));
                return 3;
            }
        }
        if (projectFile.isEmpty()) {
            QString error;
            if (!VNX::HubLauncher::open(VNX::HubPage::Projects, &error, true)) {
                loading.showFailure(error);
                return 3;
            }
            return 0;
        }
    }

    loading.setProjectName(hubArguments.createProject
                               ? hubArguments.projectName
                               : QFileInfo(projectFile).completeBaseName());
    auto coordinator = std::make_unique<VNX::StartupCoordinator>(
        app, loading, hubArguments, projectFile, &app);
    QTimer::singleShot(0, coordinator.get(), &VNX::StartupCoordinator::start);
    return app.exec();
}
