#include "Integrations/Unity/UnityIntegrationService.h"
#include "Integrations/IntegrationFileSystem.h"
#include "Integrations/IntegrationProgress.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Integrations/Unity/UnityProjectAccess.h"
#include "Platform/PlatformPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QThread>

#include <filesystem>
#include <functional>
#include <memory>

namespace VNX {
namespace {

using Service = UnityIntegrationService;
using StepStatus = Service::StepStatus;
using StepResult = Service::StepResult;

constexpr auto kPackageName = "com.harken.core";
constexpr auto kBackupRecord = "backup.json";
constexpr auto kBackupPackage = "package";
constexpr auto kBackupFolderMeta = "package.meta";

struct StepSpec
{
    const char* id;
    const char* title;
};

QString cleanAbsolutePath(const QString& path)
{
    if (path.trimmed().isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString normalizePath(QString path)
{
    path.replace('\\', '/');
    return path;
}

bool ignoredRelativePath(const QString& relative)
{
    const QString fileName = relative.section(QLatin1Char('/'), -1);
    return fileName == QStringLiteral(".DS_Store") || fileName.startsWith(QStringLiteral("._"))
        || relative.startsWith(QStringLiteral(".git/")) || relative.startsWith(QStringLiteral(".cache/"))
        || relative.startsWith(QStringLiteral("Unreal/"));
}

QStringList allFiles(const QString& root)
{
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        VNH::IntegrationProgress::inspectedEntry();
        files << normalizePath(QDir(root).relativeFilePath(it.next()));
    }
    files.sort(Qt::CaseSensitive);
    return files;
}

QStringList relativeFiles(const QString& root)
{
    QStringList files;
    for (const QString& relative : allFiles(root)) {
        if (!ignoredRelativePath(relative)) files << relative;
    }
    return files;
}

QByteArray sha256File(const QString& path)
{
    VNH::IntegrationProgress::inspectedEntry();
    constexpr int kRetries = 25;
    for (int attempt = 0; attempt <= kRetries; ++attempt) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (VNH::IntegrationProgress::addFileData(file, hash)) return hash.result();
        }
        if (!QFileInfo::exists(path)) return {};
        QThread::msleep(20);
    }
    return {};
}

bool copyFile(const QString& source, const QString& target, QString* error)
{
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read %1: %2").arg(source, input.errorString());
        return false;
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        *error = QStringLiteral("Cannot create directory for %1").arg(target);
        return false;
    }
    QSaveFile output(target);
    if (!output.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Cannot write %1: %2").arg(target, output.errorString());
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!input.atEnd()) {
        VNH::IntegrationProgress::checkpoint();
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count < 0 || output.write(buffer.constData(), count) != count) {
            *error = QStringLiteral("Copy failed from %1 to %2").arg(source, target);
            output.cancelWriting();
            return false;
        }
    }
    if (!output.commit()) {
        *error = QStringLiteral("Cannot commit %1: %2").arg(target, output.errorString());
        return false;
    }
    QFile::setPermissions(target, QFile::permissions(source));
    return true;
}

bool copyTree(const QString& source, const QString& target, const QStringList& files, QString* error)
{
    for (const QString& relative : files) {
        if (!copyFile(QDir(source).filePath(relative), QDir(target).filePath(relative), error)) return false;
    }
    return true;
}

QStringList mismatchedFiles(const QString& source, const QString& target, const QStringList& files)
{
    QStringList mismatches;
    for (const QString& relative : files) {
        const QByteArray expected = sha256File(QDir(source).filePath(relative));
        if (expected.isEmpty() || expected != sha256File(QDir(target).filePath(relative))) mismatches << relative;
    }
    return mismatches;
}

bool removeTree(const QString& path, QString* error)
{
    if (!QFileInfo::exists(path) || QDir(path).removeRecursively()) return true;
    *error = QStringLiteral("Cannot remove %1. Close Unity and any program using it, then retry.").arg(path);
    return false;
}

QJsonObject readJsonObject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QString packageVersion(const QString& packageRoot)
{
    return readJsonObject(QDir(packageRoot).filePath(QStringLiteral("package.json")))
        .value(QStringLiteral("version")).toString();
}

QString projectIdentity(const QString& project)
{
    QString identity = QFileInfo(project).canonicalFilePath();
    if (identity.isEmpty()) identity = QFileInfo(project).absoluteFilePath();
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    return identity;
}

