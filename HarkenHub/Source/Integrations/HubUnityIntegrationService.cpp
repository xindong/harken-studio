#include "Integrations/HubUnityIntegrationService.h"

#include "Integrations/Unity/UnityIntegrationService.h"
#include "Integrations/Unity/UnityInstallation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace VNH {
namespace {

using Service = VNX::UnityIntegrationService;

Service::Result inspectInstalled(const UnityIntegrationRequest& request)
{
    using StepStatus = Service::StepStatus;
    Service::Result result;
    result.unityProjectPath = request.unityProjectPath.trimmed().isEmpty()
        ? QString() : QDir::cleanPath(QFileInfo(request.unityProjectPath).absoluteFilePath());
    for (const auto& spec : {qMakePair("unity_preflight", "Validate Unity project"),
                             qMakePair("inspect_installed_sdk", "Inspect installed Unity Integration Package")}) {
        Service::StepResult step;
        step.id = QString::fromLatin1(spec.first);
        step.title = QString::fromLatin1(spec.second);
        step.status = StepStatus::Skipped;
        step.summary = QStringLiteral("Not run because a required check failed.");
        result.steps.push_back(step);
    }
    const auto finish = [&]() {
        result.success = true;
        for (const auto& step : result.steps) {
            result.success &= step.status == StepStatus::Succeeded || step.status == StepStatus::Warning;
            result.hasWarnings |= step.status == StepStatus::Warning;
        }
        return result;
    };
    auto& projectStep = result.steps[0];
    const QDir project(result.unityProjectPath);
    if (result.unityProjectPath.isEmpty() || !project.exists(QStringLiteral("Assets"))
        || !project.exists(QStringLiteral("Packages")) || !project.exists(QStringLiteral("ProjectSettings"))) {
        projectStep.status = StepStatus::Failed;
        projectStep.summary = QStringLiteral("Selected directory is not a Unity project root.");
        projectStep.remediation = QStringLiteral("Select the Unity project root containing Assets, Packages, and ProjectSettings.");
        return finish();
    }
    projectStep.status = StepStatus::Succeeded;
    projectStep.summary = QStringLiteral("Unity project layout is valid.");
    projectStep.artifacts << result.unityProjectPath;
    auto& inspection = result.steps[1];
    inspection.status = StepStatus::Failed;
    result.backupPath = Service::previousVersionBackup(result.unityProjectPath);
    result.previousVersionAvailable = !result.backupPath.isEmpty();
    const auto discovery = VNX::UnityInstallation::discover(result.unityProjectPath);
    for (const auto& installation : discovery.installations) inspection.artifacts << installation.path;
    result.targetInstalled = discovery.installations.size() == 1;
    if (discovery.installations.size() == 1) {
        const QString path = discovery.installations.front().path;
        result.integrationInstallPath = project.relativeFilePath(path);
        QFile package(QDir(path).filePath(QStringLiteral("package.json")));
        if (package.open(QIODevice::ReadOnly))
            result.installedVersion = QJsonDocument::fromJson(package.readAll()).object()
                .value(QStringLiteral("version")).toString();
    }
    if (!discovery.errors.isEmpty() || discovery.installations.size() > 1) {
        inspection.summary = discovery.installations.size() > 1
            ? QStringLiteral("Multiple Integration Package installations were found:")
            : QStringLiteral("The installed Integration Package could not be inspected.");
        inspection.details = discovery.errors;
        return finish();
    }
    if (discovery.installations.isEmpty()) {
        inspection.summary = discovery.integrationReferences.isEmpty()
            ? QStringLiteral("No Harken Unity Integration Package is installed in this project.")
            : QStringLiteral("Integration Package references could not be verified.");
        inspection.details = discovery.integrationReferences;
        return finish();
    }
    inspection.status = StepStatus::Succeeded;
    inspection.summary = QStringLiteral("Integration Package %1 is installed at %2.")
                             .arg(result.installedVersion, result.integrationInstallPath);
    return finish();
}

struct Template
{
    QRegularExpression expression;
    const char* source;
};

Template makeTemplate(const char* source)
{
    QString pattern = QRegularExpression::escape(QString::fromUtf8(source));
    pattern.replace(QRegularExpression(QStringLiteral(R"(\\?%\d)")), QStringLiteral("(.*)"));
    return {QRegularExpression(QStringLiteral("\\A") + pattern + QStringLiteral("\\z"),
                               QRegularExpression::DotMatchesEverythingOption), source};
}

const QVector<Template>& templates()
{
    static const QVector<Template> list = [] {
        QVector<Template> result;
        for (const char* source : {
                 QT_TRANSLATE_NOOP("HarkenHub", "Not run because %1 failed."),
                 QT_TRANSLATE_NOOP("HarkenHub", "The selected path cannot be found: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Missing required directories: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "The selected folder is not a valid com.harken.core Integration Package source: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Validated Integration Package %1: %2 file(s)."),
                 QT_TRANSLATE_NOOP("HarkenHub", "No Harken Integration Package is installed; version %1 will be installed at %2."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Version %1 is already installed at %2; installing again replaces the whole directory with the selected package."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Installed version %1 at %2 will be replaced by version %3."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Installed Integration Package %1 at %2: %3 file(s)."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Verified %1 installed Integration Package file(s)."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Integration Package %1 is installed at %2 and will be removed."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Integration Package %1 is installed at %2."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Removed Integration Package %1 from %2."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Version %1 can be restored at %2."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Restored Integration Package %1 at %2."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot remove %1. Close Unity and any program using it, then retry."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Backup verification failed for %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Check write permission for the Hub report directory: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot check whether this Unity project is open: %1 (system error %2). Check project permissions and try again."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot move directory from %1 to %2 (system error %3: %4)."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Check file write protection, directory permissions and processes holding files open at %1. If checkout or unlocking is required, do that first, then retry. The existing Integration Package was not changed."),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot determine the Integration Package identity: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Integration Package discovery exceeded its safety limits at %1. Check local package references and reduce the scanned content."),
                 QT_TRANSLATE_NOOP("HarkenHub", "External package discovery exceeded its safety limits at %1. Point file: dependencies at individual package directories and remove unrelated content before retrying."),
                 QT_TRANSLATE_NOOP("HarkenHub", "External package path has no package.json: %1. Point the file: dependency at the package directory."),
                 QT_TRANSLATE_NOOP("HarkenHub", "A local package path includes the Unity project: %1. Reference a package directory instead."),
                 QT_TRANSLATE_NOOP("HarkenHub", "An Integration Package is referenced by Unity Package Manager: %1. Resolve this dependency before using Hub installation."),
                 QT_TRANSLATE_NOOP("HarkenHub", "An Integration Package is already installed at %1. Use that location or remove it before installing elsewhere."),
                 QT_TRANSLATE_NOOP("HarkenHub", "The installation directory contains unrelated files: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Choose a safe directory inside the Unity project: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "The installation location is not a directory: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Symbolic links and directory junctions are not supported: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Linked paths cannot be inspected safely: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Unsupported filesystem entry: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot inspect directory: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot enumerate directory %1: %2"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Invalid JSON object: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot create directory for %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot create directory: %1"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Copy failed from %1 to %2"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot commit %1: %2"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot write %1: %2"),
                 QT_TRANSLATE_NOOP("HarkenHub", "Cannot read %1: %2"),
             })
            result.push_back(makeTemplate(source));
        return result;
    }();
    return list;
}

QString localizeUnityText(const QString& text)
{
    if (text.isEmpty()) return {};
    const QString exact = QCoreApplication::translate("HarkenHub", text.toUtf8().constData());
    if (exact != text) return exact;
    for (const Template& item : templates()) {
        const QRegularExpressionMatch match = item.expression.match(text);
        if (!match.hasMatch()) continue;
        const QString translated = QCoreApplication::translate("HarkenHub", item.source);
        switch (match.lastCapturedIndex()) {
        case 1: return translated.arg(match.captured(1));
        case 2: return translated.arg(match.captured(1), match.captured(2));
        case 3: return translated.arg(match.captured(1), match.captured(2), match.captured(3));
        default: return translated.arg(match.captured(1), match.captured(2), match.captured(3), match.captured(4));
        }
    }
    if (text.contains(QLatin1Char('\n'))) {
        QStringList lines = text.split(QLatin1Char('\n'));
        for (QString& line : lines) line = localizeUnityText(line);
        return lines.join(QLatin1Char('\n'));
    }
    return text;
}

QStringList localizeUnityTexts(const QStringList& texts)
{
    QStringList localized;
    localized.reserve(texts.size());
    for (const QString& text : texts) localized << localizeUnityText(text);
    return localized;
}

}

UnityIntegrationResult HubUnityIntegrationService::run(const UnityIntegrationRequest& request)
{
    Service::Request serviceRequest;
    serviceRequest.unityProjectPath = request.unityProjectPath;
    serviceRequest.integrationSourcePath = request.integrationSourcePath;
    serviceRequest.integrationInstallPath = request.integrationInstallPath;
    serviceRequest.checkOnly = request.checkOnly;
    serviceRequest.removeIntegration = request.removeIntegration;
    serviceRequest.restorePrevious = request.restorePrevious;
    Service service;
    UnityIntegrationResult result{request.inspectOnly ? inspectInstalled(request) : service.run(serviceRequest)};
    for (auto& step : result.steps) {
        step.title = localizeUnityText(step.title);
        step.summary = localizeUnityText(step.summary);
        step.cause = localizeUnityText(step.cause);
        step.remediation = localizeUnityText(step.remediation);
        step.details = localizeUnityTexts(step.details);
    }
    return result;
}

QString HubUnityIntegrationService::localizeText(const QString& text)
{
    return localizeUnityText(text);
}

}
