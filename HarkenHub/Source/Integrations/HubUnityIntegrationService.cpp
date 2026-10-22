#include "Integrations/HubUnityIntegrationService.h"
#include "Integrations/IntegrationProgress.h"

#include "Integrations/Unity/UnityIntegrationService.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Integrations/Unity/UnityProjectAccess.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace VNH {
namespace {

VNX::UnityIntegrationService::Result verifyInstalled(
    const UnityIntegrationRequest& request)
{
    using Service = VNX::UnityIntegrationService;
    using StepStatus = Service::StepStatus;
    Service::Result result;
    result.unityProjectPath = request.unityProjectPath.trimmed().isEmpty()
        ? QString() : QDir::cleanPath(QFileInfo(request.unityProjectPath).absoluteFilePath());
    for (const auto& spec : {qMakePair("unity_preflight", "Validate Unity project"),
                             qMakePair("inspect_installed_sdk", "Inspect installed Unity SDK"),
                             qMakePair("verify", "Verify installed SDK")}) {
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
            result.success &= step.status == StepStatus::Succeeded;
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
    if (VNX::UnityInstallation::hasPendingMigration(result.unityProjectPath)) {
        inspection.summary = QStringLiteral("An unfinished SDK migration must be recovered before continuing.");
        return finish();
    }
    const auto discovery = VNX::UnityInstallation::discover(result.unityProjectPath);
    for (const auto& installation : discovery.installations) inspection.artifacts << installation.path;
    result.integrationFound = !discovery.installations.isEmpty();
    result.targetInstalled = result.integrationFound;
    if (discovery.installations.size() == 1)
        result.sdkInstallPath = project.relativeFilePath(discovery.installations.front().path);
    if (!discovery.errors.isEmpty() || discovery.installations.size() > 1) {
        inspection.summary = discovery.installations.size() > 1
            ? QStringLiteral("Multiple SDK installations were found:")
            : QStringLiteral("The installed SDK could not be inspected.");
        inspection.details = discovery.errors;
        return finish();
    }
    if (discovery.installations.isEmpty()) {
        inspection.summary = discovery.sdkReferences.isEmpty()
            ? QStringLiteral("No Harken Unity SDK is installed in this project.")
            : QStringLiteral("SDK package references could not be verified.");
        inspection.details = discovery.sdkReferences;
        return finish();
    }
    const auto& installation = discovery.installations.front();
    QString error;
    if (!VNX::UnityInstallation::validateTree(installation.path, &error)) {
        inspection.summary = QStringLiteral("The installed SDK could not be inspected.");
        inspection.cause = error;
        return finish();
    }
    QFile stateFile(QDir(installation.path).filePath(QStringLiteral(".harken-integration-state.json")));
    const QJsonObject state = stateFile.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(stateFile.readAll()).object() : QJsonObject();
    bool validOwnership = state.value(QStringLiteral("schema_version")).toInt(-1) == 1
        && state.value(QStringLiteral("package_name")).toString() == QStringLiteral("com.harken.core")
        && state.value(QStringLiteral("managed_files")).isArray();
    QSet<QString> managedFiles;
    for (const auto& entry : state.value(QStringLiteral("managed_files")).toArray()) {
        validOwnership &= entry.isString() && VNX::UnityInstallation::safeRelativePath(entry.toString());
        managedFiles.insert(entry.toString());
    }
    if (!validOwnership || managedFiles.isEmpty()) {
        inspection.summary = QStringLiteral("The SDK ownership record is missing, invalid or unreadable. Restore it from backup before updating or removing the SDK.");
        return finish();
    }
    const QJsonObject hashes = state.value(QStringLiteral("sha256")).toObject();
    const QRegularExpression hashFormat(QStringLiteral("\\A[0-9a-fA-F]{64}\\z"));
    for (const QString& relative : managedFiles) {
        if (!hashFormat.match(hashes.value(relative).toString()).hasMatch()) {
            inspection.summary = QStringLiteral("The installed SDK has incomplete or invalid verification data.");
            inspection.details << relative;
            inspection.remediation = QStringLiteral("Restore the installation record from a trusted backup before verifying again.");
            return finish();
        }
    }
    if (!managedFiles.contains(QStringLiteral("package.json"))
        || !managedFiles.contains(QStringLiteral("harken-integration.json"))) {
        inspection.summary = QStringLiteral("The installed SDK has incomplete or invalid verification data.");
        return finish();
    }
    inspection.status = StepStatus::Succeeded;
    inspection.summary = QStringLiteral("The installation record is complete. File contents are checked in the next step.");
    auto& verification = result.steps[2];
    verification.artifacts << installation.path;
    auto& missing = result.missingFiles;
    auto& modified = result.modifiedFiles;
    auto& unreadable = result.unreadableFiles;
    QStringList files = managedFiles.values();
    files.sort();
    for (const QString& relative : files) {
        IntegrationProgress::inspectedEntry();
        const QString path = QDir(installation.path).filePath(relative);
        const QFileInfo info(path);
        QFile file(path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!info.exists()) {
            if (VNX::UnityInstallation::isDocumentationOrSample(relative))
                result.missingOptionalFiles << relative;
            else
                missing << relative;
        }
        else if (!info.isFile() || !file.open(QIODevice::ReadOnly) || !IntegrationProgress::addFileData(file, hash))
            unreadable << relative;
        else if (hash.result().toHex() != hashes.value(relative).toString().toLatin1().toLower()) {
            if (VNX::UnityInstallation::matchesTextHashIgnoringLineEndings(
                    path, hashes.value(relative).toString().toLatin1()))
                result.lineEndingFiles << relative;
            else
                modified << relative;
        }
    }
    if (!result.lineEndingFiles.isEmpty())
        verification.details << QStringLiteral("%1 SDK text file(s) differ only in LF/CRLF line endings; contents are unchanged.")
                                    .arg(result.lineEndingFiles.size())
                             << result.lineEndingFiles.join(QLatin1Char('\n'));
    if (!result.missingOptionalFiles.isEmpty()) {
        verification.details << QStringLiteral("Missing documentation/sample files (does not block SDK updates):")
                             << result.missingOptionalFiles.join(QLatin1Char('\n'));
    }
    result.changesDetected = !missing.isEmpty() || !modified.isEmpty();
    if (result.changesDetected || !unreadable.isEmpty()) {
        verification.status = StepStatus::Failed;
        QStringList issues;
        QStringList guidance;
        if (!modified.isEmpty()) {
            issues << QStringLiteral("%1 SDK file(s) differ from their contents at installation.").arg(modified.size());
            guidance << QStringLiteral("Review the listed content differences to confirm whether they were intentional. To restore the original SDK files, use Change SDK and select the complete package for the installed version.");
            verification.details << QStringLiteral("SDK files with contents different from installation:") << modified.join(QLatin1Char('\n'));
        }
        if (!missing.isEmpty()) {
            issues << QStringLiteral("%1 SDK file(s) recorded at installation are missing.").arg(missing.size());
            guidance << QStringLiteral("Restore the listed missing files from a matching backup, or use Change SDK and select the complete package for the installed version.");
            verification.details << QStringLiteral("Missing SDK files:") << missing.join(QLatin1Char('\n'));
        }
        if (!unreadable.isEmpty()) {
            issues << QStringLiteral("%1 SDK file(s) could not be read, so verification could not finish.").arg(unreadable.size());
            guidance << QStringLiteral("Check read permissions and whether another program is preventing access to the listed files, then check the SDK status again.");
            verification.details << QStringLiteral("Unreadable SDK files:") << unreadable.join(QLatin1Char('\n'));
        }
        verification.summary = issues.join(QLatin1Char('\n'));
        verification.remediation = guidance.join(QStringLiteral("\n\n"));
        return finish();
    }
    verification.status = StepStatus::Succeeded;
    verification.summary = QStringLiteral("Verified the content integrity of %1 Harken-managed SDK file(s).")
                               .arg(files.size() - result.missingOptionalFiles.size());
    if (!result.missingOptionalFiles.isEmpty())
        verification.summary += QLatin1Char('\n') + QStringLiteral("%1 documentation/sample file(s) are missing; SDK updates can continue.")
            .arg(result.missingOptionalFiles.size());
    return finish();
}

QString localizeCountSummary(const QString& text)
{
    static const QRegularExpression expression(QStringLiteral(
        R"(^(\d+) missing SDK file\(s\) will be added; (\d+) existing SDK file\(s\) need updating; (\d+) obsolete Harken-managed file\(s\) will be removed; (\d+) project-specific file\(s\) will be kept unchanged; (\d+) Unity-generated metadata file\(s\) and (\d+) optional target runtime file\(s\) will be kept automatically$)"));
    const QRegularExpressionMatch match = expression.match(text);
    if (!match.hasMatch())
        return text;
    return QCoreApplication::translate(
               "HarkenHub",
               "%1 missing SDK file(s) will be added; %2 existing SDK file(s) need updating; %3 obsolete Harken-managed file(s) will be removed; %4 project-specific file(s) will be kept unchanged; %5 Unity-generated metadata file(s) and %6 optional target runtime file(s) will be kept automatically")
        .arg(match.captured(1), match.captured(2), match.captured(3),
             match.captured(4), match.captured(5), match.captured(6));
}

QString localizeUnityText(const QString& text)
{
    if (text.isEmpty())
        return {};
    const QByteArray sourceUtf8 = text.toUtf8();
    const QString translatedText =
        QCoreApplication::translate("HarkenHub", sourceUtf8.constData());
    if (translatedText != text)
        return translatedText;

    if (text == QStringLiteral("Validate Unity project"))
        return QCoreApplication::translate("HarkenHub", "Validate Unity project");
    if (text == QStringLiteral("Validate Harken Unity SDK source"))
        return QCoreApplication::translate("HarkenHub", "Validate Harken Unity SDK source");
    if (text == QStringLiteral("Compare installed Unity SDK"))
        return QCoreApplication::translate("HarkenHub", "Compare installed Unity SDK");
    if (text == QStringLiteral("Install or update Unity SDK"))
        return QCoreApplication::translate("HarkenHub", "Install or update Unity SDK");
    if (text == QStringLiteral("Verify installed SDK"))
        return QCoreApplication::translate("HarkenHub", "Verify installed SDK");
    if (text == QStringLiteral("Write integration report"))
        return QCoreApplication::translate("HarkenHub", "Write integration report");

    if (text == QStringLiteral("Unity project layout and write access are valid."))
        return QCoreApplication::translate(
            "HarkenHub", "Unity project layout and write access are valid.");
    if (text == QStringLiteral(
            "Installed SDK verification is represented by the differences reported above."))
        return QCoreApplication::translate(
            "HarkenHub",
            "Installed SDK verification is represented by the differences reported above.");
    if (text == QStringLiteral(
            "Wrote machine-readable and human-readable SDK integration reports."))
        return QCoreApplication::translate(
            "HarkenHub",
            "Wrote machine-readable and human-readable SDK integration reports.");
    if (text == QStringLiteral(
            "Check-only mode requested; no SDK files were changed."))
        return QCoreApplication::translate(
            "HarkenHub", "Check-only mode requested; no SDK files were changed.");
    if (text == QStringLiteral(
            "The installed SDK could not be compared because its ownership record is unreadable."))
        return QCoreApplication::translate(
            "HarkenHub",
            "The installed SDK could not be compared because its ownership record is unreadable.");
    if (text == QStringLiteral(
            "Unity includes this runtime in Windows games and uses it for Windows Editor play mode."))
        return QCoreApplication::translate(
            "HarkenHub",
            "Unity includes this runtime in Windows games and uses it for Windows Editor play mode.");
    if (text == QStringLiteral(
            "Unity includes this runtime in Linux games built from the project."))
        return QCoreApplication::translate(
            "HarkenHub",
            "Unity includes this runtime in Linux games built from the project.");
    if (text == QStringLiteral(
            "Unity packages this runtime into Android APK or AAB builds so Harken audio can run on Android devices."))
        return QCoreApplication::translate(
            "HarkenHub",
            "Unity packages this runtime into Android APK or AAB builds so Harken audio can run on Android devices.");
    if (text == QStringLiteral(
            "Unity includes this runtime in macOS games and uses it for macOS Editor play mode."))
        return QCoreApplication::translate(
            "HarkenHub",
            "Unity includes this runtime in macOS games and uses it for macOS Editor play mode.");
    if (text == QStringLiteral(
            "Unity adds this static runtime to the generated Xcode project for the final iOS application."))
        return QCoreApplication::translate(
            "HarkenHub",
            "Unity adds this static runtime to the generated Xcode project for the final iOS application.");
    if (text == QStringLiteral(
            "Unity includes this runtime when it builds games for this target platform."))
        return QCoreApplication::translate(
            "HarkenHub",
            "Unity includes this runtime when it builds games for this target platform.");

    const auto translateMatch = [&text](const QRegularExpression& expression,
                                         const char* source) {
        const QRegularExpressionMatch match = expression.match(text);
        if (!match.hasMatch())
            return QString();
        QString translated = QCoreApplication::translate("HarkenHub", source);
        for (int index = 1; index <= match.lastCapturedIndex(); ++index)
            translated = translated.arg(match.captured(index));
        return translated;
    };

    static const struct {
        QRegularExpression expression;
        const char* source;
    } installationMessages[] = {
        {QRegularExpression(QStringLiteral(R"(^Cannot check whether this Unity project is open: (.*) \(system error ([0-9]+)\)\. Check project permissions and try again\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot check whether this Unity project is open: %1 (system error %2). Check project permissions and try again.")},
        {QRegularExpression(QStringLiteral(R"(^(\d+) SDK file\(s\) differ from their contents at installation\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "%1 SDK file(s) differ from their contents at installation.")},
        {QRegularExpression(QStringLiteral(R"(^(\d+) SDK file\(s\) recorded at installation are missing\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "%1 SDK file(s) recorded at installation are missing.")},
        {QRegularExpression(QStringLiteral(R"(^(\d+) SDK file\(s\) could not be read, so verification could not finish\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "%1 SDK file(s) could not be read, so verification could not finish.")},
        {QRegularExpression(QStringLiteral(R"(^Cannot move directory from (.*) to (.*) \(system error ([0-9]+): (.*)\)\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot move directory from %1 to %2 (system error %3: %4).")},
        {QRegularExpression(QStringLiteral(R"(^Check file write protection, directory permissions and processes holding files open at (.*)\. If checkout or unlocking is required, do that first, then retry\. The existing SDK was not changed\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Check file write protection, directory permissions and processes holding files open at %1. If checkout or unlocking is required, do that first, then retry. The existing SDK was not changed.")},
        {QRegularExpression(QStringLiteral(R"(^Check-only mode did not change (.*)\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Check-only mode did not change %1.")},
        {QRegularExpression(QStringLiteral(R"(^Ready to remove (\d+) Harken SDK/runtime file\(s\) and preserve (\d+) project-specific file\(s\)\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Ready to remove %1 Harken SDK/runtime file(s) and preserve %2 project-specific file(s).")},
        {QRegularExpression(QStringLiteral(R"(^Installed package: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Installed package: %1")},
        {QRegularExpression(QStringLiteral(R"(^Safety backup: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Safety backup: %1")},
        {QRegularExpression(QStringLiteral(R"(^To integrate again later, select or drop this SDK source: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "To integrate again later, select or drop this SDK source: %1")},
        {QRegularExpression(QStringLiteral(R"(^Backed up and removed the Harken SDK and native runtimes; preserved (\d+) project-specific file\(s\)\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Backed up and removed the Harken SDK and native runtimes; preserved %1 project-specific file(s).")},
        {QRegularExpression(QStringLiteral(R"(^Installed SDK (.*) from a verified staging directory; copied (\d+) managed file\(s\) and preserved (\d+) target-only file\(s\)\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Installed SDK %1 from a verified staging directory; copied %2 managed file(s) and preserved %3 target-only file(s).")},
        {QRegularExpression(QStringLiteral(R"(^Verified the content integrity of (\d+) Harken-managed SDK file\(s\)\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Verified the content integrity of %1 Harken-managed SDK file(s).")},
        {QRegularExpression(QStringLiteral(R"(^(\d+) documentation/sample file\(s\) are missing; SDK updates can continue\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "%1 documentation/sample file(s) are missing; SDK updates can continue.")},
        {QRegularExpression(QStringLiteral(R"(^(\d+) SDK text file\(s\) differ only in LF/CRLF line endings; contents are unchanged\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "%1 SDK text file(s) differ only in LF/CRLF line endings; contents are unchanged.")},
        {QRegularExpression(QStringLiteral(R"(^Cannot read SDK file for migration: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot read SDK file for migration: %1")},
        {QRegularExpression(QStringLiteral(R"(^Cannot read SDK file: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot read SDK file: %1")},
        {QRegularExpression(QStringLiteral(R"(^Cannot write (.*): (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot write %1: %2")},
        {QRegularExpression(QStringLiteral(R"(^Check write permission for the Hub report directory: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Check write permission for the Hub report directory: %1")},
        {QRegularExpression(QStringLiteral(R"(^Cannot create directory for (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot create directory for %1")},
        {QRegularExpression(QStringLiteral(R"(^Copy failed from (.*) to (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Copy failed from %1 to %2")},
        {QRegularExpression(QStringLiteral(R"(^Cannot commit (.*): (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot commit %1: %2")},
        {QRegularExpression(QStringLiteral(R"(^Cannot read (.*): (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot read %1: %2")},
        {QRegularExpression(QStringLiteral(R"(^Cannot enumerate directory (.*): (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot enumerate directory %1: %2")},
        {QRegularExpression(QStringLiteral(R"(^Invalid JSON object: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Invalid JSON object: %1")},
        {QRegularExpression(QStringLiteral(R"(^Cannot determine the SDK package identity: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot determine the SDK package identity: %1")},
        {QRegularExpression(QStringLiteral(R"(^SDK discovery exceeded its safety limits at (.*)\. Check local package references and reduce the scanned content\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "SDK discovery exceeded its safety limits at %1. Check local package references and reduce the scanned content.")},
        {QRegularExpression(QStringLiteral(R"(^External package discovery exceeded its safety limits at (.*)\. Point file: dependencies at individual package directories and remove unrelated content before retrying\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "External package discovery exceeded its safety limits at %1. Point file: dependencies at individual package directories and remove unrelated content before retrying.")},
        {QRegularExpression(QStringLiteral(R"(^External package path has no package\.json: (.*)\. Point the file: dependency at the package directory\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "External package path has no package.json: %1. Point the file: dependency at the package directory.")},
        {QRegularExpression(QStringLiteral(R"(^A local package path includes the Unity project: (.*)\. Reference a package directory instead\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "A local package path includes the Unity project: %1. Reference a package directory instead.")},
        {QRegularExpression(QStringLiteral(R"(^Cannot inspect directory: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot inspect directory: %1")},
        {QRegularExpression(QStringLiteral(R"(^Cannot create directory: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot create directory: %1")},
        {QRegularExpression(QStringLiteral(R"(^Linked paths cannot be inspected safely: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Linked paths cannot be inspected safely: %1")},
        {QRegularExpression(QStringLiteral(R"(^Unsupported filesystem entry: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Unsupported filesystem entry: %1")},
        {QRegularExpression(QStringLiteral(R"(^Symbolic links and directory junctions are not supported: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Symbolic links and directory junctions are not supported: %1")},
        {QRegularExpression(QStringLiteral(R"(^Choose a safe directory inside the Unity project: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Choose a safe directory inside the Unity project: %1")},
        {QRegularExpression(QStringLiteral(R"(^The installation location is not a directory: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "The installation location is not a directory: %1")},
        {QRegularExpression(QStringLiteral(R"(^An SDK is referenced by Unity Package Manager: (.*)\. Resolve this dependency before using Hub installation\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "An SDK is referenced by Unity Package Manager: %1. Resolve this dependency before using Hub installation.")},
        {QRegularExpression(QStringLiteral(R"(^SDK files without an identifiable installation root: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "SDK files without an identifiable installation root: %1")},
        {QRegularExpression(QStringLiteral(R"(^An SDK is already installed at (.*)\. Use that location or remove it before installing elsewhere\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "An SDK is already installed at %1. Use that location or remove it before installing elsewhere.")},
        {QRegularExpression(QStringLiteral(R"(^The installation directory contains unrelated files: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "The installation directory contains unrelated files: %1")},
        {QRegularExpression(QStringLiteral(R"(^(.*) could not establish which files belong to Harken\. Nothing was compared or changed\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "%1 could not establish which files belong to Harken. Nothing was compared or changed.")},
    };
    for (const auto& message : installationMessages) {
        const auto match = message.expression.match(text);
        if (!match.hasMatch()) continue;
        const QString translated = QCoreApplication::translate("HarkenHub", message.source);
        return match.lastCapturedIndex() == 4
            ? translated.arg(match.captured(1), match.captured(2), match.captured(3), match.captured(4))
            : match.lastCapturedIndex() == 3
            ? translated.arg(match.captured(1), match.captured(2), match.captured(3))
            : match.lastCapturedIndex() == 2
            ? translated.arg(match.captured(1), match.captured(2))
            : translated.arg(match.captured(1));
    }

    const QString validatedSdk = translateMatch(
        QRegularExpression(
            QStringLiteral(R"(^Validated SDK (.*): ([0-9]+) file\(s\); the (.*) runtime required for this editor is present\.(.*)$)")),
        "Validated SDK %1: %2 file(s); the %3 runtime required for this editor is present.");
    if (!validatedSdk.isEmpty()) {
        const QRegularExpressionMatch match = QRegularExpression(
            QStringLiteral(R"(^Validated SDK (.*): ([0-9]+) file\(s\); the (.*) runtime required for this editor is present\.(.*)$)"))
            .match(text);
        QString result = QCoreApplication::translate(
            "HarkenHub",
            "Validated SDK %1: %2 file(s); the %3 runtime required for this editor is present.")
                             .arg(match.captured(1), match.captured(2), match.captured(3));
        QString suffix = match.captured(4);
        const auto appendMatch = [&suffix, &result](const QRegularExpression& expression,
                                                      const char* source) {
            const QRegularExpressionMatch match = expression.match(suffix);
            if (!match.hasMatch())
                return false;
            QString translated = QCoreApplication::translate("HarkenHub", source);
            for (int index = 1; index <= match.lastCapturedIndex(); ++index)
                translated = translated.arg(match.captured(index));
            result += translated;
            suffix = suffix.mid(match.capturedLength());
            return true;
        };
        appendMatch(
            QRegularExpression(QStringLiteral(
                R"(^ Target runtimes not included: (.*)\. Available platforms can still be integrated\.)")),
            " Target runtimes not included: %1. Available platforms can still be integrated.");
        appendMatch(
            QRegularExpression(QStringLiteral(
                R"(^ Runtimes that require another editor operating system are not included: (.*)\.)")),
            " Runtimes that require another editor operating system are not included: %1.");
        return result + suffix;
    }

    const QString availableRuntimes = translateMatch(
        QRegularExpression(
            QStringLiteral(R"(^Native runtimes available in this SDK source: (.*)$)")),
        "Native runtimes available in this SDK source: %1");
    if (!availableRuntimes.isEmpty())
        return availableRuntimes;

    const QRegularExpressionMatch sdkDifferences = QRegularExpression(
        QStringLiteral(R"(^Check found SDK differences: (.*)\.$)"))
        .match(text);
    if (sdkDifferences.hasMatch())
        return QCoreApplication::translate(
                   "HarkenHub", "Check found SDK differences: %1.")
            .arg(localizeCountSummary(sdkDifferences.captured(1)));

    const QString readyToInstall = translateMatch(
        QRegularExpression(
            QStringLiteral(R"(^No embedded Harken SDK is installed; ([0-9]+) file\(s\) are ready to install\.$)")),
        "No embedded Harken SDK is installed; %1 file(s) are ready to install.");
    if (!readyToInstall.isEmpty())
        return readyToInstall;

    const QRegularExpressionMatch approvedUpdate = QRegularExpression(
        QStringLiteral(R"(^Approved SDK update: (.*)\.$)"))
        .match(text);
    if (approvedUpdate.hasMatch())
        return QCoreApplication::translate(
                   "HarkenHub", "Approved SDK update: %1.")
            .arg(localizeCountSummary(approvedUpdate.captured(1)));

    const QString exactSdk = translateMatch(
        QRegularExpression(
            QStringLiteral(R"(^The installed SDK exactly matches selected SDK source (.*)\.$)")),
        "The installed SDK exactly matches selected SDK source %1.");
    if (!exactSdk.isEmpty())
        return exactSdk;

    const QString managedCurrent = translateMatch(
        QRegularExpression(QStringLiteral(R"(^All Harken-managed files match SDK (.*); (\d+) Unity metadata file\(s\) and (\d+) optional target runtime file\(s\) are retained as expected\.$)")),
        "All Harken-managed files match SDK %1; %2 Unity metadata file(s) and %3 optional target runtime file(s) are retained as expected.");
    if (!managedCurrent.isEmpty())
        return managedCurrent;

    const QString projectFilesNeedReview = translateMatch(
        QRegularExpression(QStringLiteral(R"(^All Harken-managed files match SDK (.*); (\d+) project-specific file\(s\) need review, while (\d+) Unity metadata and (\d+) optional runtime file\(s\) are retained normally\.$)")),
        "All Harken-managed files match SDK %1; %2 project-specific file(s) need review, while %3 Unity metadata and %4 optional runtime file(s) are retained normally.");
    if (!projectFilesNeedReview.isEmpty())
        return projectFilesNeedReview;

    if (text == QStringLiteral("The project-specific files are outside Harken SDK ownership, so the check cannot determine whether they are still needed."))
        return QCoreApplication::translate("HarkenHub", "The project-specific files are outside Harken SDK ownership, so the check cannot determine whether they are still needed.");
    if (text == QStringLiteral("Review only the project-specific files listed below. Unity-generated metadata requires no action."))
        return QCoreApplication::translate("HarkenHub", "Review only the project-specific files listed below. Unity-generated metadata requires no action.");

    const QRegularExpressionMatch managedDiffers = QRegularExpression(
        QStringLiteral(
            R"(^The existing SDK differs from selected SDK source (.*): (.*)\.$)"))
        .match(text);
    if (managedDiffers.hasMatch())
        return QCoreApplication::translate(
                   "HarkenHub",
                   "The existing SDK differs from selected SDK source %1: %2.")
            .arg(managedDiffers.captured(1),
                 localizeCountSummary(managedDiffers.captured(2)));

    const QString targetRuntimes = translateMatch(
        QRegularExpression(
            QStringLiteral(R"(^Target runtimes deferred to another operating system: (.*)$)")),
        "Target runtimes deferred to another operating system: %1");
    if (!targetRuntimes.isEmpty())
        return targetRuntimes;

    const QString missingTargetCause = translateMatch(
        QRegularExpression(QStringLiteral(
            R"(^The (.*) files in the selected SDK source are complete, but the listed target platforms have no verified runtime in this source\. Each target is independent, so their absence does not prevent (.*) integration\.$)")),
        "The %1 files in the selected SDK source are complete, but the listed target platforms have no verified runtime in this source. Each target is independent, so their absence does not prevent %1 integration.");
    if (!missingTargetCause.isEmpty())
        return missingTargetCause;

    const QString missingTargetPolicy = translateMatch(
        QRegularExpression(QStringLiteral(
            R"(^Those targets cannot be completed on (.*) under the Harken integration policy, so their missing runtimes do not block this integration\.$)")),
        "Those targets cannot be completed on %1 under the Harken integration policy, so their missing runtimes do not block this integration.");
    if (!missingTargetPolicy.isEmpty())
        return missingTargetPolicy;

    if (text == QStringLiteral(
            "You may continue integrating and use the available platforms. Before building for a missing target, select a complete SDK source that includes its verified runtime, then run Check only again. Do not copy unknown runtime files from another Unity project."))
        return QCoreApplication::translate(
            "HarkenHub",
            "You may continue integrating and use the available platforms. Before building for a missing target, select a complete SDK source that includes its verified runtime, then run Check only again. Do not copy unknown runtime files from another Unity project.");
    if (text == QStringLiteral(
            "Before building targets that require another operating system, open the Unity project with Harken Studio there and run Check and integrate again. Apple targets must be completed on macOS."))
        return QCoreApplication::translate(
            "HarkenHub",
            "Before building targets that require another operating system, open the Unity project with Harken Studio there and run Check and integrate again. Apple targets must be completed on macOS.");

    const QRegularExpression missingRuntimeExpression(QStringLiteral(
        R"(^Missing component: (.*) game runtime\nExpected inside the selected SDK source: (.*)\nPurpose: (.*)\nWhat this means: This SDK source cannot safely prepare the Unity project for (.*) builds until a verified runtime is restored\.$)"));
    const QRegularExpressionMatch missingRuntimeMatch =
        missingRuntimeExpression.match(text);
    if (missingRuntimeMatch.hasMatch()) {
        QString translated = QCoreApplication::translate(
            "HarkenHub",
            "Missing component: %1 game runtime\nExpected inside the selected SDK source: %2\nPurpose: %3\nWhat this means: This SDK source cannot safely prepare the Unity project for %4 builds until a verified runtime is restored.");
        return translated.arg(
            missingRuntimeMatch.captured(1), missingRuntimeMatch.captured(2),
            localizeUnityText(missingRuntimeMatch.captured(3)),
            missingRuntimeMatch.captured(4));
    }

    if (text.contains(QLatin1Char('\n'))) {
        const QStringList lines = text.split(QLatin1Char('\n'));
        QStringList localizedLines;
        localizedLines.reserve(lines.size());
        bool changed = false;
        for (const QString& line : lines) {
            const QString localized = localizeUnityText(line);
            changed |= localized != line;
            localizedLines.push_back(localized);
        }
        if (changed)
            return localizedLines.join(QLatin1Char('\n'));
    }

    const QString missingRuntime = translateMatch(
        QRegularExpression(QStringLiteral(
            R"(^Missing component: (.*) game runtime\nExpected inside the selected SDK source: (.*)\nPurpose: (.*)\nWhat this means: This SDK source cannot safely prepare the Unity project for (.*) builds until a verified runtime is restored\.$)")),
        "Missing component: %1 game runtime\nExpected inside the selected SDK source: %2\nPurpose: %3\nWhat this means: This SDK source cannot safely prepare the Unity project for %4 builds until a verified runtime is restored.");
    if (!missingRuntime.isEmpty())
        return missingRuntime;

    return text;
}

QStringList localizeUnityTexts(const QStringList& texts)
{
    QStringList localized;
    localized.reserve(texts.size());
    for (const QString& text : texts)
        localized << localizeUnityText(text);
    return localized;
}

UnityIntegrationStepStatus convertStatus(
    VNX::UnityIntegrationService::StepStatus status)
{
    using Source = VNX::UnityIntegrationService::StepStatus;
    switch (status) {
        case Source::Pending:
            return UnityIntegrationStepStatus::Pending;
        case Source::Running:
            return UnityIntegrationStepStatus::Running;
        case Source::Succeeded:
            return UnityIntegrationStepStatus::Succeeded;
        case Source::Warning:
            return UnityIntegrationStepStatus::Warning;
        case Source::Failed:
            return UnityIntegrationStepStatus::Failed;
        case Source::Skipped:
            return UnityIntegrationStepStatus::Skipped;
    }
    return UnityIntegrationStepStatus::Failed;
}

}

UnityIntegrationResult HubUnityIntegrationService::run(
    const UnityIntegrationRequest& request)
{
    QString accessError;
    if (!request.verifyInstalledOnly && !request.unityProjectPath.trimmed().isEmpty()
        && !VNX::checkUnityProjectClosed(request.unityProjectPath, &accessError)) {
        UnityIntegrationResult result;
        result.unityProjectPath = QDir::cleanPath(QFileInfo(request.unityProjectPath).absoluteFilePath());
        UnityIntegrationStep step;
        step.id = QStringLiteral("unity_preflight");
        step.title = localizeUnityText(QStringLiteral("Validate Unity project"));
        step.status = UnityIntegrationStepStatus::Failed;
        step.summary = localizeUnityText(accessError);
        result.steps.push_back(step);
        return result;
    }
    VNX::UnityIntegrationService::Request studioRequest;
    studioRequest.unityProjectPath = request.unityProjectPath;
    studioRequest.sdkSourcePath = request.sdkSourcePath;
    studioRequest.sdkInstallPath = request.sdkInstallPath;
    studioRequest.installToAssets = request.installToAssets;
    studioRequest.checkOnly = request.checkOnly;
    studioRequest.confirmTargetChanges = request.confirmTargetChanges;
    studioRequest.approvedPlanHash = request.approvedPlanHash;
    studioRequest.removeIntegration = request.removeIntegration;
    studioRequest.confirmRemoval = request.confirmRemoval;
    studioRequest.removalBackupPath = request.removalBackupPath;
    studioRequest.migrateIntegration = request.migrateIntegration;
    studioRequest.recoverMigration = request.recoverMigration;
    studioRequest.confirmMigration = request.confirmMigration;
    studioRequest.migrationTargetPath = request.migrationTargetPath;
    studioRequest.migrationBackupPath = request.migrationBackupPath;
    studioRequest.migrationPlanHash = request.migrationPlanHash;
    VNX::UnityIntegrationService service;
    const auto source = request.verifyInstalledOnly ? verifyInstalled(request) : service.run(studioRequest);
    UnityIntegrationResult result;
    result.success = source.success;
    result.hasWarnings = source.hasWarnings;
    result.requiresConfirmation = source.requiresConfirmation;
    result.targetAlreadyCurrent = source.targetAlreadyCurrent;
    result.targetCurrent = source.targetCurrent;
    result.targetInstalled = source.targetInstalled;
    result.integrationFound = source.integrationFound;
    result.integrationRemoved = source.integrationRemoved;
    result.removalBackupCreated = source.removalBackupCreated;
    result.changesDetected = source.changesDetected;
    result.planHash = source.planHash;
    result.modifiedFiles = source.modifiedFiles;
    result.lineEndingFiles = source.lineEndingFiles;
    result.missingFiles = source.missingFiles;
    result.missingOptionalFiles = source.missingOptionalFiles;
    result.unreadableFiles = source.unreadableFiles;
    result.integrationMigrated = source.integrationMigrated;
    result.migrationRecovered = source.migrationRecovered;
    result.migrationBackupPath = source.migrationBackupPath;
    result.migrationTargetPath = source.migrationTargetPath;
    result.migrationPlanHash = source.migrationPlanHash;
    result.unityProjectPath = source.unityProjectPath;
    result.sdkSourcePath = source.sdkSourcePath;
    result.sdkInstallPath = source.sdkInstallPath;
    result.removalBackupPath = source.removalBackupPath;
    result.jsonReportPath = source.jsonReportPath;
    result.textReportPath = source.textReportPath;
    result.sdkFilesCopied = source.sdkFilesCopied;
    result.filesAdded = source.filesAdded;
    result.filesChanged = source.filesChanged;
    result.staleFilesToRemove = source.staleFilesToRemove;
    result.targetOnlyFilesPreserved = source.targetOnlyFilesPreserved;
    result.filesRemoved = source.filesRemoved;
    result.fileChanges.reserve(source.fileChanges.size());
    for (const auto& change : source.fileChanges)
        result.fileChanges.push_back({change.action, change.relative});
    result.steps.reserve(source.steps.size());
    for (const auto& step : source.steps) {
        result.steps.push_back({
            step.id,
            localizeUnityText(step.title),
            convertStatus(step.status),
            localizeUnityText(step.summary),
            localizeUnityText(step.cause),
            localizeUnityText(step.remediation),
            localizeUnityTexts(step.details),
            step.artifacts,
            step.elapsedMs,
        });
    }
    return result;
}

QString HubUnityIntegrationService::localizeText(const QString& text)
{
    return localizeUnityText(text);
}

}