QString hubProjectDirectory(const QString& area, const QString& project, QString* error)
{
    const QString userRoot = VNH::userDataDir();
    if (userRoot.isEmpty() || !QDir().mkpath(userRoot)) {
        *error = QCoreApplication::translate("HarkenHub", "Cannot create the Hub application-data directory: %1").arg(userRoot);
        return {};
    }
    std::error_code resolutionError;
    const auto resolvedRoot = std::filesystem::canonical(std::filesystem::path(userRoot.toStdU16String()), resolutionError);
    if (resolutionError) {
        *error = QCoreApplication::translate("HarkenHub", "Cannot resolve the Hub application-data directory: %1").arg(userRoot);
        return {};
    }
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        projectIdentity(project).toUtf8(), QCryptographicHash::Sha256).toHex());
    return QDir(QString::fromStdU16String(resolvedRoot.generic_u16string())).filePath(area + QLatin1Char('/') + key);
}

QString backupDirectory(const QString& project, QString* error)
{
    return hubProjectDirectory(QStringLiteral("UnityIntegration/Backups"), project, error);
}

QStringList backupFiles(const QString& backupRoot)
{
    const QJsonObject record = readJsonObject(QDir(backupRoot).filePath(QLatin1String(kBackupRecord)));
    if (!UnityInstallation::safeRelativePath(record.value(QStringLiteral("install_path")).toString())
        || !record.value(QStringLiteral("files")).isArray()) return {};
    const QString packageCopy = QDir(backupRoot).filePath(QLatin1String(kBackupPackage));
    QStringList files;
    for (const QJsonValue& value : record.value(QStringLiteral("files")).toArray()) {
        const QString relative = value.toString();
        const QString path = QDir::cleanPath(QDir(packageCopy).filePath(relative));
        if (relative.isEmpty() || QDir::isAbsolutePath(relative) || UnityInstallation::samePath(path, packageCopy)
            || !UnityInstallation::containsPath(packageCopy, path) || !QFileInfo(path).isFile())
            return {};
        files << relative;
    }
    return files.isEmpty() ? QStringList() : files;
}

bool backupReady(const QString& backupRoot)
{
    return !backupFiles(backupRoot).isEmpty();
}

bool removeBackup(const QString& backupRoot, QString* error)
{
    const QString record = QDir(backupRoot).filePath(QLatin1String(kBackupRecord));
    if (QFileInfo::exists(record) && !QFile::remove(record)) {
        *error = QStringLiteral("Cannot remove %1. Close Unity and any program using it, then retry.").arg(record);
        return false;
    }
    return removeTree(backupRoot, error);
}

bool discardIncompleteBackup(const QString& backupRoot, QString* error)
{
    return backupReady(backupRoot) || removeBackup(backupRoot, error);
}

bool lockProject(const QString& project, std::unique_ptr<QLockFile>* lock, QString* error)
{
    const QString directory = hubProjectDirectory(QStringLiteral("UnityIntegration/Locks"), project, error);
    if (directory.isEmpty()) return false;
    if (!QDir().mkpath(directory)) {
        *error = QStringLiteral("Cannot create directory: %1").arg(directory);
        return false;
    }
    *lock = std::make_unique<QLockFile>(QDir(directory).filePath(QStringLiteral("integration.lock")));
    (*lock)->setStaleLockTime(0);
    if (!(*lock)->tryLock()) {
        *error = QStringLiteral("Another integration operation is using this Unity project.");
        return false;
    }
    return true;
}

bool createBackup(const QString& project, const QString& target, const QString& backupRoot, QString* error)
{
    if (backupReady(backupRoot)) return true;
    if (!removeBackup(backupRoot, error)) return false;
    const QString packageCopy = QDir(backupRoot).filePath(QLatin1String(kBackupPackage));
    const QStringList files = allFiles(target);
    if (!QDir().mkpath(packageCopy) || !copyTree(target, packageCopy, files, error)) {
        if (error->isEmpty()) *error = QStringLiteral("Cannot create directory: %1").arg(packageCopy);
        QString cleanup;
        removeTree(backupRoot, &cleanup);
        return false;
    }
    const QStringList mismatches = mismatchedFiles(target, packageCopy, files);
    const QString folderMeta = target + QStringLiteral(".meta");
    if (!mismatches.isEmpty()
        || (QFileInfo::exists(folderMeta)
            && !copyFile(folderMeta, QDir(backupRoot).filePath(QLatin1String(kBackupFolderMeta)), error))) {
        if (error->isEmpty()) *error = QStringLiteral("Backup verification failed for %1").arg(mismatches.join(QStringLiteral(", ")));
        QString cleanup;
        removeTree(backupRoot, &cleanup);
        return false;
    }
    const QJsonObject record{
        {QStringLiteral("project"), projectIdentity(project)},
        {QStringLiteral("install_path"), QDir(project).relativeFilePath(target)},
        {QStringLiteral("version"), packageVersion(target)},
        {QStringLiteral("files"), QJsonArray::fromStringList(files)},
        {QStringLiteral("created_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    if (VNH::writeIntegrationFile(QDir(backupRoot).filePath(QLatin1String(kBackupRecord)),
                                  QJsonDocument(record).toJson(QJsonDocument::Indented), error)
        && (files.isEmpty() || backupReady(backupRoot)))
        return true;
    if (error->isEmpty()) *error = QStringLiteral("Backup verification failed for %1").arg(backupRoot);
    QString cleanup;
    removeBackup(backupRoot, &cleanup);
    return false;
}

class StepRunner
{
public:
    StepRunner(Service::Result& result, std::function<void(int)> publish, std::initializer_list<StepSpec> specs)
        : result_(result), publish_(std::move(publish))
    {
        for (const StepSpec& spec : specs) {
            StepResult step;
            step.id = QString::fromLatin1(spec.id);
            step.title = QString::fromLatin1(spec.title);
            result_.steps.push_back(step);
        }
    }

    bool blocked() const { return blocked_; }

    void run(const std::function<void(StepResult&)>& body, bool runWhenBlocked = false)
    {
        StepResult& step = result_.steps[next_];
        if (blocked_ && !runWhenBlocked) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Not run because %1 failed.").arg(blockingTitle_);
            step.cause = QStringLiteral("A required earlier step did not complete.");
            step.remediation = QStringLiteral("Fix the first failed step, then run integration again.");
            publish_(next_++);
            return;
        }
        QElapsedTimer timer;
        timer.start();
        step.status = StepStatus::Running;
        publish_(next_);
        body(step);
        step.elapsedMs = timer.elapsed();
        if (step.status == StepStatus::Running) step.status = StepStatus::Succeeded;
        if (step.status == StepStatus::Failed && !blocked_) {
            blocked_ = true;
            blockingTitle_ = step.title;
        }
        publish_(next_++);
    }

    void finish()
    {
        result_.success = true;
        for (const StepResult& step : result_.steps) {
            result_.success &= step.status != StepStatus::Failed;
            result_.hasWarnings |= step.status == StepStatus::Warning;
        }
    }

private:
    Service::Result& result_;
    std::function<void(int)> publish_;
    int next_ = 0;
    bool blocked_ = false;
    QString blockingTitle_;
};

bool validateProject(const QString& requested, const QString& root, StepResult& step)
{
    if (requested.trimmed().isEmpty() || !QDir(root).exists()) {
        step.status = StepStatus::Failed;
        step.summary = QStringLiteral("Unity project directory does not exist.");
        step.cause = requested.trimmed().isEmpty()
            ? QStringLiteral("No Unity project path was selected.")
            : QStringLiteral("The selected path cannot be found: %1").arg(root);
        step.remediation = QStringLiteral("Select the Unity project root containing Assets, Packages, and ProjectSettings.");
        return false;
    }
    QStringList missing;
    for (const QString& marker : {QStringLiteral("Assets"), QStringLiteral("Packages"), QStringLiteral("ProjectSettings")}) {
        if (!QDir(QDir(root).filePath(marker)).exists()) missing << marker;
    }
    if (!missing.isEmpty()) {
        step.status = StepStatus::Failed;
        step.summary = QStringLiteral("Selected directory is not a Unity project root.");
        step.cause = QStringLiteral("Missing required directories: %1").arg(missing.join(QStringLiteral(", ")));
        step.remediation = QStringLiteral("Select the parent directory that directly contains Assets, Packages, and ProjectSettings.");
        return false;
    }
    return true;
}

void writeReport(Service::Result& result, const QString& kind, bool checkOnly, StepResult& step)
{
    if (result.unityProjectPath.isEmpty()) {
        step.status = StepStatus::Skipped;
        step.summary = QStringLiteral("No report was written because the Unity project was not valid.");
        return;
    }
    const QString reportDir = hubProjectDirectory(QStringLiteral("IntegrationReports/Unity"), result.unityProjectPath, &step.cause);
    if (reportDir.isEmpty()) {
        step.status = StepStatus::Failed;
        step.summary = QStringLiteral("Could not prepare the Hub report directory.");
        return;
    }
    const QString jsonPath = QDir(reportDir).filePath(QStringLiteral("latest.json"));
    const QString textPath = QDir(reportDir).filePath(QStringLiteral("latest.txt"));
    bool success = true;
    QJsonArray steps;
    QString text = QStringLiteral("Harken Unity Integration Package %1 report\n\nGenerated: %2\nUnity project: %3\nInstallation location: %4\nInstalled version: %5\nSelected version: %6\n\n")
        .arg(kind, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), result.unityProjectPath,
             result.integrationInstallPath, result.installedVersion, result.sourceVersion);
    for (const StepResult& item : result.steps) {
        if (&item == &step) break;
        success &= item.status != StepStatus::Failed;
        QJsonObject object;
        object[QStringLiteral("id")] = item.id;
        object[QStringLiteral("title")] = item.title;
        object[QStringLiteral("status")] = Service::statusName(item.status);
        object[QStringLiteral("summary")] = item.summary;
        object[QStringLiteral("cause")] = item.cause;
        object[QStringLiteral("remediation")] = item.remediation;
        object[QStringLiteral("details")] = QJsonArray::fromStringList(item.details);
        object[QStringLiteral("artifacts")] = QJsonArray::fromStringList(item.artifacts);
        steps.append(object);
        text += QStringLiteral("%1 — %2\n\n%3\n\n").arg(item.title, Service::statusName(item.status), item.summary);
        if (!item.cause.isEmpty()) text += QStringLiteral("Cause: %1\n\n").arg(item.cause);
        if (!item.remediation.isEmpty()) text += QStringLiteral("How to fix: %1\n\n").arg(item.remediation);
        if (!item.details.isEmpty()) text += QStringLiteral("Details:\n%1\n\n").arg(item.details.join(QLatin1Char('\n')));
    }
    const QJsonObject root{
        {QStringLiteral("schema_version"), 3},
        {QStringLiteral("operation"), kind},
        {QStringLiteral("generated_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("success"), success},
        {QStringLiteral("check_only"), checkOnly},
        {QStringLiteral("unity_project_path"), result.unityProjectPath},
        {QStringLiteral("integration_source_path"), result.integrationSourcePath},
        {QStringLiteral("integration_install_path"), result.integrationInstallPath},
        {QStringLiteral("installed_version"), result.installedVersion},
        {QStringLiteral("source_version"), result.sourceVersion},
        {QStringLiteral("previous_version_available"), result.previousVersionAvailable},
        {QStringLiteral("steps"), steps}};
    QString error;
    if (!VNH::writeIntegrationFile(jsonPath, QJsonDocument(root).toJson(QJsonDocument::Indented), &error)
        || !VNH::writeIntegrationFile(textPath, text.toUtf8(), &error)) {
        step.status = StepStatus::Failed;
        step.summary = QStringLiteral("The integration report could not be written.");
        step.cause = error;
        step.remediation = QStringLiteral("Check write permission for the Hub report directory: %1").arg(reportDir);
        return;
    }
    step.summary = QStringLiteral("Wrote machine-readable and human-readable Integration Package reports.");
    result.jsonReportPath = jsonPath;
    result.textReportPath = textPath;
    step.artifacts << jsonPath << textPath;
}

} // namespace

UnityIntegrationService::UnityIntegrationService(QObject* parent)
    : QObject(parent)
{
}

QString UnityIntegrationService::statusName(StepStatus status)
{
    switch (status) {
    case StepStatus::Pending:   return QStringLiteral("pending");
    case StepStatus::Running:   return QStringLiteral("running");
    case StepStatus::Succeeded: return QStringLiteral("succeeded");
    case StepStatus::Warning:   return QStringLiteral("warning");
    case StepStatus::Failed:    return QStringLiteral("failed");
    case StepStatus::Skipped:   return QStringLiteral("skipped");
    }
    return QStringLiteral("unknown");
}

QString UnityIntegrationService::locateIntegrationSource(const QString& preferredPath)
{
    const auto validPackageRoot = [](const QString& candidate) {
        const QString root = cleanAbsolutePath(candidate);
        if (root.isEmpty()) return QString{};
        const QJsonObject package = readJsonObject(QDir(root).filePath(QStringLiteral("package.json")));
        return package.value(QStringLiteral("name")).toString() == QLatin1String(kPackageName) ? root : QString{};
    };
    const auto normalizeCandidate = [&](const QString& candidate) {
        const QString root = cleanAbsolutePath(candidate);
        if (root.isEmpty()) return QString{};
        QStringList packageCandidates;
        packageCandidates << root;
        const QString baseName = QFileInfo(root).fileName();
        if (baseName.compare(QStringLiteral("HarkenIntegration"), Qt::CaseInsensitive) == 0
            || baseName.compare(QStringLiteral("HarkenSDK"), Qt::CaseInsensitive) == 0)
            packageCandidates << QDir(root).filePath(QStringLiteral("Unity/Harken"));
        if (baseName.compare(QStringLiteral("Unity"), Qt::CaseInsensitive) == 0)
            packageCandidates << QDir(root).filePath(QStringLiteral("Harken"));
        packageCandidates << QDir(root).filePath(QStringLiteral("Unity/Harken"));
        packageCandidates << QDir(root).filePath(QStringLiteral("Harken"));
        for (const QString& packageRoot : packageCandidates) {
            const QString valid = validPackageRoot(packageRoot);
            if (!valid.isEmpty()) return valid;
        }
        return QString{};
    };

    QStringList candidates;
    if (!preferredPath.trimmed().isEmpty()) {
        candidates << preferredPath;
    } else {
        const QByteArray fromEnv = qgetenv("HARKEN_UNITY_INTEGRATION_PATH");
        if (!fromEnv.isEmpty()) candidates << QString::fromLocal8Bit(fromEnv);
    }

    for (const QString& candidate : candidates) {
        const QString packageRoot = normalizeCandidate(candidate);
        if (!packageRoot.isEmpty()) return packageRoot;
    }
    return {};
}

QString UnityIntegrationService::previousVersionBackup(const QString& unityProjectPath)
{
    if (unityProjectPath.trimmed().isEmpty()) return {};
    QString error;
    const QString backupRoot = backupDirectory(cleanAbsolutePath(unityProjectPath), &error);
    return !backupRoot.isEmpty() && backupReady(backupRoot) ? backupRoot : QString();
}

QJsonObject UnityIntegrationService::stepToJson(const StepResult& step)
{
    QJsonObject object;
    object[QStringLiteral("id")] = step.id;
    object[QStringLiteral("title")] = step.title;
    object[QStringLiteral("status")] = statusName(step.status);
    object[QStringLiteral("summary")] = step.summary;
    object[QStringLiteral("cause")] = step.cause;
    object[QStringLiteral("remediation")] = step.remediation;
    object[QStringLiteral("elapsed_ms")] = step.elapsedMs;
    object[QStringLiteral("details")] = QJsonArray::fromStringList(step.details);
    object[QStringLiteral("artifacts")] = QJsonArray::fromStringList(step.artifacts);
    return object;
}

UnityIntegrationService::Result UnityIntegrationService::run(const Request& request)
{
    if (request.restorePrevious) return restorePrevious(request);
    if (request.removeIntegration) return remove(request);
    return install(request);
}

UnityIntegrationService::Result UnityIntegrationService::install(const Request& request)
{
    Result result;
    result.unityProjectPath = cleanAbsolutePath(request.unityProjectPath);
    StepRunner runner(result, [&](int index) { emit stepUpdated(index, result.steps[index]); },
        {{"unity_preflight", "Validate Unity project"},
         {"sdk_preflight", "Validate Harken Unity Integration Package source"},
         {"compare_sdk", "Compare installed Unity Integration Package"},
         {"install_sdk", "Install or update Unity Integration Package"},
         {"verify", "Verify installed Integration Package"},
         {"report", "Write integration report"}});
    QString target;
    QString backupRoot;
    QStringList sourceFiles;
    std::unique_ptr<QLockFile> lock;

    runner.run([&](StepResult& step) {
        if (!validateProject(request.unityProjectPath, result.unityProjectPath, step)) return;
        const QString relative = request.integrationInstallPath.trimmed().isEmpty()
            ? UnityInstallation::defaultRelativePath() : request.integrationInstallPath;
        QString error;
        if (!UnityInstallation::resolveTarget(result.unityProjectPath, relative, &target, &error)
            || !UnityInstallation::validatePath(target + QStringLiteral(".meta"), &error)
            || !lockProject(result.unityProjectPath, &lock, &error)
            || (backupRoot = backupDirectory(result.unityProjectPath, &error)).isEmpty()
            || !discardIncompleteBackup(backupRoot, &error)
            || !UnityInstallation::validateTarget(result.unityProjectPath, target, &error)
            || !checkUnityProjectClosed(result.unityProjectPath, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package installation location is not safe to use.");
            step.cause = error;
            step.remediation = QStringLiteral("Resolve the reported path or Integration Package conflict, then check again.");
            return;
        }
        result.integrationInstallPath = QDir(result.unityProjectPath).relativeFilePath(target);
        result.previousVersionAvailable = backupReady(backupRoot);
        if (result.previousVersionAvailable) result.backupPath = backupRoot;
        step.summary = QStringLiteral("Unity project layout and write access are valid.");
        step.artifacts << result.unityProjectPath;
    });

    runner.run([&](StepResult& step) {
        result.integrationSourcePath = locateIntegrationSource(request.integrationSourcePath);
        if (result.integrationSourcePath.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Harken Unity Integration Package could not be located.");
            if (!request.integrationSourcePath.trimmed().isEmpty()) {
                step.cause = QStringLiteral("The selected folder is not a valid com.harken.core Integration Package source: %1")
                                 .arg(cleanAbsolutePath(request.integrationSourcePath));
                step.remediation = QStringLiteral("Choose HarkenIntegration, HarkenIntegration/Unity/Harken, or a complete external Unity package root, or clear the Integration Package source field to use automatic discovery.");
            } else {
                step.cause = QStringLiteral("No Integration Package source was selected, and HARKEN_UNITY_INTEGRATION_PATH does not point to a valid com.harken.core package.");
                step.remediation = QStringLiteral("Use the Integration Package source field to choose or drop HarkenIntegration, Unity/Harken, or a complete external Unity package root.");
            }
            return;
        }
        QString error;
        if (!UnityInstallation::validateTree(result.integrationSourcePath, &error)
            || UnityInstallation::containsPath(result.integrationSourcePath, target)
            || UnityInstallation::containsPath(target, result.integrationSourcePath)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package source and installation location are not safe to use.");
            step.cause = error.isEmpty()
                ? QStringLiteral("Integration Package source and target must be separate directories without containment.") : error;
            return;
        }
        sourceFiles = relativeFiles(result.integrationSourcePath);
        if (sourceFiles.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The selected Unity Integration Package source is empty. The Unity project was not changed.");
            step.remediation = QStringLiteral("Choose or drop a complete Unity/Harken package, then run Check only again.");
            return;
        }
        result.sourceVersion = packageVersion(result.integrationSourcePath);
        step.details << QStringLiteral("Hub checks package files. Unity compilation, playback and target-platform support must be tested in Unity.");
        step.summary = QStringLiteral("Validated Integration Package %1: %2 file(s).")
                           .arg(result.sourceVersion, QString::number(sourceFiles.size()));
        step.artifacts << result.integrationSourcePath;
    });

    runner.run([&](StepResult& step) {
        result.targetInstalled = QFileInfo::exists(QDir(target).filePath(QStringLiteral("package.json")));
        if (!result.targetInstalled) {
            step.summary = QStringLiteral("No Harken Integration Package is installed; version %1 will be installed at %2.")
                               .arg(result.sourceVersion, result.integrationInstallPath);
            return;
        }
        result.installedVersion = packageVersion(target);
        result.targetCurrent = result.installedVersion == result.sourceVersion;
        step.summary = result.targetCurrent
            ? QStringLiteral("Version %1 is already installed at %2; installing again replaces the whole directory with the selected package.")
                  .arg(result.installedVersion, result.integrationInstallPath)
            : QStringLiteral("Installed version %1 at %2 will be replaced by version %3.")
                  .arg(result.installedVersion, result.integrationInstallPath, result.sourceVersion);
        step.details << QStringLiteral("The whole installation directory is replaced. Files you changed or added inside it are not kept; use version control to keep local changes.");
    });

    runner.run([&](StepResult& step) {
        if (request.checkOnly) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Check only: the Unity project was not changed.");
            return;
        }
        QString error;
        if (!allFiles(target).isEmpty() && !createBackup(result.unityProjectPath, target, backupRoot, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The installed Integration Package could not be backed up. The Unity project was not changed.");
            step.cause = error;
            return;
        }
        result.previousVersionAvailable = backupReady(backupRoot);
        if (result.previousVersionAvailable) result.backupPath = backupRoot;
        if (!removeTree(target, &error) || !QDir().mkpath(target)
            || !copyTree(result.integrationSourcePath, target, sourceFiles, &error)) {
            QString cleanup;
            removeTree(target, &cleanup);
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package could not be installed.");
            step.cause = error.isEmpty() ? QStringLiteral("Cannot create directory: %1").arg(target) : error;
            step.remediation = result.previousVersionAvailable
                ? QStringLiteral("Retry the installation, or restore the previous version.")
                : QStringLiteral("Close Unity and any program using the installation directory, then retry.");
            return;
        }
        result.integrationFilesCopied = sourceFiles.size();
        step.summary = QStringLiteral("Installed Integration Package %1 at %2: %3 file(s).")
                           .arg(result.sourceVersion, result.integrationInstallPath)
                           .arg(sourceFiles.size());
        step.artifacts << target;
    });

    runner.run([&](StepResult& step) {
        if (request.checkOnly) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Check only: nothing was installed to verify.");
            return;
        }
        const QStringList mismatches = mismatchedFiles(result.integrationSourcePath, target, sourceFiles);
        if (!mismatches.isEmpty()) {
            QString cleanup;
            removeTree(target, &cleanup);
            result.integrationFilesCopied = 0;
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Installed Integration Package files do not match the selected package.");
            step.details = mismatches;
            step.remediation = result.previousVersionAvailable
                ? QStringLiteral("Retry the installation, or restore the previous version.")
                : QStringLiteral("Close Unity and any program using the installation directory, then retry.");
            return;
        }
        result.targetCurrent = true;
        result.installedVersion = result.sourceVersion;
        step.summary = QStringLiteral("Verified %1 installed Integration Package file(s).").arg(sourceFiles.size());
        step.artifacts << target;
        QString error;
        if (result.previousVersionAvailable && !removeBackup(backupRoot, &error)) {
            step.status = StepStatus::Warning;
            step.details << QStringLiteral("The installation succeeded, but the backup of the previous version could not be removed.");
            step.artifacts << backupRoot;
            return;
        }
        result.previousVersionAvailable = false;
        result.backupPath.clear();
    });

    runner.run([&](StepResult& step) {
        writeReport(result, QStringLiteral("installation"), request.checkOnly, step);
    }, true);
    runner.finish();
    return result;
}

UnityIntegrationService::Result UnityIntegrationService::remove(const Request& request)
{
    Result result;
    result.unityProjectPath = cleanAbsolutePath(request.unityProjectPath);
    StepRunner runner(result, [&](int index) { emit stepUpdated(index, result.steps[index]); },
        {{"unity_preflight", "Validate Unity project"},
         {"inspect_installation", "Review installed Harken Integration Package"},
         {"remove_integration", "Remove Harken Integration Package"},
         {"removal_report", "Write removal report"}});
    QString target;
    QString backupRoot;
    std::unique_ptr<QLockFile> lock;

    runner.run([&](StepResult& step) {
        if (!validateProject(request.unityProjectPath, result.unityProjectPath, step)) return;
        QString error;
        if (!lockProject(result.unityProjectPath, &lock, &error)
            || (backupRoot = backupDirectory(result.unityProjectPath, &error)).isEmpty()
            || !discardIncompleteBackup(backupRoot, &error)
            || !checkUnityProjectClosed(result.unityProjectPath, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Unity project cannot be changed safely.");
            step.cause = error;
            return;
        }
        step.summary = QStringLiteral("Unity project layout and write access are valid.");
        step.artifacts << result.unityProjectPath;
    });

    runner.run([&](StepResult& step) {
        const auto discovery = UnityInstallation::discover(result.unityProjectPath);
        if (discovery.installations.size() != 1) {
            step.status = StepStatus::Failed;
            step.summary = discovery.installations.isEmpty()
                ? QStringLiteral("No Harken Unity Integration Package is installed in this project.")
                : QStringLiteral("Multiple Integration Package installations were found:");
            for (const auto& installation : discovery.installations) step.details << installation.path;
            return;
        }
        target = discovery.installations.front().path;
        QString error;
        if (!request.integrationInstallPath.trimmed().isEmpty()) {
            QString requested;
            if (!UnityInstallation::resolveTarget(result.unityProjectPath, request.integrationInstallPath, &requested, &error)
                || !UnityInstallation::samePath(requested, target)) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("The selected location does not contain the installed Integration Package.");
                step.cause = error.isEmpty() ? target : error;
                return;
            }
        }
        if (!UnityInstallation::validateTarget(result.unityProjectPath, target, &error, true)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The installed Integration Package cannot be removed safely.");
            step.cause = error;
            return;
        }
        result.targetInstalled = true;
        result.integrationInstallPath = QDir(result.unityProjectPath).relativeFilePath(target);
        result.installedVersion = packageVersion(target);
        step.summary = QStringLiteral("Integration Package %1 is installed at %2 and will be removed.")
                           .arg(result.installedVersion, result.integrationInstallPath);
        step.details << QStringLiteral("Project settings created by the Integration Package are outside the installation directory and are kept.");
        step.artifacts << target;
    });

    runner.run([&](StepResult& step) {
        if (request.checkOnly) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Check only: the Unity project was not changed.");
            return;
        }
        QString error;
        if (!createBackup(result.unityProjectPath, target, backupRoot, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The installed Integration Package could not be backed up. The Unity project was not changed.");
            step.cause = error;
            return;
        }
        result.previousVersionAvailable = true;
        result.backupPath = backupRoot;
        const QString folderMeta = target + QStringLiteral(".meta");
        if (!removeTree(target, &error) || (QFileInfo::exists(folderMeta) && !QFile::remove(folderMeta))) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package could not be removed completely.");
            step.cause = error.isEmpty() ? folderMeta : error;
            step.remediation = QStringLiteral("Close Unity and any program using the installation directory, then retry, or restore the previous version.");
            return;
        }
        result.integrationRemoved = true;
        step.summary = QStringLiteral("Removed Integration Package %1 from %2.")
                           .arg(result.installedVersion, result.integrationInstallPath);
        if (!removeBackup(backupRoot, &error)) {
            step.status = StepStatus::Warning;
            step.details << QStringLiteral("The removal succeeded, but the backup of the removed version could not be deleted.");
            step.artifacts << backupRoot;
            return;
        }
        result.previousVersionAvailable = false;
        result.backupPath.clear();
    });

    runner.run([&](StepResult& step) {
        writeReport(result, QStringLiteral("removal"), request.checkOnly, step);
    }, true);
    runner.finish();
    return result;
}

UnityIntegrationService::Result UnityIntegrationService::restorePrevious(const Request& request)
{
    Result result;
    result.unityProjectPath = cleanAbsolutePath(request.unityProjectPath);
    StepRunner runner(result, [&](int index) { emit stepUpdated(index, result.steps[index]); },
        {{"unity_preflight", "Validate Unity project"},
         {"restore_previous", "Restore previous Integration Package"},
         {"report", "Write integration report"}});
    QString backupRoot;
    std::unique_ptr<QLockFile> lock;

    runner.run([&](StepResult& step) {
        if (!validateProject(request.unityProjectPath, result.unityProjectPath, step)) return;
        QString error;
        if (!lockProject(result.unityProjectPath, &lock, &error)
            || (backupRoot = backupDirectory(result.unityProjectPath, &error)).isEmpty()
            || !checkUnityProjectClosed(result.unityProjectPath, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Unity project cannot be changed safely.");
            step.cause = error;
            return;
        }
        step.summary = QStringLiteral("Unity project layout and write access are valid.");
        step.artifacts << result.unityProjectPath;
    });

    runner.run([&](StepResult& step) {
        if (!backupReady(backupRoot)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("No previous Integration Package version is available for this project.");
            return;
        }
        const QJsonObject record = readJsonObject(QDir(backupRoot).filePath(QLatin1String(kBackupRecord)));
        const QString packageCopy = QDir(backupRoot).filePath(QLatin1String(kBackupPackage));
        result.previousVersionAvailable = true;
        result.backupPath = backupRoot;
        result.installedVersion = record.value(QStringLiteral("version")).toString();
        result.integrationInstallPath = record.value(QStringLiteral("install_path")).toString();
        QString target;
        QString error;
        if (!UnityInstallation::resolveTarget(result.unityProjectPath, result.integrationInstallPath, &target, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The previous installation location is not safe to use.");
            step.cause = error;
            return;
        }
        if (request.checkOnly) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Version %1 can be restored at %2.")
                               .arg(result.installedVersion, result.integrationInstallPath);
            return;
        }
        const QStringList files = backupFiles(backupRoot);
        if (!removeTree(target, &error) || !QDir().mkpath(target) || !copyTree(packageCopy, target, files, &error)
            || !mismatchedFiles(packageCopy, target, files).isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The previous Integration Package version could not be restored.");
            step.cause = error;
            step.remediation = QStringLiteral("Close Unity and any program using the installation directory, then retry.");
            return;
        }
        const QString savedMeta = QDir(backupRoot).filePath(QLatin1String(kBackupFolderMeta));
        const QString folderMeta = target + QStringLiteral(".meta");
        if (QFileInfo::exists(savedMeta) && !QFileInfo::exists(folderMeta) && !copyFile(savedMeta, folderMeta, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The previous Integration Package version could not be restored.");
            step.cause = error;
            return;
        }
        result.previousRestored = true;
        result.targetInstalled = true;
        step.summary = QStringLiteral("Restored Integration Package %1 at %2.")
                           .arg(result.installedVersion, result.integrationInstallPath);
        step.artifacts << target;
        if (!removeBackup(backupRoot, &error)) {
            step.status = StepStatus::Warning;
            step.details << QStringLiteral("The previous version was restored, but its backup could not be deleted.");
            step.artifacts << backupRoot;
            return;
        }
        result.previousVersionAvailable = false;
        result.backupPath.clear();
    });

    runner.run([&](StepResult& step) {
        writeReport(result, QStringLiteral("restoration"), request.checkOnly, step);
    }, true);
    runner.finish();
    return result;
}

} // namespace VNX
