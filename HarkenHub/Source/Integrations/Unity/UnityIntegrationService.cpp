#include "Integrations/IntegrationFileSystem.h"
#include "Integrations/Unity/UnityIntegrationService.h"
#include "Integrations/IntegrationProgress.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Integrations/Unity/UnityProjectAccess.h"
#include "Platform/PlatformPaths.h"
#include "Packaging/PackageInventory.h"

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
#include <QMap>
#include <QLockFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <filesystem>
#include <memory>

namespace VNX {
namespace {

QString normalizePath(QString path)
{
    path.replace('\\', '/');
    while (path.contains(QStringLiteral("//")))
        path.replace(QStringLiteral("//"), QStringLiteral("/"));
    if (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

bool isSafeRelativePath(const QString& relativePath)
{
    return UnityInstallation::safeRelativePath(relativePath);
}

constexpr auto kPackageName = "com.harken.core";
constexpr auto kIntegrationManifest = "harken-integration.json";
constexpr auto kInstalledState = ".harken-integration-state.json";
constexpr int kIntegrationStateVersion = 1;

struct StepSpec
{
    const char* id;
    const char* title;
};

enum class ReadStatus
{
    Ok,
    Missing,
    Invalid,
    Unreadable,
};

struct FileDigest
{
    ReadStatus status = ReadStatus::Missing;
    QByteArray hash;

    bool unreadable() const { return status == ReadStatus::Unreadable; }
    bool matches(const QByteArray& expected) const
    {
        return status == ReadStatus::Ok && hash == expected;
    }
};

struct JsonRead
{
    ReadStatus status = ReadStatus::Missing;
    QJsonObject object;

    bool unreadable() const { return status == ReadStatus::Unreadable; }
};

constexpr StepSpec kStepSpecs[] = {
    {"unity_preflight", "Validate Unity project"},
    {"sdk_preflight", "Validate Harken Unity Integration Package source"},
    {"compare_sdk", "Compare installed Unity Integration Package"},
    {"install_sdk", "Install or update Unity Integration Package"},
    {"verify", "Verify installed Integration Package"},
    {"report", "Write integration report"},
};

constexpr StepSpec kRemovalStepSpecs[] = {
    {"unity_preflight", "Validate Unity project"},
    {"inspect_installation", "Review installed Harken Integration Package"},
    {"remove_integration", "Back up and remove Harken Integration Package"},
    {"removal_report", "Write removal report"},
};

QString cleanAbsolutePath(const QString& path)
{
    if (path.trimmed().isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString safePathSegment(const QString& value)
{
    QString safe;
    safe.reserve(value.size());
    for (const QChar character : value) {
        safe += character.isLetterOrNumber() || character == QLatin1Char('-') ||
                        character == QLatin1Char('_')
                    ? character
                    : QLatin1Char('_');
    }
    return safe.isEmpty() ? QStringLiteral("UnityProject") : safe;
}

QString defaultIntegrationBackupPath(const QString& unityProjectPath)
{
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (appData.isEmpty()) return {};
    const QString timestamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz"));
    const QString projectName = safePathSegment(
        QFileInfo(unityProjectPath).fileName());
    QString path = QDir(appData).filePath(
        QStringLiteral("UnityIntegration/Backups/%1_%2")
            .arg(timestamp, projectName));
    if (QFileInfo::exists(path)) {
        path += QStringLiteral("_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    return QDir::cleanPath(path);
}

bool discardableOsMetadata(const QString& relative)
{
    const QString fileName = normalizePath(relative)
                                 .section(QLatin1Char('/'), -1);
    return fileName == QStringLiteral(".DS_Store") ||
           fileName.startsWith(QStringLiteral("._"));
}

bool ignoredRelativePath(const QString& relative)
{
    const QString normalized = normalizePath(relative);
    return discardableOsMetadata(normalized) ||
           normalized == QLatin1String(kInstalledState) ||
           normalized.startsWith(QStringLiteral(".git/")) ||
           normalized.startsWith(QStringLiteral(".cache/")) ||
           normalized.startsWith(QStringLiteral("Unreal/"));
}

QStringList allRelativeFiles(const QString& root)
{
    QStringList files;
    QDirIterator it(root,
                    QDir::Files | QDir::AllDirs | QDir::Hidden |
                        QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        VNH::IntegrationProgress::inspectedEntry();
        const QString absolute = it.next();
        if (!QFileInfo(absolute).isFile()) continue;
        const QString relative = normalizePath(
            QDir(root).relativeFilePath(absolute));
        files << relative;
    }
    files.sort(Qt::CaseSensitive);
    return files;
}

QStringList relativeFiles(const QString& root)
{
    QStringList files;
    for (const QString& relative : allRelativeFiles(root)) {
        if (!ignoredRelativePath(relative)) files << relative;
    }
    return files;
}

bool copyFileAtomically(const QString& sourcePath,
                        const QString& targetPath,
                        QString* error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot read %1: %2")
                                .arg(sourcePath, source.errorString());
        return false;
    }
    if (!QDir().mkpath(QFileInfo(targetPath).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create directory for %1")
                                .arg(targetPath);
        return false;
    }

    QSaveFile target(targetPath);
    if (!target.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Cannot write %1: %2")
                                .arg(targetPath, target.errorString());
        return false;
    }

    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!source.atEnd()) {
        const qint64 count = source.read(buffer.data(), buffer.size());
        if (count < 0 || target.write(buffer.constData(), count) != count) {
            if (error) *error = QStringLiteral("Copy failed from %1 to %2")
                                    .arg(sourcePath, targetPath);
            target.cancelWriting();
            return false;
        }
    }
    if (!target.commit()) {
        if (error) *error = QStringLiteral("Cannot commit %1: %2")
                                .arg(targetPath, target.errorString());
        return false;
    }
    return true;
}

bool copyFiles(const QString& sourceRoot,
               const QString& targetRoot,
               const QStringList& relativePaths,
               QString* error)
{
    for (const QString& relative : relativePaths) {
        if (!copyFileAtomically(QDir(sourceRoot).filePath(relative),
                                QDir(targetRoot).filePath(relative), error)) {
            return false;
        }
    }
    return true;
}

bool copyFilePermissions(const QString& sourceRoot, const QString& targetRoot,
                         const QStringList& relativePaths, QString* error)
{
    for (const auto& relative : relativePaths) {
        if (!QFile::setPermissions(QDir(targetRoot).filePath(relative),
                                  QFile::permissions(QDir(sourceRoot).filePath(relative)))) {
            *error = relative;
            return false;
        }
    }
    return true;
}

FileDigest sha256File(const QString& path);

bool verifyCopiedFiles(const QString& sourceRoot,
                       const QString& targetRoot,
                       const QStringList& relativePaths,
                       QString* error)
{
    for (const QString& relative : relativePaths) {
        const QString source = QDir(sourceRoot).filePath(relative);
        const QString target = QDir(targetRoot).filePath(relative);
        const FileDigest sourceDigest = sha256File(source);
        const FileDigest targetDigest = sha256File(target);
        if (sourceDigest.status != ReadStatus::Ok ||
            targetDigest.status == ReadStatus::Unreadable) {
            if (error) {
                *error = QStringLiteral("Cannot read %1 while verifying the backup copy of %2")
                             .arg(sourceDigest.status == ReadStatus::Ok ? target : source,
                                  relative);
            }
            return false;
        }
        if (!targetDigest.matches(sourceDigest.hash)) {
            if (error) {
                *error = QStringLiteral("Backup verification failed for %1")
                             .arg(relative);
            }
            return false;
        }
    }
    return true;
}

FileDigest sha256File(const QString& path)
{
    VNH::IntegrationProgress::inspectedEntry();
    const QFileInfo info(path);
    if (!info.exists()) return {ReadStatus::Missing, {}};
    if (info.isDir()) return {ReadStatus::Unreadable, {}};
    constexpr int kRetries = 25;
    constexpr unsigned long kRetryDelayMs = 20;
    for (int attempt = 0; attempt <= kRetries; ++attempt) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (VNH::IntegrationProgress::addFileData(file, hash))
                return {ReadStatus::Ok, hash.result().toHex()};
        }
        QThread::msleep(kRetryDelayMs);
    }
    return {ReadStatus::Unreadable, {}};
}

JsonRead readJsonObject(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists()) return {ReadStatus::Missing, {}};
    if (info.isDir()) return {ReadStatus::Unreadable, {}};
    constexpr int kRetries = 25;
    constexpr unsigned long kRetryDelayMs = 20;
    for (int attempt = 0; attempt <= kRetries; ++attempt) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray contents = file.readAll();
            if (file.error() == QFileDevice::NoError) {
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(contents, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject())
                    return {ReadStatus::Invalid, {}};
                return {ReadStatus::Ok, document.object()};
            }
        }
        QThread::msleep(kRetryDelayMs);
    }
    return {ReadStatus::Unreadable, {}};
}

bool existingArtifact(const QString& path)
{
    const QFileInfo info(path);
    if (info.isFile()) return info.size() > 0;
    if (!info.isDir()) return false;
    QDirIterator files(path, QDir::Files, QDirIterator::Subdirectories);
    return files.hasNext();
}

QString packageVersion(const QString& packageRoot)
{
    return readJsonObject(QDir(packageRoot).filePath(QStringLiteral("package.json")))
        .object.value(QStringLiteral("version")).toString();
}

QString requestedInstallPath(const UnityIntegrationService::Request& request)
{
    if (!request.integrationInstallPath.trimmed().isEmpty()) return request.integrationInstallPath;
    return request.installToAssets
        ? QStringLiteral("Assets/HarkenSDK")
        : UnityInstallation::defaultRelativePath();
}

bool lockUnityProject(const QString& root, std::unique_ptr<QLockFile>* lock,
                      QString* error)
{
    const QString operationDir = QDir(root).filePath(QStringLiteral("Library/HarkenIntegration"));
    if (!UnityInstallation::validatePath(operationDir, error)
        || !UnityInstallation::validatePath(QDir(operationDir).filePath(QStringLiteral("pending-migration.json")), error)) return false;
    if (!QDir().mkpath(operationDir)) {
        *error = QStringLiteral("Cannot create directory: %1").arg(operationDir);
        return false;
    }
    const QString lockPath = QDir(operationDir).filePath(QStringLiteral("integration.lock"));
    if (!UnityInstallation::validatePath(lockPath, error)) return false;
    *lock = std::make_unique<QLockFile>(lockPath);
    (*lock)->setStaleLockTime(0);
    if (!(*lock)->tryLock()) {
        *error = QStringLiteral("Another integration operation is using this Unity project.");
        return false;
    }
    return true;
}

QString integrationReportDirectory(const QString& project, QString* error)
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
    const QString canonicalRoot = QString::fromStdU16String(resolvedRoot.generic_u16string());
    QString identity = QFileInfo(project).canonicalFilePath();
    if (identity.isEmpty()) identity = QFileInfo(project).absoluteFilePath();
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex());
    return QDir(canonicalRoot).filePath(QStringLiteral("IntegrationReports/Unity/%1").arg(key));
}

bool writeIntegrationReport(const QString& path, const QByteArray& contents, QString* error)
{
    return UnityInstallation::validatePath(path, error)
        && VNH::writeIntegrationFile(path, contents, error);
}

bool prepareInstallation(const UnityIntegrationService::Request& request,
                         UnityIntegrationService::Result& result,
                         QString* target, std::unique_ptr<QLockFile>* lock,
                         QString* error)
{
    const QString root = result.unityProjectPath;
    QString installPath = requestedInstallPath(request);
    const bool pendingRemoval = request.removeIntegration && UnityInstallation::hasPendingRemoval(root);
    if (pendingRemoval) {
        if (!UnityInstallation::validatePath(UnityInstallation::removalJournalPath(root), error)) return false;
        const JsonRead journal = readJsonObject(UnityInstallation::removalJournalPath(root));
        if (journal.status != ReadStatus::Ok || journal.object.value(QStringLiteral("install_path")).toString().isEmpty()) {
            *error = QStringLiteral("The unfinished removal record is invalid or its backup is unavailable. No files were changed.");
            return false;
        }
        installPath = journal.object.value(QStringLiteral("install_path")).toString();
    }
    if (!UnityInstallation::resolveTarget(root, installPath, target, error))
        return false;
    result.integrationInstallPath = QDir(root).relativeFilePath(*target);
    if (!UnityInstallation::validatePath(*target + QStringLiteral(".meta"), error)
        || !lockUnityProject(root, lock, error)) return false;
    if (request.recoverRestore) {
        if (UnityInstallation::hasPendingRemoval(root)) {
            *error = QStringLiteral("An unfinished Integration Package removal must be completed before continuing. Use Uninstall Integration Package to retry.");
            return false;
        }
        if (UnityInstallation::hasPendingMigration(root)) {
            *error = QStringLiteral("An unfinished Integration Package migration must be recovered before continuing.");
            return false;
        }
        if (QFileInfo::exists(*target) && !UnityInstallation::validateTree(*target, error)) return false;
    } else if (!UnityInstallation::validateTarget(root, *target, error, request.removeIntegration,
                                           request.preserveUnownedTarget || pendingRemoval,
                                           request.restoreIntegration || request.recoverRestore)) return false;
    if ((!request.checkOnly || request.removeIntegration)
        && !checkUnityProjectClosed(root, error)) {
        return false;
    }
    return true;
}

QString hostPlatformName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macOS");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Linux");
#else
    return {};
#endif
}

QString runtimePurpose(const QString& platform)
{
    if (platform == QStringLiteral("Windows"))
        return QStringLiteral("Unity includes this runtime in Windows games and uses it for Windows Editor play mode.");
    if (platform == QStringLiteral("Linux"))
        return QStringLiteral("Unity includes this runtime in Linux games built from the project.");
    if (platform == QStringLiteral("Android"))
        return QStringLiteral("Unity packages this runtime into Android APK or AAB builds so Harken audio can run on Android devices.");
    if (platform == QStringLiteral("macOS"))
        return QStringLiteral("Unity includes this runtime in macOS games and uses it for macOS Editor play mode.");
    if (platform == QStringLiteral("iOS"))
        return QStringLiteral("Unity adds this static runtime to the generated Xcode project for the final iOS application.");
    return QStringLiteral("Unity includes this runtime when it builds games for this target platform.");
}

QString missingRuntimeDetail(const QString& integrationRoot, const QString& platform,
                             const QStringList& alternatives)
{
    QStringList locations;
    for (const QString& relative : alternatives)
        locations << QDir::toNativeSeparators(QDir(integrationRoot).filePath(relative));
    return QStringLiteral("Missing component: %1 game runtime\n"
                          "Expected inside the selected Integration Package source: %2\n"
                          "Purpose: %3\n"
                          "What this means: This Integration Package source cannot safely prepare the Unity project for %1 builds until a verified runtime is restored.")
        .arg(platform, locations.join(QStringLiteral(" or ")),
             runtimePurpose(platform));
}

struct FileExplanation
{
    QString key;
    QString title;
    QString purpose;
    QString impact;
};

FileExplanation explainIntegrationFile(const QString& relative)
{
    const QString path = normalizePath(relative);
    if (path.endsWith(QStringLiteral(".meta"), Qt::CaseInsensitive)) {
        return {
            QStringLiteral("unity_metadata"),
            QStringLiteral("Unity identity and import settings"),
            QStringLiteral("These files let Unity keep stable asset references and remember how neighboring Integration Package files should be imported."),
            QStringLiteral("Keeping them unchanged preserves existing Unity references and platform import settings.")};
    }
    if (path == QStringLiteral("package.json")) {
        return {
            QStringLiteral("package_identity"),
            QStringLiteral("Package identity and compatibility"),
            QStringLiteral("Describes the Harken package name, version, Unity compatibility, and optional samples to Unity Package Manager."),
            QStringLiteral("Unity will recognize and load the selected Harken Integration Package version with its current package settings.")};
    }
    if (path == QLatin1String(kIntegrationManifest)) {
        return {
            QStringLiteral("integration_safety"),
            QStringLiteral("Integration safety rules"),
            QStringLiteral("Defines which Integration Package files are mandatory and which Unity target runtimes should be available on each editor operating system."),
            QStringLiteral("Future checks can detect an incomplete or damaged Harken Integration Package before changing project files.")};
    }
    if (path == QStringLiteral("Runtime/Harken.Core.asmdef") ||
        path == QStringLiteral("Runtime/Unity/Harken.Unity.asmdef")) {
        return {
            QStringLiteral("managed_assembly"),
            QStringLiteral("Unity C# assembly settings"),
            QStringLiteral("Tells Unity how to compile the Harken C# runtime and host-adapter assemblies and which dependencies they need."),
            QStringLiteral("Unity scripts can compile against the Harken API using the settings required by this Integration Package version.")};
    }
    if (path.startsWith(QStringLiteral("Runtime/Generated/"))) {
        return {
            QStringLiteral("generated_binding"),
            QStringLiteral("C# connection to the native audio engine"),
            QStringLiteral("These generated files define native function calls, shared data types, constants, callbacks, and compatibility checks."),
            QStringLiteral("Unity scripts and the native Harken runtime will use matching function and data definitions.")};
    }
    if (path.startsWith(QStringLiteral("Runtime/API/"))
        || path.startsWith(QStringLiteral("Runtime/Sdk/"))) {
        return {
            QStringLiteral("managed_api"),
            QStringLiteral("C# gameplay API"),
            QStringLiteral("These files provide the C# API for engine lifecycle, actions, emitters, listeners, Packs, gameplay values, voices, and callbacks."),
            QStringLiteral("Unity gameplay code will use the behavior and API contract supplied by this Integration Package version.")};
    }
    if (path.startsWith(QStringLiteral("Runtime/Unity/"))) {
        return {
            QStringLiteral("unity_adapter"),
            QStringLiteral("Unity spatial adapter"),
            QStringLiteral("Converts Unity world-space positions and directions into the fixed Harken coordinate system."),
            QStringLiteral("Listener and emitter updates will preserve Unity left/right orientation without project-specific conversion code.")};
    }
    if (path.startsWith(QStringLiteral("Runtime/Plugins/")) &&
        (path.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive) ||
         path.endsWith(QStringLiteral(".so"), Qt::CaseInsensitive) ||
         path.endsWith(QStringLiteral(".dylib"), Qt::CaseInsensitive) ||
         path.endsWith(QStringLiteral(".bundle"), Qt::CaseInsensitive) ||
         path.endsWith(QStringLiteral(".a"), Qt::CaseInsensitive))) {
        return {
            QStringLiteral("native_runtime"),
            QStringLiteral("Native Harken audio engine"),
            QStringLiteral("Contains the compiled Harken audio engine that Unity loads on the platform indicated by this folder."),
            QStringLiteral("Harken audio can run in the matching Unity Editor or game build. Other platforms are unaffected.")};
    }
    if (path.startsWith(QStringLiteral("Samples~/"))) {
        return {
            QStringLiteral("samples"),
            QStringLiteral("Optional Unity examples"),
            QStringLiteral("Provides an optional example showing how a Unity project can call the Harken Integration Package."),
            QStringLiteral("The example becomes available for learning or import; it does not change game behavior unless the user imports it.")};
    }
    if (path.endsWith(QStringLiteral("README.md"), Qt::CaseInsensitive) ||
        path.endsWith(QStringLiteral("CHANGELOG.md"), Qt::CaseInsensitive) ||
        path.endsWith(QStringLiteral("LICENSE.md"), Qt::CaseInsensitive)) {
        return {
            QStringLiteral("documentation"),
            QStringLiteral("Documentation and release information"),
            QStringLiteral("Provides documentation, release notes, or licensing information for this part of the Harken Integration Package."),
            QStringLiteral("Users will see documentation that matches the installed Integration Package; runtime behavior is unchanged.")};
    }
    return {
        QStringLiteral("supporting_content"),
        QStringLiteral("Supporting package content"),
        QStringLiteral("Provides supporting content used by the embedded Harken Unity package."),
        QStringLiteral("The installed package will contain the supporting data expected by this Integration Package version.")};
}

using FileChange = UnityIntegrationService::FileChange;

QString changeTitle(const QString& action)
{
    if (action == QStringLiteral("add")) return QStringLiteral("Files to add");
    if (action == QStringLiteral("replace")) return QStringLiteral("Files to update");
    if (action == QStringLiteral("remove")) return QStringLiteral("Obsolete files to remove");
    return QStringLiteral("Files kept unchanged");
}

QString changeReason(const QString& action)
{
    if (action == QStringLiteral("add")) {
        return QStringLiteral("This content is missing from the Unity project and will be added from the verified Integration Package.");
    }
    if (action == QStringLiteral("replace")) {
        return QStringLiteral("The installed content differs from the verified Integration Package source and will be updated to the selected version.");
    }
    if (action == QStringLiteral("remove")) {
        return QStringLiteral("A previous Harken Integration Package installation managed this content, but the current Integration Package no longer uses it; removing it avoids obsolete code or runtime conflicts.");
    }
    return QStringLiteral("This content exists only in the Unity project and is not managed by the selected Integration Package source, so it will remain unchanged.");
}

QStringList groupedLocations(const QStringList& relativePaths,
                             const QString& targetLabel)
{
    QMap<QString, QStringList> filesByDirectory;
    for (const QString& original : relativePaths) {
        const QString relative = normalizePath(original);
        const int separator = relative.lastIndexOf(QLatin1Char('/'));
        const QString directory = separator < 0 ? QString{} : relative.left(separator);
        const QString fileName = separator < 0 ? relative : relative.mid(separator + 1);
        filesByDirectory[directory] << fileName;
    }

    QStringList locations;
    for (auto it = filesByDirectory.cbegin(); it != filesByDirectory.cend(); ++it) {
        const QString base = it.key().isEmpty()
            ? targetLabel
            : QStringLiteral("%1/%2").arg(targetLabel, it.key());
        const QStringList files = it.value();
        if (files.size() == 1) {
            locations << QStringLiteral("Location: %1/%2").arg(base, files.front());
            continue;
        }
        QStringList examples = files.mid(0, 6);
        QString suffix;
        if (files.size() > examples.size())
            suffix = QStringLiteral(", and %1 more").arg(files.size() - examples.size());
        locations << QStringLiteral("Location: %1/ — %2 files (%3%4)")
                         .arg(base).arg(files.size())
                         .arg(examples.join(QStringLiteral(", ")), suffix);
    }
    return locations;
}

QStringList describeFileChanges(const QVector<FileChange>& changes,
                                const QString& targetLabel)
{
    struct Group
    {
        QString action;
        FileExplanation explanation;
        QStringList paths;
    };
    QVector<Group> groups;
    for (const FileChange& change : changes) {
        const FileExplanation explanation = explainIntegrationFile(change.relative);
        auto found = std::find_if(groups.begin(), groups.end(), [&](const Group& group) {
            return group.action == change.action &&
                   group.explanation.key == explanation.key;
        });
        if (found == groups.end()) {
            groups.push_back({change.action, explanation, {change.relative}});
        } else {
            found->paths << change.relative;
        }
    }

    QStringList descriptions;
    for (const Group& group : groups) {
        QStringList lines;
        lines << QStringLiteral("%1 — %2 (%3 files)")
                     .arg(changeTitle(group.action), group.explanation.title)
                     .arg(group.paths.size());
        lines << groupedLocations(group.paths, targetLabel);
        lines << QStringLiteral("Purpose: %1").arg(group.explanation.purpose);
        lines << QStringLiteral("Planned change: %1").arg(changeReason(group.action));
        lines << QStringLiteral("Impact: %1").arg(group.explanation.impact);
        descriptions << lines.join(QLatin1Char('\n'));
    }
    return descriptions;
}

QJsonObject sourceReleaseHint(const QString& sourceRoot)
{
    QDir parent(sourceRoot);
    for (int depth = 0; depth < 8; ++depth) {
        QFile file(parent.filePath(QStringLiteral("package-manifest.json")));
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly) || file.size() > VNH::PackageInventory::maximumBytes) return {};
            const auto bytes = file.readAll();
            const auto identity = QJsonDocument::fromJson(bytes).object();
            const auto inventory = VNH::PackageInventory::parse(bytes,
                identity.value(QStringLiteral("version")).toString(), identity.value(QStringLiteral("buildId")).toString());
            if (!inventory.ok || inventory.unityRoot.isEmpty()
                || !UnityInstallation::samePath(parent.filePath(inventory.unityRoot), sourceRoot)) return {};
            return QJsonObject{{QStringLiteral("buildId"), identity.value(QStringLiteral("buildId"))},
                {QStringLiteral("version"), identity.value(QStringLiteral("version"))},
                {QStringLiteral("channel"), inventory.channel},
                {QStringLiteral("packageManifestSha256"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
        }
        if (!parent.cdUp()) break;
    }
    return {};
}

bool writeInstalledState(const QString& stageRoot,
                         const QString& version,
                         const QStringList& managedFiles,
                         const QMap<QString, QByteArray>& hashes,
                         const QMap<QString, QByteArray>& textHashes,
                         QString* error, const QJsonObject& sourceRelease = {})
{
    QJsonObject root;
    root[QStringLiteral("schema_version")] =
        kIntegrationStateVersion;
    root[QStringLiteral("package_name")] = QLatin1String(kPackageName);
    root[QStringLiteral("package_version")] = version;
    if (!sourceRelease.isEmpty()) root[QStringLiteral("source_release")] = sourceRelease;
    root[QStringLiteral("installed_at_utc")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    root[QStringLiteral("managed_files")] = QJsonArray::fromStringList(managedFiles);
    QJsonObject fileHashes;
    for (auto it = hashes.cbegin(); it != hashes.cend(); ++it)
        fileHashes[it.key()] = QString::fromLatin1(it.value());
    root[QStringLiteral("sha256")] = fileHashes;
    QJsonObject textFileHashes;
    for (auto it = textHashes.cbegin(); it != textHashes.cend(); ++it)
        textFileHashes[it.key()] = QString::fromLatin1(it.value());
    root[QStringLiteral("text_lf_sha256")] = textFileHashes;

    QSaveFile file(QDir(stageRoot).filePath(QLatin1String(kInstalledState)));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

struct OwnershipRecord
{
    ReadStatus status = ReadStatus::Missing;
    QSet<QString> managedFiles;

    bool unreadable() const { return status == ReadStatus::Unreadable; }
};

OwnershipRecord previousManagedFiles(const QString& targetRoot)
{
    const JsonRead state = readJsonObject(
        QDir(targetRoot).filePath(QLatin1String(kInstalledState)));
    OwnershipRecord record;
    record.status = state.status;
    if (state.status == ReadStatus::Ok
        && (state.object.value(QStringLiteral("schema_version")).toInt(-1)
                != kIntegrationStateVersion
            || state.object.value(QStringLiteral("package_name")).toString()
                != QLatin1String(kPackageName)
            || !state.object.value(QStringLiteral("managed_files")).isArray())) {
        record.status = ReadStatus::Invalid;
        return record;
    }
    for (const QJsonValue& entry :
         state.object.value(QStringLiteral("managed_files")).toArray()) {
        if (!entry.isString()
            || !isSafeRelativePath(entry.toString())) {
            record.status = ReadStatus::Invalid;
            record.managedFiles.clear();
            return record;
        }
        record.managedFiles.insert(entry.toString());
    }
    return record;
}

bool integrationSnapshot(const QString& root, QJsonObject* snapshot, QString* error)
{
    if (!UnityInstallation::validateTree(root, error)) return false;
    QJsonObject hashes;
    QStringList directories;
    QDirIterator entries(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        VNH::IntegrationProgress::inspectedEntry();
        const QString path = entries.next();
        const QString relative = QDir(root).relativeFilePath(path);
        if (!isSafeRelativePath(relative)) {
            *error = QStringLiteral("The Integration Package contains an unsafe path.");
            return false;
        }
        if (entries.fileInfo().isDir()) {
            directories << relative;
        } else {
            const FileDigest digest = sha256File(path);
            if (digest.status != ReadStatus::Ok) {
                *error = QStringLiteral("Cannot read Integration Package file: %1").arg(path);
                return false;
            }
            hashes[relative] = QString::fromLatin1(digest.hash);
        }
    }
    directories.sort();
    *snapshot = QJsonObject{{QStringLiteral("files"), hashes},
                           {QStringLiteral("directories"), QJsonArray::fromStringList(directories)}};
    return true;
}

bool removalSnapshot(const QString& root, QJsonObject* snapshot, QString* error)
{
    if (QFileInfo::exists(root)) {
        if (!integrationSnapshot(root, snapshot, error)) return false;
    } else {
        *snapshot = QJsonObject{{QStringLiteral("files"), QJsonObject{}},
                               {QStringLiteral("directories"), QJsonArray{}}};
    }
    const QString metadata = root + QStringLiteral(".meta");
    if (!UnityInstallation::validatePath(metadata, error)) return false;
    const FileDigest digest = sha256File(metadata);
    if (digest.unreadable()) {
        *error = QStringLiteral("Cannot read Integration Package file: %1").arg(metadata);
        return false;
    }
    (*snapshot)[QStringLiteral("root_meta")] = QString::fromLatin1(digest.hash);
    return true;
}

QByteArray integrationChangeFingerprint(const QString& project, const QString& source,
                               const QString& target, const QMap<QString, QByteArray>& checkedSource,
                               QJsonObject* targetSnapshot, QString* error)
{
    QJsonObject sourceSnapshot;
    if (!integrationSnapshot(source, &sourceSnapshot, error)) return {};
    const auto sourceFiles = sourceSnapshot.value(QStringLiteral("files")).toObject();
    QMap<QString, QByteArray> currentSource;
    for (auto file = sourceFiles.begin(); file != sourceFiles.end(); ++file)
        if (!ignoredRelativePath(file.key())) currentSource.insert(file.key(), file.value().toString().toLatin1());
    if (currentSource != checkedSource) {
        *error = QStringLiteral("Integration Package files changed after the safety check. Run the check again before continuing.");
        return {};
    }
    if (QFileInfo::exists(target) && !integrationSnapshot(target, targetSnapshot, error)) return {};
    const QJsonObject snapshot{
        {QStringLiteral("project"), project},
        {QStringLiteral("source"), source},
        {QStringLiteral("target"), target},
        {QStringLiteral("targetExists"), QDir(target).exists()},
        {QStringLiteral("sourceFiles"), sourceSnapshot},
        {QStringLiteral("targetFiles"), *targetSnapshot}};
    return QCryptographicHash::hash(QJsonDocument(snapshot).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}

bool matchesMigrationSnapshot(const QString& root, const QJsonObject& expected, QString* error)
{
    QJsonObject actual;
    if (!integrationSnapshot(root, &actual, error)) return false;
    if (actual != expected) {
        *error = QStringLiteral("Integration Package contents changed during migration. No existing files will be overwritten.");
        return false;
    }
    return true;
}

bool emptyMigrationDestination(const QString& path, QString* error)
{
    if (!UnityInstallation::validatePath(path, error)
        || !UnityInstallation::validatePath(path + QStringLiteral(".meta"), error)) return false;
    if (QFileInfo::exists(path)
        && (!QFileInfo(path).isDir() || !QDir(path).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))) {
        *error = QStringLiteral("The migration destination must be an empty directory.");
        return false;
    }
    if (QFileInfo::exists(path + QStringLiteral(".meta"))) {
        *error = QStringLiteral("The destination already has Unity metadata. Choose a new directory to preserve existing GUIDs.");
        return false;
    }
    return true;
}

bool validateMigration(const QString& project, const QString& from, const QString& to, QString* error)
{
    if (!checkUnityProjectClosed(project, error)) return false;
    if (!UnityInstallation::validateTarget(project, from, error)
        || !UnityInstallation::validatePath(from + QStringLiteral(".meta"), error)) return false;
    if (UnityInstallation::containsPath(from, to) || UnityInstallation::containsPath(to, from)) {
        *error = QStringLiteral("The current and new Integration Package locations must be separate directories without containment.");
        return false;
    }
    if (!emptyMigrationDestination(to, error)) return false;
    const auto discovery = UnityInstallation::discover(project);
    if (!discovery.errors.isEmpty()) {
        *error = discovery.errors.join(QLatin1Char('\n'));
        return false;
    }
    if (!discovery.integrationReferences.isEmpty()) {
        *error = QStringLiteral("Resolve Integration Package dependencies before migration to avoid loading another Integration Package or breaking a package reference.");
        return false;
    }
    for (const QString& reference : discovery.localPackagePaths) {
        if (UnityInstallation::containsPath(from, reference) || UnityInstallation::containsPath(reference, from)
            || UnityInstallation::containsPath(to, reference) || UnityInstallation::containsPath(reference, to)) {
            *error = QStringLiteral("Resolve Integration Package dependencies before migration to avoid loading another Integration Package or breaking a package reference.");
            return false;
        }
    }
    const OwnershipRecord ownership = previousManagedFiles(from);
    if (ownership.status != ReadStatus::Ok || ownership.managedFiles.isEmpty()) {
        *error = QStringLiteral("A valid Integration Package ownership record is required for migration.");
        return false;
    }
    const JsonRead manifest = readJsonObject(QDir(from).filePath(QLatin1String(kIntegrationManifest)));
    const JsonRead package = readJsonObject(QDir(from).filePath(QStringLiteral("package.json")));
    if (manifest.status != ReadStatus::Ok || package.status != ReadStatus::Ok
        || manifest.object.value(QStringLiteral("schema_version")).toInt() != 3
        || manifest.object.value(QStringLiteral("package_name")).toString() != QLatin1String(kPackageName)
        || package.object.value(QStringLiteral("name")).toString() != QLatin1String(kPackageName)) {
        *error = QStringLiteral("The installed Integration Package identity or integration manifest is invalid.");
        return false;
    }
    const QString relative = QDir(project).relativeFilePath(to);
    if (relative != UnityInstallation::defaultRelativePath() && relative != QStringLiteral("Assets/HarkenSDK")
        && !manifest.object.value(QStringLiteral("supports_custom_install_path")).toBool()) {
        *error = QStringLiteral("This Integration Package version does not support a custom installation location.");
        return false;
    }
    const QJsonObject dependencies = package.object.value(QStringLiteral("dependencies")).toObject();
    for (auto it = dependencies.begin(); it != dependencies.end(); ++it) {
        if (it.value().toString().startsWith(QStringLiteral("file:"))) {
            *error = QStringLiteral("The installed Integration Package has local package dependencies. Resolve them before migration.");
            return false;
        }
    }
    return true;
}

bool validateMigrationBackup(const QString& project, const QString& backup, QString* error)
{
    using namespace UnityInstallation;
    if (backup.isEmpty() || containsPath(project, backup) || containsPath(backup, project)) {
        *error = QStringLiteral("Choose a backup directory outside the Unity project.");
        return false;
    }
    if (!validatePath(backup, error)) return false;
    if (QFileInfo::exists(backup)) {
        *error = QStringLiteral("The planned backup folder already exists.");
        return false;
    }
    const auto discovery = discover(project);
    if (!discovery.errors.isEmpty()) {
        *error = discovery.errors.join(QLatin1Char('\n'));
        return false;
    }
    for (const QString& reference : discovery.localPackagePaths) {
        if (containsPath(reference, backup) || containsPath(backup, reference)) {
            *error = QStringLiteral("The backup location overlaps a local Unity package. Choose another backup directory.");
            return false;
        }
    }
    return true;
}

bool recoverIntegrationMigration(const QString& project, const QJsonObject& record, bool apply, QString* error)
{
    using namespace UnityInstallation;
    QString from;
    QString to;
    if (record.value(QStringLiteral("schema_version")).toInt() != 1
        || record.value(QStringLiteral("source")).toString().isEmpty()
        || record.value(QStringLiteral("target")).toString().isEmpty()
        || !samePath(record.value(QStringLiteral("project")).toString(), project)
        || !resolveTarget(project, record.value(QStringLiteral("source")).toString(), &from, error)
        || !resolveTarget(project, record.value(QStringLiteral("target")).toString(), &to, error)
        || containsPath(from, to) || containsPath(to, from)) {
        if (error->isEmpty()) *error = QStringLiteral("The Integration Package migration record is invalid. Restore the verified backup manually.");
        return false;
    }
    if (!checkUnityProjectClosed(project, error)) return false;
    if (!validatePath(from + QStringLiteral(".meta"), error)
        || !validatePath(to + QStringLiteral(".meta"), error)) return false;
    const auto sourceOwnership = previousManagedFiles(from);
    const bool atSource = sourceOwnership.status == ReadStatus::Ok && !sourceOwnership.managedFiles.isEmpty();
    const QString current = atSource ? from : to;
    const QString other = atSource ? to : from;
    const auto ownership = atSource ? sourceOwnership : previousManagedFiles(to);
    if (ownership.status != ReadStatus::Ok || ownership.managedFiles.isEmpty()
        || (QDir(other).exists() && !QDir(other).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))) {
        *error = QStringLiteral("Integration Package migration recovery found conflicting or missing files. Preserve both locations and restore the verified backup manually.");
        return false;
    }
    if (!matchesMigrationSnapshot(current, record.value(QStringLiteral("inventory")).toObject(), error)) return false;
    const QString fromMeta = from + QStringLiteral(".meta");
    const QString toMeta = to + QStringLiteral(".meta");
    const bool metaAtSource = QFileInfo::exists(fromMeta);
    const bool metaAtTarget = QFileInfo::exists(toMeta);
    const QByteArray expectedMeta = record.value(QStringLiteral("root_meta_sha256")).toString().toLatin1();
    if ((expectedMeta.isEmpty() && (metaAtSource || metaAtTarget))
        || (!expectedMeta.isEmpty() && (metaAtSource == metaAtTarget
            || !sha256File(metaAtSource ? fromMeta : toMeta).matches(expectedMeta)))) {
        *error = QStringLiteral("Integration Package directory metadata changed. Recovery will not overwrite Unity GUIDs.");
        return false;
    }
    if (!apply) return true;
    QDir filesystem;
    if ((!atSource && QDir(from).exists() && !filesystem.rmdir(from))
        || (metaAtTarget && !QFile::rename(toMeta, fromMeta))
        || (!atSource && !VNH::renameIntegrationDirectory(filesystem.filePath(to), filesystem.filePath(from)))
        || (record.value(QStringLiteral("target_existed")).toBool() && !QDir().mkpath(to))) {
        *error = QStringLiteral("The Integration Package could not be restored to its previous location. Close programs using these files and retry recovery.");
        return false;
    }
    if (!matchesMigrationSnapshot(from, record.value(QStringLiteral("inventory")).toObject(), error)) return false;
    if ((!expectedMeta.isEmpty() && !sha256File(fromMeta).matches(expectedMeta))
        || (expectedMeta.isEmpty() && QFileInfo::exists(fromMeta)) || QFileInfo::exists(toMeta)) {
        *error = QStringLiteral("Integration Package directory metadata changed. Recovery will not overwrite Unity GUIDs.");
        return false;
    }
    if (!QFile::remove(migrationJournalPath(project))) {
        *error = QStringLiteral("The Integration Package migration record could not be cleared. Retry recovery before continuing.");
        return false;
    }
    return true;
}

Qt::CaseSensitivity integrationPathCaseSensitivity()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString comparisonPathKey(const QString& relative)
{
    const QString normalized = normalizePath(relative);
    return integrationPathCaseSensitivity() == Qt::CaseInsensitive
        ? normalized.toCaseFolded()
        : normalized;
}

QSet<QString> comparisonPathKeys(const QStringList& paths)
{
    QSet<QString> keys;
    for (const QString& path : paths) keys.insert(comparisonPathKey(path));
    return keys;
}

QSet<QString> comparisonPathKeys(const QSet<QString>& paths)
{
    QSet<QString> keys;
    for (const QString& path : paths) keys.insert(comparisonPathKey(path));
    return keys;
}

QString harkenRuntimeIdentity(const QString& relative)
{
    const QString normalized = normalizePath(relative);
    const int slash = normalized.lastIndexOf(QLatin1Char('/'));
    if (slash < 0 ||
        !normalized.left(slash + 1).startsWith(
            QStringLiteral("Runtime/Plugins/"),
            integrationPathCaseSensitivity())) {
        return {};
    }

    QString fileName = normalized.mid(slash + 1).toCaseFolded();
    fileName.remove(QLatin1Char('_'));
    if (fileName != QStringLiteral("harkencore.dll") &&
        fileName != QStringLiteral("libharkencore.so") &&
        fileName != QStringLiteral("libharkencore.dylib") &&
        fileName != QStringLiteral("libharkencore.a")) {
        return {};
    }
    return normalized.left(slash + 1).toCaseFolded() + fileName;
}

bool matchesDeclaredArtifact(const QString& relative,
                             const QSet<QString>& declaredArtifacts)
{
    const QString normalized = normalizePath(relative);
    const Qt::CaseSensitivity sensitivity = integrationPathCaseSensitivity();
    for (const QString& artifact : declaredArtifacts) {
        const QString normalizedArtifact = normalizePath(artifact);
        if (normalized.compare(normalizedArtifact, sensitivity) == 0 ||
            normalized.startsWith(normalizedArtifact + QLatin1Char('/'),
                                  sensitivity)) {
            return true;
        }
    }
    return false;
}

QString joinedCountSummary(int added, int changed, int removed,
                           int projectSpecific, int unityMetadata,
                           int optionalRuntimes)
{
    return QStringLiteral("%1 missing Integration Package file(s) will be added; %2 existing Integration Package file(s) "
                          "need updating; %3 obsolete Harken-managed file(s) will be removed; "
                          "%4 project-specific file(s) will be kept unchanged; "
                          "%5 Unity-generated metadata file(s) and %6 optional target runtime "
                          "file(s) will be kept automatically")
        .arg(added).arg(changed).arg(removed).arg(projectSpecific)
        .arg(unityMetadata).arg(optionalRuntimes);
}

QString jsonArrayText(const QStringList& values)
{
    return values.join(QStringLiteral("\n\n"));
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
        const QJsonObject package = readJsonObject(
            QDir(root).filePath(QStringLiteral("package.json"))).object;
        if (package.value(QStringLiteral("name")).toString() ==
            QLatin1String(kPackageName)) {
            return root;
        }
        return QString{};
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
        QByteArray fromEnv = qgetenv("HARKEN_UNITY_INTEGRATION_PATH");
        if (fromEnv.isEmpty()) fromEnv = qgetenv("HARKEN_UNITY_SDK_PATH");
        if (!fromEnv.isEmpty()) candidates << QString::fromLocal8Bit(fromEnv);

        QDir cursor(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 6; ++i) {
            candidates << cursor.filePath(QStringLiteral("HarkenIntegration/Unity/Harken"));
            candidates << cursor.filePath(QStringLiteral("HarkenIntegration"));
            candidates << cursor.filePath(QStringLiteral("HarkenSDK/Unity/Harken"));
            candidates << cursor.filePath(QStringLiteral("HarkenSDK"));
            if (!cursor.cdUp()) break;
        }
    }

    for (const QString& candidate : candidates) {
        const QString packageRoot = normalizeCandidate(candidate);
        if (!packageRoot.isEmpty()) return packageRoot;
    }
    return {};
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

UnityIntegrationService::Result UnityIntegrationService::migrateIntegration(const Request& request)
{
    using namespace UnityInstallation;
    Result result;
    result.unityProjectPath = cleanAbsolutePath(request.unityProjectPath);
    const QString project = result.unityProjectPath;
    QString error;
    std::unique_ptr<QLockFile> operationLock;
    QElapsedTimer timer;
    const auto beginStep = [&](const char* id, const char* title) {
        StepResult step;
        step.id = QString::fromLatin1(id);
        step.title = QString::fromLatin1(title);
        step.status = StepStatus::Running;
        result.steps.push_back(step);
        timer.restart();
        emit stepUpdated(result.steps.size() - 1, result.steps.back());
    };
    const auto finishStep = [&](StepStatus status, const QString& summary) {
        auto& step = result.steps.back();
        step.status = status;
        step.summary = summary;
        step.elapsedMs = timer.elapsed();
        emit stepUpdated(result.steps.size() - 1, step);
    };
    const auto fail = [&](const QString& cause) {
        result.steps.back().cause = cause;
        finishStep(StepStatus::Failed, QStringLiteral("Integration Package migration could not be completed."));
        return result;
    };
    beginStep("migration_review", "Review Integration Package migration");
    if (request.unityProjectPath.trimmed().isEmpty()
        || !QDir(project).exists(QStringLiteral("Assets"))
        || !QDir(project).exists(QStringLiteral("Packages"))
        || !QDir(project).exists(QStringLiteral("ProjectSettings")))
        return fail(QStringLiteral("Selected directory is not a Unity project root."));
    if (!lockUnityProject(project, &operationLock, &error)) return fail(error);
    if (UnityInstallation::hasPendingRemoval(project))
        return fail(QStringLiteral("An unfinished Integration Package removal must be completed before continuing. Use Uninstall Integration Package to retry."));
    if (request.removeIntegration || request.checkOnly)
        return fail(QStringLiteral("Migration cannot be combined with installation, removal or check-only mode."));

    QJsonObject record;
    QString from;
    QString to;
    if (request.recoverMigration) {
        const JsonRead journal = readJsonObject(migrationJournalPath(project));
        if (journal.status != ReadStatus::Ok)
            return fail(QStringLiteral("The Integration Package migration record is invalid. Restore the verified backup manually."));
        record = journal.object;
        if (!recoverIntegrationMigration(project, record, false, &error)) return fail(error);
        result.integrationInstallPath = record.value(QStringLiteral("source")).toString();
        result.migrationTargetPath = record.value(QStringLiteral("target")).toString();
        result.migrationBackupPath = record.value(QStringLiteral("backup")).toString();
    } else {
        if (request.migrationTargetPath.trimmed().isEmpty())
            return fail(QStringLiteral("Select a new Integration Package installation location."));
        if (!resolveTarget(project, requestedInstallPath(request), &from, &error)
            || !resolveTarget(project, request.migrationTargetPath, &to, &error)
            || !validateMigration(project, from, to, &error)) return fail(error);
        result.integrationInstallPath = QDir(project).relativeFilePath(from);
        result.migrationTargetPath = QDir(project).relativeFilePath(to);
        result.migrationBackupPath = request.migrationBackupPath.trimmed().isEmpty()
            ? defaultIntegrationBackupPath(project) : cleanAbsolutePath(request.migrationBackupPath);
        if (!validateMigrationBackup(project, result.migrationBackupPath, &error)) return fail(error);
        QJsonObject inventory;
        if (!integrationSnapshot(from, &inventory, &error)) return fail(error);
        QByteArray metaHash;
        if (QFileInfo::exists(from + QStringLiteral(".meta"))) {
            const FileDigest digest = sha256File(from + QStringLiteral(".meta"));
            if (digest.status != ReadStatus::Ok)
                return fail(QStringLiteral("The Integration Package directory metadata could not be read."));
            metaHash = digest.hash;
        }
        record = QJsonObject{
            {QStringLiteral("schema_version"), 1}, {QStringLiteral("project"), project},
            {QStringLiteral("source"), result.integrationInstallPath},
            {QStringLiteral("target"), result.migrationTargetPath},
            {QStringLiteral("backup"), result.migrationBackupPath},
            {QStringLiteral("inventory"), inventory},
            {QStringLiteral("root_meta_sha256"), QString::fromLatin1(metaHash)},
            {QStringLiteral("target_existed"), QDir(to).exists()}};
    }
    result.integrationFound = true;
    result.targetInstalled = true;
    const QByteArray recordBytes = QJsonDocument(record).toJson(QJsonDocument::Indented);
    result.migrationPlanHash = QString::fromLatin1(
        QCryptographicHash::hash(recordBytes, QCryptographicHash::Sha256).toHex());
    result.steps.back().artifacts = {QDir(project).filePath(result.integrationInstallPath),
                                    QDir(project).filePath(result.migrationTargetPath), result.migrationBackupPath};
    if (!request.confirmMigration) {
        result.requiresConfirmation = true;
        finishStep(StepStatus::Warning, request.recoverMigration
            ? QStringLiteral("Recovery will restore the Integration Package to its previous location without changing project relationships.")
            : QStringLiteral("Migration will preserve all installed files, Unity GUIDs and project relationships."));
        return result;
    }
    if (request.migrationPlanHash.isEmpty() || request.migrationPlanHash != result.migrationPlanHash)
        return fail(QStringLiteral("The migration plan changed after review. Review the migration again."));
    finishStep(StepStatus::Succeeded, QStringLiteral("The migration plan passed safety checks."));
    if (request.recoverMigration) {
        beginStep("recover_migration", "Recover Integration Package migration");
        if (!recoverIntegrationMigration(project, record, true, &error)) return fail(error);
        result.migrationRecovered = true;
        result.success = true;
        finishStep(StepStatus::Succeeded, QStringLiteral("The Integration Package was restored to its previous location. Project relationships were kept."));
        return result;
    }

    beginStep("migration_backup", "Back up installed Integration Package");
    const QString backup = QDir(result.migrationBackupPath).filePath(QStringLiteral("SDK"));
    const QJsonObject inventory = record.value(QStringLiteral("inventory")).toObject();
    const QByteArray metaHash = record.value(QStringLiteral("root_meta_sha256")).toString().toLatin1();
    const QString fromMeta = from + QStringLiteral(".meta");
    const QString toMeta = to + QStringLiteral(".meta");
    if (!validateMigration(project, from, to, &error)
        || !validateMigrationBackup(project, result.migrationBackupPath, &error)) return fail(error);
    if (!QDir().mkpath(backup)) return fail(QStringLiteral("The Integration Package migration backup could not be created."));
    for (const QJsonValue& directory : inventory.value(QStringLiteral("directories")).toArray()) {
        if (!QDir().mkpath(QDir(backup).filePath(directory.toString())))
            return fail(QStringLiteral("The Integration Package migration backup could not be created."));
    }
    if (!copyFiles(from, backup, inventory.value(QStringLiteral("files")).toObject().keys(), &error)
        || (!metaHash.isEmpty() && !copyFileAtomically(fromMeta, backup + QStringLiteral(".meta"), &error))
        || !matchesMigrationSnapshot(backup, inventory, &error)) return fail(error);
    if (!metaHash.isEmpty() && !sha256File(backup + QStringLiteral(".meta")).matches(metaHash))
        return fail(QStringLiteral("Integration Package directory metadata backup verification failed."));
    if (!VNH::writeIntegrationFile(QDir(result.migrationBackupPath).filePath(QStringLiteral("migration.json")), recordBytes, &error))
        return fail(error);
    result.steps.back().artifacts << result.migrationBackupPath;
    finishStep(StepStatus::Succeeded, QStringLiteral("All installed Integration Package files and directory metadata were backed up and verified."));

    beginStep("migrate_sdk", "Move installed Integration Package");
    if (!validateMigration(project, from, to, &error)
        || !matchesMigrationSnapshot(from, inventory, &error)) return fail(error);
    if ((!metaHash.isEmpty() && !sha256File(fromMeta).matches(metaHash))
        || (metaHash.isEmpty() && QFileInfo::exists(fromMeta)))
        return fail(QStringLiteral("Integration Package directory metadata changed. Migration was cancelled."));
    if (!QDir().mkpath(QFileInfo(to).absolutePath()))
        return fail(QStringLiteral("The Integration Package migration destination could not be created."));
    if (!VNH::writeIntegrationFile(migrationJournalPath(project), recordBytes, &error)) return fail(error);
    const auto rollback = [&](const QString& cause) {
        QString recoveryError;
        if (recoverIntegrationMigration(project, record, true, &recoveryError))
            return fail(cause + QLatin1Char('\n') + QStringLiteral("The Integration Package was restored to its previous location. Project relationships were kept."));
        result.steps.back().remediation = QStringLiteral("Use Recover to finish the interrupted migration before installing, updating or removing the Integration Package.");
        result.steps.back().artifacts << result.migrationBackupPath;
        return fail(cause + QLatin1Char('\n') + recoveryError);
    };
    QDir filesystem;
    if ((QDir(to).exists() && !filesystem.rmdir(to)) || !VNH::renameIntegrationDirectory(filesystem.filePath(from), filesystem.filePath(to)))
        return rollback(QStringLiteral("The Integration Package directory could not be moved."));
    if (!metaHash.isEmpty() && !QFile::rename(fromMeta, toMeta))
        return rollback(QStringLiteral("The Integration Package directory metadata could not be moved."));
    finishStep(StepStatus::Succeeded, QStringLiteral("The existing Integration Package directory was moved without replacing its contents."));
    beginStep("verify_migration", "Verify Integration Package migration");
    if (!matchesMigrationSnapshot(to, inventory, &error)) return rollback(error);
    if ((!metaHash.isEmpty() && !sha256File(toMeta).matches(metaHash))
        || (metaHash.isEmpty() && QFileInfo::exists(toMeta)) || QFileInfo::exists(fromMeta)
        || QFileInfo::exists(from))
        return rollback(QStringLiteral("Integration Package directory metadata changed. Migration was cancelled."));
    const auto discovery = discover(project);
    if (!discovery.errors.isEmpty() || discovery.installations.size() != 1
        || !samePath(discovery.installations.front().path, to) || !discovery.integrationReferences.isEmpty())
        return rollback(QStringLiteral("The Unity project changed during migration. Review Integration Package paths and dependencies again."));
    if (!QFile::remove(migrationJournalPath(project)))
        return rollback(QStringLiteral("The Integration Package migration record could not be cleared. Retry recovery before continuing."));
    result.integrationInstallPath = result.migrationTargetPath;
    result.integrationMigrated = true;
    result.success = true;
    finishStep(StepStatus::Succeeded, QStringLiteral("Integration Package migration completed. All installed files, Unity GUIDs and project relationships were kept."));
    return result;
}

UnityIntegrationService::Result UnityIntegrationService::removeIntegration(
    const Request& request)
{
    Result result;
    result.unityProjectPath = cleanAbsolutePath(request.unityProjectPath);
    result.steps.reserve(static_cast<int>(std::size(kRemovalStepSpecs)));
    for (const StepSpec& spec : kRemovalStepSpecs) {
        StepResult step;
        step.id = QString::fromLatin1(spec.id);
        step.title = QString::fromLatin1(spec.title);
        result.steps.push_back(step);
    }

    int nextStep = 0;
    bool blocked = false;
    QString blockingTitle;
    QString targetIntegration;
    std::unique_ptr<QLockFile> operationLock;
    QStringList removalFiles;
    QStringList filesToChange;
    QStringList preservedFiles;
    QStringList integrationBackupFiles;
    QStringList installedFiles;
    bool resuming = false;
    QJsonObject inventory;
    QJsonObject reviewSnapshot;
    QJsonObject journal;

    auto publish = [&](int index) { emit stepUpdated(index, result.steps[index]); };
    auto runStep = [&](const std::function<void(StepResult&)>& body,
                       bool runWhenBlocked = false) {
        StepResult& step = result.steps[nextStep];
        if (blocked && !runWhenBlocked) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Not run because %1 failed.").arg(blockingTitle);
            step.cause = QStringLiteral("A required earlier step did not complete.");
            step.remediation = QStringLiteral("Fix the first failed step, then try removing the integration again.");
            publish(nextStep++);
            return;
        }
        QElapsedTimer timer;
        timer.start();
        step.status = StepStatus::Running;
        publish(nextStep);
        body(step);
        step.elapsedMs = timer.elapsed();
        if (step.status == StepStatus::Running) step.status = StepStatus::Succeeded;
        if (step.status == StepStatus::Warning) result.hasWarnings = true;
        if (step.status == StepStatus::Failed) {
            blocked = true;
            blockingTitle = step.title;
        }
        publish(nextStep++);
    };

    runStep([&](StepResult& step) {
        const QString root = result.unityProjectPath;
        if (request.unityProjectPath.trimmed().isEmpty() || !QDir(root).exists()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Unity project directory does not exist.");
            step.cause = request.unityProjectPath.trimmed().isEmpty()
                ? QStringLiteral("No Unity project path was selected.")
                : QStringLiteral("The selected path cannot be found: %1").arg(root);
            step.remediation = QStringLiteral("Select the Unity project root containing Assets, Packages, and ProjectSettings.");
            return;
        }
        const QStringList markers = {QStringLiteral("Assets"), QStringLiteral("Packages"),
                                     QStringLiteral("ProjectSettings")};
        QStringList missing;
        for (const QString& marker : markers) {
            if (!QDir(QDir(root).filePath(marker)).exists()) missing << marker;
        }
        if (!missing.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Selected directory is not a Unity project root.");
            step.cause = QStringLiteral("Missing required directories: %1")
                             .arg(missing.join(QStringLiteral(", ")));
            step.remediation = QStringLiteral("Select the parent directory that directly contains Assets, Packages, and ProjectSettings.");
            return;
        }
        QString error;
        if (!prepareInstallation(request, result, &targetIntegration, &operationLock, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package installation location is not safe to use.");
            step.cause = error;
            step.remediation = QStringLiteral("Resolve the reported path or Integration Package conflict, then check again.");
            return;
        }
        step.summary = QStringLiteral("Unity project layout and write access are valid.");
        step.artifacts << root;
    });

    runStep([&](StepResult& step) {
        resuming = UnityInstallation::hasPendingRemoval(result.unityProjectPath);
        if (resuming) {
            const JsonRead saved = readJsonObject(UnityInstallation::removalJournalPath(result.unityProjectPath));
            journal = saved.object;
            const auto invalid = [&]() {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("The unfinished removal record is invalid or its backup is unavailable. No files were changed.");
                step.artifacts << UnityInstallation::removalJournalPath(result.unityProjectPath);
            };
            QString recordedTarget;
            QString error;
            if (saved.status != ReadStatus::Ok
                || journal.value(QStringLiteral("schema_version")).toInt() != 1
                || journal.value(QStringLiteral("package_name")).toString() != QLatin1String(kPackageName)
                || !UnityInstallation::samePath(journal.value(QStringLiteral("project")).toString(), result.unityProjectPath)
                || !UnityInstallation::resolveTarget(result.unityProjectPath,
                    journal.value(QStringLiteral("install_path")).toString(), &recordedTarget, &error)
                || !UnityInstallation::samePath(recordedTarget, targetIntegration)) {
                invalid();
                return;
            }
            result.removalBackupPath = journal.value(QStringLiteral("backup_path")).toString();
            inventory = journal.value(QStringLiteral("inventory")).toObject();
            const QJsonObject hashes = inventory.value(QStringLiteral("files")).toObject();
            bool valid = QDir::isAbsolutePath(result.removalBackupPath)
                && !UnityInstallation::containsPath(result.unityProjectPath, result.removalBackupPath)
                && !UnityInstallation::containsPath(result.removalBackupPath, result.unityProjectPath)
                && hashes.contains(QLatin1String(kInstalledState))
                && inventory.value(QStringLiteral("directories")).isArray()
                && journal.value(QStringLiteral("removal_files")).isArray();
            for (auto it = hashes.begin(); it != hashes.end(); ++it)
                valid &= isSafeRelativePath(it.key()) && it.value().isString()
                    && it.value().toString().size() == 64
                    && QByteArray::fromHex(it.value().toString().toLatin1()).size() == 32;
            for (const auto& entry : inventory.value(QStringLiteral("directories")).toArray())
                valid &= entry.isString() && isSafeRelativePath(entry.toString());
            for (const auto& entry : journal.value(QStringLiteral("removal_files")).toArray()) {
                valid &= entry.isString() && hashes.contains(entry.toString());
                removalFiles << entry.toString();
            }
            valid &= removalFiles.contains(QLatin1String(kInstalledState));
            QJsonObject backupInventory;
            const QString snapshotRoot = QDir(result.removalBackupPath).filePath(
                QStringLiteral("UnityProject/") + result.integrationInstallPath);
            if (!valid || !UnityInstallation::validatePath(result.removalBackupPath, &error)
                || readJsonObject(QDir(result.removalBackupPath).filePath(QStringLiteral("backup.json"))).object != journal
                || !removalSnapshot(snapshotRoot, &backupInventory, &error) || backupInventory != inventory) {
                invalid();
                step.cause = error;
                return;
            }
            result.removalBackupCreated = true;
        } else {
            if (!QDir(targetIntegration).exists()) {
                step.summary = QStringLiteral("No embedded Harken Integration Package is installed in this Unity project.");
                return;
            }
            result.integrationFound = true;
            result.targetInstalled = true;
            const JsonRead state = readJsonObject(QDir(targetIntegration).filePath(QLatin1String(kInstalledState)));
            const OwnershipRecord ownership = previousManagedFiles(targetIntegration);
            if (ownership.status == ReadStatus::Ok && ownership.managedFiles.isEmpty()
                && state.object.value(QStringLiteral("installation_state")).toString() == QStringLiteral("removed")) {
                result.integrationFound = false;
                result.targetInstalled = false;
                step.summary = QStringLiteral("The Integration Package is removed. Remaining project files are preserved and can be used with a new installation.");
                return;
            }
            if (ownership.status != ReadStatus::Ok
                || !ownership.managedFiles.contains(QStringLiteral("package.json"))
                || !ownership.managedFiles.contains(QLatin1String(kIntegrationManifest))) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("The installed package has no trustworthy Harken ownership record.");
                step.cause = QDir(targetIntegration).filePath(QLatin1String(kInstalledState));
                step.remediation = QStringLiteral("Restore this installation's ownership record from its own backup before removing files.");
                return;
            }
            QString error;
            if (!removalSnapshot(targetIntegration, &inventory, &error)) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("The Integration Package files could not be checked safely.");
                step.cause = error;
                return;
            }
            const QSet<QString> managed = comparisonPathKeys(ownership.managedFiles);
            QSet<QString> declaredRuntimes;
            const JsonRead manifest = readJsonObject(QDir(targetIntegration).filePath(QLatin1String(kIntegrationManifest)));
            if (manifest.object.value(QStringLiteral("schema_version")).toInt() == 3
                && manifest.object.value(QStringLiteral("package_name")).toString() == QLatin1String(kPackageName)) {
                for (const auto& plugin : manifest.object.value(QStringLiteral("native_plugins")).toArray())
                    for (const auto& alternative : plugin.toObject().value(QStringLiteral("alternatives")).toArray())
                        if (alternative.isString() && isSafeRelativePath(alternative.toString()))
                            declaredRuntimes.insert(normalizePath(alternative.toString()));
            }
            installedFiles = inventory.value(QStringLiteral("files")).toObject().keys();
            QSet<QString> removeSet{QLatin1String(kInstalledState)};
            for (const QString& relative : installedFiles) {
                if (discardableOsMetadata(relative) || managed.contains(comparisonPathKey(relative))
                    || matchesDeclaredArtifact(relative, declaredRuntimes) || !harkenRuntimeIdentity(relative).isEmpty())
                    removeSet.insert(relative);
            }
            for (const QString& relative : installedFiles) {
                if (!relative.endsWith(QStringLiteral(".meta"), Qt::CaseInsensitive)) continue;
                const QString base = relative.left(relative.size() - 5);
                if (removeSet.contains(base) || managed.contains(comparisonPathKey(base))
                    || matchesDeclaredArtifact(base, declaredRuntimes)) removeSet.insert(relative);
            }
            for (const QString& relative : installedFiles) {
                if (!relative.endsWith(QStringLiteral(".meta"), Qt::CaseInsensitive) || removeSet.contains(relative)) continue;
                const QString directory = relative.left(relative.size() - 5);
                if (!QFileInfo(QDir(targetIntegration).filePath(directory)).isDir()) continue;
                const QString prefix = directory + QLatin1Char('/');
                if (std::none_of(installedFiles.cbegin(), installedFiles.cend(), [&](const QString& child) {
                        return child.startsWith(prefix) && !removeSet.contains(child);
                    })) removeSet.insert(relative);
            }
            for (const QString& relative : installedFiles)
                if (removeSet.contains(relative)) removalFiles << relative;
            for (const QString& relative : ownership.managedFiles) {
                if (inventory.value(QStringLiteral("files")).toObject().contains(relative)) integrationBackupFiles << relative;
                else result.missingFiles << relative;
            }
            integrationBackupFiles.sort();
            result.missingFiles.sort();
            result.removalBackupPath = request.removalBackupPath.trimmed().isEmpty()
                ? defaultIntegrationBackupPath(result.unityProjectPath) : cleanAbsolutePath(request.removalBackupPath);
            journal = QJsonObject{{QStringLiteral("schema_version"), 1},
                {QStringLiteral("package_name"), QLatin1String(kPackageName)},
                {QStringLiteral("project"), result.unityProjectPath},
                {QStringLiteral("install_path"), result.integrationInstallPath},
                {QStringLiteral("backup_path"), result.removalBackupPath},
                {QStringLiteral("inventory"), inventory},
                {QStringLiteral("removal_files"), QJsonArray::fromStringList(removalFiles)},
                {QStringLiteral("missing_files"), QJsonArray::fromStringList(result.missingFiles)}};
        }
        result.integrationFound = true;
        result.targetInstalled = true;
        QString error;
        if (!removalSnapshot(targetIntegration, &reviewSnapshot, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package files could not be checked safely.");
            step.cause = error;
            return;
        }
        const QJsonObject currentFiles = reviewSnapshot.value(QStringLiteral("files")).toObject();
        for (auto it = currentFiles.begin(); it != currentFiles.end(); ++it) {
            if (removalFiles.contains(it.key())) filesToChange << QDir(targetIntegration).filePath(it.key());
            else preservedFiles << it.key();
        }
        const qsizetype pendingFiles = filesToChange.size();
        const auto currentDirectories = reviewSnapshot.value(QStringLiteral("directories")).toArray();
        const bool preservedDirectory = std::any_of(currentDirectories.cbegin(), currentDirectories.cend(), [&](const QJsonValue& directory) {
            const QString prefix = directory.toString() + QLatin1Char('/');
            return std::none_of(removalFiles.cbegin(), removalFiles.cend(), [&](const QString& relative) {
                return relative.startsWith(prefix);
            });
        });
        if (preservedFiles.isEmpty() && !preservedDirectory) filesToChange << targetIntegration + QStringLiteral(".meta");
        if (!VNH::checkIntegrationFilesWritable(filesToChange, &error)
            || !VNH::checkIntegrationDirectoryChanges({targetIntegration}, {targetIntegration}, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Integration Package file write access check failed.");
            step.cause = error;
            return;
        }
        result.planHash = QCryptographicHash::hash(QJsonDocument(QJsonObject{
            {QStringLiteral("journal"), journal}, {QStringLiteral("current"), reviewSnapshot}}).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256);
        if (!request.approvedPlanHash.isEmpty() && request.approvedPlanHash != result.planHash) {
            step.status = StepStatus::Failed;
            result.requiresConfirmation = true;
            step.summary = QStringLiteral("Integration Package files changed after the safety check. Run the check again before continuing.");
            return;
        }
        result.targetOnlyFilesPreserved = preservedFiles.size();
        step.summary = QStringLiteral("Ready to remove %1 Harken Integration Package/runtime file(s) and preserve %2 project-specific file(s).")
            .arg(pendingFiles).arg(preservedFiles.size());
        step.details << QStringLiteral("Harken Integration Package and runtime files that will be removed:\n%1")
            .arg(groupedLocations(removalFiles, result.integrationInstallPath).join(QLatin1Char('\n')));
        if (!preservedFiles.isEmpty()) step.details << QStringLiteral("Project-specific files that will be preserved:\n%1")
            .arg(groupedLocations(preservedFiles, result.integrationInstallPath).join(QLatin1Char('\n')));
        if (!result.missingFiles.isEmpty())
            step.details << QStringLiteral("Missing files will be recorded in the backup and do not prevent removal:") << result.missingFiles;
        step.details << QStringLiteral("The backup preserves the current files, including any damage. Use a complete trusted Integration Package source to install again.");
        step.artifacts << targetIntegration << result.removalBackupPath;
    });

    runStep([&](StepResult& step) {
        if (!result.integrationFound) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Nothing is installed, so no files were removed.");
            return;
        }
        if (!request.confirmRemoval) {
            result.requiresConfirmation = true;
            step.status = StepStatus::Warning;
            step.summary = QStringLiteral("Removal is ready and requires explicit confirmation.");
            step.remediation = QStringLiteral("Review the listed paths and backup location, then approve removal or cancel without changing the Unity project.");
            return;
        }
        QString error;
        const QString journalPath = UnityInstallation::removalJournalPath(result.unityProjectPath);
        const auto fail = [&]() {
            step.status = StepStatus::Failed;
            step.summary = UnityInstallation::hasPendingRemoval(result.unityProjectPath)
                ? QStringLiteral("Integration Package removal is unfinished. The verified backup and removal record have been kept.")
                : QStringLiteral("Integration Package removal stopped before changing project files.");
            step.cause = error;
            step.remediation = QStringLiteral("Resolve the reported file access or content changes, then use Uninstall Integration Package to retry.");
            if (result.removalBackupCreated) step.artifacts << result.removalBackupPath;
        };
        const auto unchanged = [&]() {
            QJsonObject current;
            if (!removalSnapshot(targetIntegration, &current, &error)) return false;
            if (current == reviewSnapshot) return true;
            error = QStringLiteral("Integration Package files changed after the safety check. Run the check again before continuing.");
            return false;
        };
        if (!UnityInstallation::validateTarget(result.unityProjectPath, targetIntegration, &error, true, resuming)
            || !checkUnityProjectClosed(result.unityProjectPath, &error) || !unchanged()) {
            fail();
            return;
        }
        const QString snapshotRoot = QDir(result.removalBackupPath).filePath(QStringLiteral("UnityProject/") + result.integrationInstallPath);
        if (!resuming) {
            if (QFileInfo::exists(result.removalBackupPath)) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("The planned backup folder already exists.");
                step.cause = result.removalBackupPath;
                step.remediation = QStringLiteral("Run the removal review again to create a new timestamped backup location.");
                return;
            }
            if (result.removalBackupPath.isEmpty()
                || !UnityInstallation::validatePath(result.removalBackupPath, &error)
                || UnityInstallation::containsPath(result.unityProjectPath, result.removalBackupPath)
                || UnityInstallation::containsPath(result.removalBackupPath, result.unityProjectPath)) {
                if (error.isEmpty()) error = QStringLiteral("Choose a new backup directory outside the Unity project.");
                fail();
                return;
            }
            for (const auto& directory : inventory.value(QStringLiteral("directories")).toArray()) {
                if (!QDir().mkpath(QDir(snapshotRoot).filePath(directory.toString()))) {
                    error = QStringLiteral("Cannot create directory: %1").arg(snapshotRoot);
                    fail();
                    return;
                }
            }
            const QString metadata = targetIntegration + QStringLiteral(".meta");
            const QString backupMeta = snapshotRoot + QStringLiteral(".meta");
            QJsonObject copied;
            if (!copyFiles(targetIntegration, snapshotRoot, installedFiles, &error)
                || !copyFiles(targetIntegration, QDir(result.removalBackupPath).filePath(QStringLiteral("ReintegrationSDK")), integrationBackupFiles, &error)
                || (!inventory.value(QStringLiteral("root_meta")).toString().isEmpty()
                    && !copyFileAtomically(metadata, backupMeta, &error))
                || !removalSnapshot(snapshotRoot, &copied, &error) || copied != inventory) {
                if (error.isEmpty()) error = QStringLiteral("Backup verification failed. No project files were changed.");
                fail();
                return;
            }
            const QByteArray record = QJsonDocument(journal).toJson(QJsonDocument::Indented);
            if (!VNH::writeIntegrationFile(QDir(result.removalBackupPath).filePath(QStringLiteral("backup.json")), record, &error)
                || !VNH::writeIntegrationFile(QDir(result.removalBackupPath).filePath(QStringLiteral("README-FIRST.txt")),
                    QByteArrayLiteral("This backup preserves the files present before removal, including modified or damaged files.\n"
                                      "Missing files are listed in backup.json. UnityProject preserves the installation and its ownership record.\n"
                                      "ReintegrationSDK contains the managed files that were present; it may be incomplete or damaged.\n"
                                      "To install again, select a complete trusted Integration Package source in Hub.\n"), &error)) {
                fail();
                return;
            }
            result.removalBackupCreated = true;
            if (!unchanged() || !checkUnityProjectClosed(result.unityProjectPath, &error)
                || !VNH::checkIntegrationFilesWritable(filesToChange, &error)
                || !UnityInstallation::validatePath(journalPath, &error)
                || !VNH::writeIntegrationFile(journalPath, record, &error)) {
                fail();
                return;
            }
        }
        step.summary = QStringLiteral("The safety backup is verified. Removing the approved files.");
        publish(nextStep);
        const QJsonObject hashes = inventory.value(QStringLiteral("files")).toObject();
        const auto removeVerified = [&](const QString& path, const QString& expected) {
            if (!UnityInstallation::validatePath(path, &error)) return false;
            if (!VNH::checkIntegrationFilesWritable({path}, &error)) return false;
            return VNH::removeVerifiedIntegrationFile(path, expected.toLatin1(), &error);
        };
        QStringList directories;
        for (const QString& relative : removalFiles) {
            if (relative == QLatin1String(kInstalledState)) continue;
            const QString path = QDir(targetIntegration).filePath(relative);
            const bool existed = QFileInfo::exists(path);
            if (!removeVerified(path, hashes.value(relative).toString())) { fail(); return; }
            if (existed) ++result.filesRemoved;
            QString parent = QFileInfo(relative).path();
            while (parent != QStringLiteral(".")) {
                directories << parent;
                parent = QFileInfo(parent).path();
            }
        }
        directories.removeDuplicates();
        std::sort(directories.begin(), directories.end(), [](const QString& left, const QString& right) {
            return left.size() > right.size();
        });
        for (const QString& relative : directories) QDir().rmdir(QDir(targetIntegration).filePath(relative));
        for (const QString& relative : removalFiles) {
            if (relative != QLatin1String(kInstalledState) && QFileInfo::exists(QDir(targetIntegration).filePath(relative))) {
                error = QStringLiteral("A file changed after backup and was preserved:") + QLatin1Char('\n') + relative;
                fail();
                return;
            }
        }
        const QString statePath = QDir(targetIntegration).filePath(QLatin1String(kInstalledState));
        const QJsonObject removedState{{QStringLiteral("schema_version"), 1},
            {QStringLiteral("package_name"), QLatin1String(kPackageName)},
            {QStringLiteral("installation_state"), QStringLiteral("removed")},
            {QStringLiteral("managed_files"), QJsonArray{}},
            {QStringLiteral("backup_path"), result.removalBackupPath}};
        const JsonRead currentState = readJsonObject(statePath);
        const QByteArray stateHash = currentState.object == removedState
            ? QCryptographicHash::hash(QJsonDocument(removedState).toJson(QJsonDocument::Indented), QCryptographicHash::Sha256).toHex()
            : hashes.value(QLatin1String(kInstalledState)).toString().toLatin1();
        if (!sha256File(statePath).matches(stateHash) && QFileInfo::exists(statePath)) {
            error = QStringLiteral("A file changed after backup and was preserved:") + QLatin1Char('\n') + statePath;
            fail();
            return;
        }
        if (!VNH::checkIntegrationFilesWritable({statePath}, &error)) { fail(); return; }
        QStringList remaining = QDir(targetIntegration).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        remaining.removeAll(QLatin1String(kInstalledState));
        if (remaining.isEmpty()) {
            if (!removeVerified(targetIntegration + QStringLiteral(".meta"), inventory.value(QStringLiteral("root_meta")).toString())
                || !removeVerified(statePath, QString::fromLatin1(stateHash))) {
                if (error.isEmpty()) error = QStringLiteral("Cannot remove file:") + QLatin1Char('\n') + statePath;
                fail();
                return;
            }
            if (QDir(targetIntegration).exists() && !QDir().rmdir(targetIntegration)) {
                error = QStringLiteral("The installation directory changed during removal. Its remaining files were preserved.");
                fail();
                return;
            }
        } else if (!VNH::writeIntegrationFile(statePath, QJsonDocument(removedState).toJson(QJsonDocument::Indented), &error)) {
            fail();
            return;
        }
        preservedFiles = allRelativeFiles(targetIntegration);
        preservedFiles.removeAll(QLatin1String(kInstalledState));
        result.targetOnlyFilesPreserved = preservedFiles.size();
        if (!QFile::remove(journalPath)) {
            error = QStringLiteral("The removal record could not be cleared. Retry Uninstall Integration Package to finish.");
            fail();
            return;
        }
        result.integrationRemoved = true;
        result.targetInstalled = false;
        result.targetCurrent = false;
        step.summary = QStringLiteral("Backed up and removed the Harken Integration Package and native runtimes; preserved %1 project-specific file(s).")
            .arg(result.targetOnlyFilesPreserved);
        if (!preservedFiles.isEmpty()) {
            step.details << QStringLiteral("The Integration Package is removed. Remaining project files are preserved and can be used with a new installation.") << preservedFiles;
        }
        step.artifacts << result.removalBackupPath;
    });

    runStep([&](StepResult& step) {
        if (request.unityProjectPath.trimmed().isEmpty()) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("No removal report was written because the Unity project was not valid.");
            return;
        }
        const QString reportDir = integrationReportDirectory(result.unityProjectPath, &step.cause);
        if (reportDir.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not prepare the Hub report directory.");
            return;
        }
        result.jsonReportPath = QDir(reportDir).filePath(
            QStringLiteral("latest-removal.json"));
        result.textReportPath = QDir(reportDir).filePath(
            QStringLiteral("latest-removal.txt"));

        QJsonObject root;
        root[QStringLiteral("schema_version")] = 1;
        root[QStringLiteral("operation")] = QStringLiteral("remove_integration");
        root[QStringLiteral("generated_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        root[QStringLiteral("success")] = !blocked && !result.requiresConfirmation;
        root[QStringLiteral("requires_confirmation")] = result.requiresConfirmation;
        root[QStringLiteral("integration_found")] = result.integrationFound;
        root[QStringLiteral("integration_removed")] = result.integrationRemoved;
        root[QStringLiteral("removal_backup_created")] =
            result.removalBackupCreated;
        root[QStringLiteral("unity_project_path")] = result.unityProjectPath;
        root[QStringLiteral("sdk_install_path")] = result.integrationInstallPath;
        root[QStringLiteral("removal_backup_path")] = result.removalBackupPath;
        root[QStringLiteral("files_removed")] = result.filesRemoved;
        root[QStringLiteral("project_files_preserved")] =
            result.targetOnlyFilesPreserved;
        QJsonArray steps;
        for (const StepResult& item : result.steps) steps.append(stepToJson(item));
        root[QStringLiteral("steps")] = steps;

        QString jsonFileError;
        if (!writeIntegrationReport(result.jsonReportPath,
                QJsonDocument(root).toJson(QJsonDocument::Indented), &jsonFileError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The removal report could not be written.");
            step.cause = jsonFileError;
            step.remediation = QStringLiteral("Check write permission for the Hub report directory: %1").arg(reportDir);
            return;
        }

        QString reportText = QStringLiteral("Harken Unity integration removal report\n\n");
        reportText += QStringLiteral("Generated: %1\nUnity project: %2\nSafety backup: %3\nIntegration removed: %4\nHarken files removed: %5\nProject-specific files preserved: %6\n\n")
                        .arg(root[QStringLiteral("generated_at_utc")].toString(),
                             result.unityProjectPath,
                             result.removalBackupPath,
                             result.integrationRemoved ? QStringLiteral("yes")
                                                       : QStringLiteral("no"))
                        .arg(result.filesRemoved)
                        .arg(result.targetOnlyFilesPreserved);
        for (const StepResult& item : result.steps) {
            reportText += QStringLiteral("%1 - %2\n\n%3\n\n")
                .arg(item.title, item.status == StepStatus::Warning ? QStringLiteral("review recommended") : statusName(item.status), item.summary);
            if (!item.cause.isEmpty()) reportText += QStringLiteral("Cause: %1\n\n").arg(item.cause);
            if (!item.remediation.isEmpty()) reportText += QStringLiteral("Action: %1\n\n").arg(item.remediation);
            for (const QString& detail : item.details) reportText += detail + QStringLiteral("\n\n");
        }
        QString textFileError;
        if (!writeIntegrationReport(result.textReportPath,
                reportText.toUtf8(), &textFileError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("JSON report was written, but the text removal report failed.");
            step.cause = textFileError;
            step.remediation = QStringLiteral("Check write permission for the Hub report directory: %1").arg(reportDir);
            return;
        }
        step.summary = QStringLiteral("Wrote machine-readable and human-readable removal reports.");
        step.artifacts << result.jsonReportPath << result.textReportPath;
    }, true);

    while (nextStep < result.steps.size()) runStep([](StepResult&) {});
    result.success = !blocked && !result.requiresConfirmation;
    for (const StepResult& step : result.steps) {
        if (step.status == StepStatus::Failed) result.success = false;
        if (step.status == StepStatus::Warning) result.hasWarnings = true;
    }
    return result;
}

UnityIntegrationService::Result UnityIntegrationService::restoreIntegration(const Request& request)
{
    Result result;
    result.unityProjectPath = cleanAbsolutePath(request.unityProjectPath);
    result.integrationSourcePath = cleanAbsolutePath(request.integrationSourcePath);
    StepResult step;
    step.id = QStringLiteral("restore");
    step.title = QStringLiteral("Restore current Integration Package");
    QString error;
    const auto finish = [&](bool success, const QString& summary) {
        result.success = success;
        step.status = success ? StepStatus::Succeeded : StepStatus::Failed;
        step.summary = summary;
        step.cause = error;
        if (!result.restorationBackupPath.isEmpty()) step.artifacts << result.restorationBackupPath;
        result.steps.push_back(step);
        return result;
    };
    const QString journalPath = UnityInstallation::restoreJournalPath(result.unityProjectPath);
    for (const auto& marker : {QStringLiteral("Assets"), QStringLiteral("Packages"), QStringLiteral("ProjectSettings")})
        if (!QDir(result.unityProjectPath).exists(marker))
            return finish(false, QStringLiteral("Selected directory is not a Unity project root."));
    if (!UnityInstallation::validatePath(journalPath, &error))
        return finish(false, QStringLiteral("The restoration record cannot be accessed safely."));
    const auto pending = readJsonObject(journalPath);
    Request preparedRequest = request;
    if (request.recoverRestore) {
        if (pending.status != ReadStatus::Ok)
            return finish(false, QStringLiteral("The unfinished restoration record is missing or invalid."));
        preparedRequest.integrationInstallPath = pending.object.value(QStringLiteral("install_path")).toString();
    } else if (pending.status != ReadStatus::Missing) {
        return finish(false, QStringLiteral("Recover the unfinished Integration Package restoration before continuing."));
    }
    QString target;
    std::unique_ptr<QLockFile> lock;
    if (!prepareInstallation(preparedRequest, result, &target, &lock, &error)
        || !checkUnityProjectClosed(result.unityProjectPath, &error))
        return finish(false, QStringLiteral("The Integration Package cannot be restored safely."));
    if (request.recoverRestore) {
        const auto record = pending.object;
        const QString token = record.value(QStringLiteral("token")).toString();
        const QString work = QDir(result.unityProjectPath).filePath(QStringLiteral("Library/HarkenIntegration/Restore/") + token);
        const QString original = QDir(work).filePath(QStringLiteral("original"));
        const QString backup = record.value(QStringLiteral("backup_path")).toString();
        result.restorationBackupPath = backup;
        if (QUuid(token).isNull() || QUuid(token).toString(QUuid::WithoutBraces) != token
            || record.value(QStringLiteral("schema_version")).toInt() != 1
            || record.value(QStringLiteral("project")).toString() != result.unityProjectPath
            || backup.isEmpty() || UnityInstallation::containsPath(result.unityProjectPath, backup)
            || UnityInstallation::containsPath(backup, result.unityProjectPath)
            || !UnityInstallation::validateTree(work, &error)
            || !UnityInstallation::validateTree(backup, &error)
            || readJsonObject(QDir(backup).filePath(QStringLiteral("backup.json"))).object != record)
            return finish(false, QStringLiteral("The restoration record or safety backup is inconsistent. No files were changed."));
        const auto before = record.value(QStringLiteral("before")).toObject();
        const auto after = record.value(QStringLiteral("after")).toObject();
        QJsonObject backupSnapshot;
        if (before.isEmpty() || after.isEmpty()
            || !integrationSnapshot(QDir(backup).filePath(QStringLiteral("IntegrationPackage")), &backupSnapshot, &error)
            || backupSnapshot != before
            || QString::fromLatin1(sha256File(QDir(backup).filePath(QStringLiteral("IntegrationPackage.meta"))).hash)
                != record.value(QStringLiteral("root_meta")).toString())
            return finish(false, QStringLiteral("The safety backup failed verification. No files were changed."));
        QJsonObject current;
        QJsonObject originalSnapshot;
        const bool targetExists = QFileInfo::exists(target);
        const bool originalExists = QFileInfo::exists(original);
        if ((targetExists && !integrationSnapshot(target, &current, &error))
            || (originalExists && !integrationSnapshot(original, &originalSnapshot, &error)))
            return finish(false, QStringLiteral("The interrupted restoration could not be inspected."));
        const bool activated = targetExists && current == after && originalExists && originalSnapshot == before;
        const bool untouched = targetExists && current == before && !originalExists;
        const bool displaced = !targetExists && originalExists && originalSnapshot == before;
        if (!activated && !untouched && !displaced)
            return finish(false, QStringLiteral("Files changed after the restoration stopped. Preserve the listed folders and resolve the conflict before recovery."));
        result.planHash = QCryptographicHash::hash(QJsonDocument(record).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
        step.artifacts << target << work;
        if (request.checkOnly)
            return finish(true, QStringLiteral("The interrupted restoration is ready to recover. Existing files will be preserved."));
        if (!request.confirmTargetChanges || request.approvedPlanHash != result.planHash) {
            result.requiresConfirmation = true;
            return finish(false, QStringLiteral("Review and confirm the restoration recovery plan."));
        }
        if (displaced && (!VNH::checkIntegrationDirectoryChanges({target}, {original}, &error)
            || !VNH::renameIntegrationDirectory(original, target, &error)))
            return finish(false, QStringLiteral("The original Integration Package could not be put back. Its backup is retained."));
        if (!QFile::remove(journalPath))
            return finish(false, QStringLiteral("Recovery finished, but the restoration record could not be cleared."));
        result.targetInstalled = true;
        result.targetCurrent = activated;
        return finish(true, activated ? QStringLiteral("The restored Integration Package was verified. The original files remain backed up.")
            : QStringLiteral("The original Integration Package is in place. Run Restore Current Version again to prepare a new plan."));
    }
    const auto ownership = previousManagedFiles(target);
    const auto receipt = readJsonObject(QDir(target).filePath(QLatin1String(kInstalledState))).object;
    const QString version = receipt.value(QStringLiteral("package_version")).toString();
    if (ownership.status != ReadStatus::Ok || ownership.managedFiles.isEmpty() || version.isEmpty()
        || packageVersion(result.integrationSourcePath) != version || request.officialFileHashes.isEmpty()
        || request.officialRelease.value(QStringLiteral("buildId")).toString().isEmpty()
        || !UnityInstallation::validateTree(result.integrationSourcePath, &error)
        || UnityInstallation::containsPath(target, result.integrationSourcePath)
        || UnityInstallation::containsPath(result.integrationSourcePath, target))
        return finish(false, QStringLiteral("The installed version or verified official source could not be established. No files were changed."));
    const auto previousRelease = receipt.value(QStringLiteral("source_release")).toObject();
    if (!previousRelease.value(QStringLiteral("buildId")).toString().isEmpty()
        && previousRelease.value(QStringLiteral("buildId")) != request.officialRelease.value(QStringLiteral("buildId")))
        return finish(false, QStringLiteral("The selected release is a different build from the installed Integration Package."));
    QJsonObject sourceSnapshot;
    QJsonObject before;
    if (!integrationSnapshot(result.integrationSourcePath, &sourceSnapshot, &error) || !integrationSnapshot(target, &before, &error))
        return finish(false, QStringLiteral("The Integration Package files could not be inspected."));
    QJsonObject expectedFiles;
    QMap<QString, QByteArray> textHashes;
    for (auto it = request.officialFileHashes.cbegin(); it != request.officialFileHashes.cend(); ++it) {
        if (!isSafeRelativePath(it.key()) || it.value().size() != 64 || it.key() == QLatin1String(kInstalledState))
            return finish(false, QStringLiteral("The official file inventory is invalid."));
        expectedFiles[it.key()] = QString::fromLatin1(it.value());
        const auto textHash = UnityInstallation::normalizedTextHash(QDir(result.integrationSourcePath).filePath(it.key()));
        if (!textHash.isEmpty()) textHashes.insert(it.key(), textHash);
    }
    if (sourceSnapshot.value(QStringLiteral("files")).toObject() != expectedFiles)
        return finish(false, QStringLiteral("The official source changed after verification. No project files were changed."));
    const auto recordedHashes = receipt.value(QStringLiteral("sha256")).toObject();
    const auto recordedText = receipt.value(QStringLiteral("text_lf_sha256")).toObject();
    for (const auto& file : ownership.managedFiles) {
        if (!expectedFiles.contains(file)
            || (recordedHashes.value(file).toString().compare(expectedFiles.value(file).toString(), Qt::CaseInsensitive) != 0
                && (textHashes.value(file).isEmpty() || recordedText.value(file).toString().toLatin1() != textHashes.value(file)))) {
            error = file;
            return finish(false, QStringLiteral("The installation record does not identify this official package as the original version. No files were changed."));
        }
    }
    const auto existingFiles = before.value(QStringLiteral("files")).toObject();
    const auto rootMetadata = sha256File(target + QStringLiteral(".meta"));
    if (rootMetadata.unreadable())
        return finish(false, QStringLiteral("Could not read Unity metadata. No project files were changed."));
    QStringList conflicts;
    QStringList changedPaths;
    for (auto it = expectedFiles.begin(); it != expectedFiles.end(); ++it) {
        if (existingFiles.value(it.key()) == it.value()) continue;
        const bool exists = existingFiles.contains(it.key());
        result.fileChanges.push_back({exists ? QStringLiteral("replace") : QStringLiteral("add"), it.key()});
        if (exists) ++result.filesChanged;
        else ++result.filesAdded;
        if (exists && !ownership.managedFiles.contains(it.key())) conflicts << it.key();
        changedPaths << QDir(target).filePath(it.key());
    }
    for (auto it = existingFiles.begin(); it != existingFiles.end(); ++it) {
        if (it.key() != QLatin1String(kInstalledState) && !expectedFiles.contains(it.key())) {
            ++result.targetOnlyFilesPreserved;
            result.fileChanges.push_back({QStringLiteral("preserve"), it.key()});
        }
    }
    result.changesDetected = result.filesAdded + result.filesChanged > 0;
    result.conflictingFiles = conflicts;
    result.targetInstalled = true;
    if (!result.changesDetected) {
        result.targetAlreadyCurrent = result.targetCurrent = true;
        return finish(true, QStringLiteral("All managed files already match the official package. No files were changed."));
    }
    result.restorationBackupPath = request.restorationBackupPath.isEmpty()
        ? defaultIntegrationBackupPath(result.unityProjectPath) : cleanAbsolutePath(request.restorationBackupPath);
    const QString backup = result.restorationBackupPath;
    if (backup.isEmpty() || QFileInfo::exists(backup)
        || UnityInstallation::containsPath(result.unityProjectPath, backup)
        || UnityInstallation::containsPath(backup, result.unityProjectPath)
        || !UnityInstallation::validatePath(backup, &error))
        return finish(false, QStringLiteral("The safety backup location is invalid or already exists."));
    result.planHash = QCryptographicHash::hash(QJsonDocument(QJsonObject{
        {QStringLiteral("project"), result.unityProjectPath}, {QStringLiteral("target"), target},
        {QStringLiteral("before"), before}, {QStringLiteral("official"), request.officialRelease},
        {QStringLiteral("root_meta"), QString::fromLatin1(rootMetadata.hash)},
        {QStringLiteral("files"), expectedFiles}, {QStringLiteral("backup"), backup}}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    step.details = describeFileChanges(result.fileChanges, result.integrationInstallPath);
    if (!conflicts.isEmpty())
        step.details << QStringLiteral("These unowned files conflict with official package paths and will be backed up before replacement:") << conflicts;
    changedPaths << QDir(target).filePath(QLatin1String(kInstalledState));
    if (!VNH::checkIntegrationFilesWritable(changedPaths, &error)
        || !VNH::checkIntegrationDirectoryChanges({target}, {target}, &error))
        return finish(false, QStringLiteral("Checkout or unlock the listed files before restoration. No files were changed."));
    if (request.checkOnly)
        return finish(true, QStringLiteral("Review the official file restoration plan. A permanent backup will be created first."));
    if (!request.confirmTargetChanges || request.approvedPlanHash != result.planHash) {
        result.requiresConfirmation = true;
        return finish(false, QStringLiteral("The files or restoration plan changed. Review and confirm again."));
    }
    if (!conflicts.isEmpty() && !request.confirmUnownedConflicts) {
        result.requiresConfirmation = true;
        return finish(false, QStringLiteral("Conflicting unowned files require separate approval before restoration."));
    }
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString work = QDir(result.unityProjectPath).filePath(QStringLiteral("Library/HarkenIntegration/Restore/") + token);
    const QString staged = QDir(work).filePath(QStringLiteral("staged"));
    const QString original = QDir(work).filePath(QStringLiteral("original"));
    const QString backupPackage = QDir(backup).filePath(QStringLiteral("IntegrationPackage"));
    if (!UnityInstallation::validatePath(work, &error) || !QDir().mkpath(staged) || !QDir().mkpath(backupPackage))
        return finish(false, QStringLiteral("Could not create restoration staging and backup folders."));
    step.artifacts << work;
    for (const auto& directory : before.value(QStringLiteral("directories")).toArray()) {
        if (!QDir().mkpath(QDir(backupPackage).filePath(directory.toString()))
            || !QDir().mkpath(QDir(staged).filePath(directory.toString())))
            return finish(false, QStringLiteral("Could not preserve existing directories. No files were changed."));
    }
    if (!copyFiles(target, backupPackage, existingFiles.keys(), &error)
        || !copyFiles(target, staged, existingFiles.keys(), &error)
        || !copyFiles(result.integrationSourcePath, staged, expectedFiles.keys(), &error)
        || !writeInstalledState(staged, version, expectedFiles.keys(), request.officialFileHashes, textHashes, &error, request.officialRelease))
        return finish(false, QStringLiteral("Could not stage or back up the restoration. No project files were changed."));
    if (!copyFilePermissions(target, backupPackage, existingFiles.keys(), &error)
        || !copyFilePermissions(target, staged, existingFiles.keys(), &error))
        return finish(false, QStringLiteral("Could not preserve existing file permissions. No project files were changed."));
    const QString rootMeta = target + QStringLiteral(".meta");
    if (QFileInfo::exists(rootMeta) && !copyFileAtomically(rootMeta, backupPackage + QStringLiteral(".meta"), &error))
        return finish(false, QStringLiteral("Could not back up Unity metadata. No project files were changed."));
    QJsonObject backedUp;
    QJsonObject current;
    QJsonObject after;
    if (!integrationSnapshot(backupPackage, &backedUp, &error) || backedUp != before
        || !integrationSnapshot(target, &current, &error) || current != before
        || sha256File(backupPackage + QStringLiteral(".meta")).hash != rootMetadata.hash
        || sha256File(rootMeta).hash != rootMetadata.hash
        || !integrationSnapshot(staged, &after, &error))
        return finish(false, QStringLiteral("Files changed while preparing the backup. No project files were changed."));
    const auto afterFiles = after.value(QStringLiteral("files")).toObject();
    auto intendedFiles = existingFiles;
    for (auto it = expectedFiles.begin(); it != expectedFiles.end(); ++it) intendedFiles[it.key()] = it.value();
    intendedFiles.remove(QLatin1String(kInstalledState));
    auto stagedFiles = afterFiles;
    stagedFiles.remove(QLatin1String(kInstalledState));
    if (intendedFiles != stagedFiles)
        return finish(false, QStringLiteral("Staged files do not match the restoration plan. No project files were changed."));
    const QJsonObject record{{QStringLiteral("schema_version"), 1}, {QStringLiteral("project"), result.unityProjectPath},
        {QStringLiteral("install_path"), result.integrationInstallPath}, {QStringLiteral("token"), token},
        {QStringLiteral("backup_path"), backup}, {QStringLiteral("before"), before}, {QStringLiteral("after"), after},
        {QStringLiteral("root_meta"), QString::fromLatin1(rootMetadata.hash)}, {QStringLiteral("official_release"), request.officialRelease}};
    const auto journalBytes = QJsonDocument(record).toJson(QJsonDocument::Indented);
    if (!VNH::writeIntegrationFile(QDir(backup).filePath(QStringLiteral("backup.json")), journalBytes, &error)
        || !VNH::writeIntegrationFile(journalPath, journalBytes, &error))
        return finish(false, QStringLiteral("Could not save the restoration recovery record. No project files were changed."));
    if (!checkUnityProjectClosed(result.unityProjectPath, &error)
        || !VNH::checkIntegrationFilesWritable(changedPaths, &error)
        || !VNH::checkIntegrationDirectoryChanges({target, original}, {target, staged}, &error)
        || !VNH::renameIntegrationDirectory(target, original, &error))
        return finish(false, QStringLiteral("Restoration stopped before activation. Use Recover Restoration to reconcile the saved operation."));
    if (!integrationSnapshot(original, &current, &error) || current != before
        || !VNH::renameIntegrationDirectory(staged, target, &error)) {
        if (!QFileInfo::exists(target)) VNH::renameIntegrationDirectory(original, target, &error);
        return finish(false, QStringLiteral("The staged package could not be activated. All original files and recovery records were retained."));
    }
    if (!integrationSnapshot(target, &current, &error) || current != after)
        return finish(false, QStringLiteral("Restored files failed verification. Preserve the backup and use Recover Restoration."));
    if (!QFile::remove(journalPath))
        return finish(false, QStringLiteral("Restoration finished, but the recovery record could not be cleared."));
    result.targetCurrent = true;
    result.integrationFilesCopied = expectedFiles.size();
    return finish(true, QStringLiteral("The current Integration Package was restored from verified official files. Custom files and safety backups were preserved."));
}

UnityIntegrationService::Result UnityIntegrationService::run(const Request& request)
{
    if (request.restoreIntegration || request.recoverRestore) return restoreIntegration(request);
    if (request.migrateIntegration || request.recoverMigration) return migrateIntegration(request);
    if (request.removeIntegration) return removeIntegration(request);

    Result result;
    result.unityProjectPath = cleanAbsolutePath(request.unityProjectPath);
    result.steps.reserve(static_cast<int>(std::size(kStepSpecs)));
    for (const StepSpec& spec : kStepSpecs) {
        StepResult step;
        step.id = QString::fromLatin1(spec.id);
        step.title = QString::fromLatin1(spec.title);
        result.steps.push_back(step);
    }

    int nextStep = 0;
    bool blocked = false;
    QString blockingTitle;
    QString targetIntegration;
    std::unique_ptr<QLockFile> operationLock;
    QString sourceVersion;
    QStringList sourceFiles;
    QStringList preservedFiles;
    QStringList textFormatPreservedFiles;
    QStringList staleFiles;
    QStringList filesToChange;
    QMap<QString, QString> runtimeMetadataMoves;
    QMap<QString, QByteArray> sourceHashes;
    QMap<QString, QByteArray> sourceTextHashes;
    QMap<QString, QByteArray> installedContentHashes;
    QSet<QString> declaredOptionalRuntimePaths;
    QString updateBackupRoot;
    bool installationActivated = false;
    bool retainedTarget = false;
    bool retainedFilesOverlap = false;
    QJsonObject retainedSnapshot;

    auto publish = [&](int index) { emit stepUpdated(index, result.steps[index]); };
    auto runStep = [&](const std::function<void(StepResult&)>& body,
                       bool runWhenBlocked = false) {
        StepResult& step = result.steps[nextStep];
        if (blocked && !runWhenBlocked) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Not run because %1 failed.").arg(blockingTitle);
            step.cause = QStringLiteral("A required earlier step did not complete.");
            step.remediation = QStringLiteral("Fix the first failed step, then run integration again.");
            publish(nextStep++);
            return;
        }
        QElapsedTimer timer;
        timer.start();
        step.status = StepStatus::Running;
        publish(nextStep);
        body(step);
        step.elapsedMs = timer.elapsed();
        if (step.status == StepStatus::Running) step.status = StepStatus::Succeeded;
        if (step.status == StepStatus::Warning) result.hasWarnings = true;
        if (step.status == StepStatus::Failed) {
            blocked = true;
            blockingTitle = step.title;
        }
        publish(nextStep++);
    };

    runStep([&](StepResult& step) {
        const QString root = result.unityProjectPath;
        if (request.unityProjectPath.trimmed().isEmpty() || !QDir(root).exists()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Unity project directory does not exist.");
            step.cause = request.unityProjectPath.trimmed().isEmpty()
                ? QStringLiteral("No Unity project path was selected.")
                : QStringLiteral("The selected path cannot be found: %1").arg(root);
            step.remediation = QStringLiteral("Select the Unity project root containing Assets, Packages, and ProjectSettings.");
            return;
        }
        const QStringList markers = {QStringLiteral("Assets"), QStringLiteral("Packages"),
                                     QStringLiteral("ProjectSettings")};
        QStringList missing;
        for (const QString& marker : markers) {
            if (!QDir(QDir(root).filePath(marker)).exists()) missing << marker;
        }
        if (!missing.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Selected directory is not a Unity project root.");
            step.cause = QStringLiteral("Missing required directories: %1")
                             .arg(missing.join(QStringLiteral(", ")));
            step.remediation = QStringLiteral("Select the parent directory that directly contains Assets, Packages, and ProjectSettings.");
            return;
        }
        QString error;
        if (!prepareInstallation(request, result, &targetIntegration, &operationLock, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package installation location is not safe to use.");
            step.cause = error;
            step.remediation = QStringLiteral("Resolve the reported path or Integration Package conflict, then check again.");
            return;
        }
        step.summary = QStringLiteral("Unity project layout and write access are valid.");
        step.artifacts << root;
    });

    runStep([&](StepResult& step) {
        result.integrationSourcePath = locateIntegrationSource(request.integrationSourcePath);
        if (result.integrationSourcePath.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Harken Unity Integration Package could not be located.");
            if (!request.integrationSourcePath.trimmed().isEmpty()) {
                step.cause = QStringLiteral("The selected folder is not a valid com.harken.core Integration Package source: %1")
                                 .arg(cleanAbsolutePath(request.integrationSourcePath));
                step.remediation = QStringLiteral("Choose HarkenIntegration, HarkenIntegration/Unity/Harken, or a complete external Unity package root, or clear the Integration Package source field to use automatic discovery.");
            } else {
                step.cause = QStringLiteral("No valid com.harken.core package was found beside Harken Studio, in a repository parent, or at HARKEN_UNITY_INTEGRATION_PATH (legacy: HARKEN_UNITY_SDK_PATH).");
                step.remediation = QStringLiteral("Use the Integration Package source field to choose or drop HarkenIntegration, Unity/Harken, or a complete external Unity package root.");
            }
            return;
        }

        const QString sourceManifestPath = QDir(result.integrationSourcePath)
                                               .filePath(QLatin1String(kIntegrationManifest));
        QString safetyError;
        if (!UnityInstallation::validateTree(result.integrationSourcePath, &safetyError)
            || UnityInstallation::containsPath(result.integrationSourcePath, targetIntegration)
            || UnityInstallation::containsPath(targetIntegration, result.integrationSourcePath)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package source and installation location are not safe to use.");
            step.cause = safetyError.isEmpty()
                ? QStringLiteral("Integration Package source and target must be separate directories without containment.") : safetyError;
            return;
        }
        const JsonRead manifestRead = readJsonObject(sourceManifestPath);
        if (manifestRead.unreadable()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The selected Integration Package installation manifest could not be read.");
            step.cause = QStringLiteral("%1 exists but cannot be opened.")
                             .arg(sourceManifestPath);
            step.remediation = QStringLiteral("Close any process holding this file and check the Integration Package folder permissions, then run Check only again.");
            return;
        }
        const QJsonObject manifest = manifestRead.object;
        if (result.integrationInstallPath != UnityInstallation::defaultRelativePath()
            && result.integrationInstallPath != QStringLiteral("Assets/HarkenSDK")
            && !manifest.value(QStringLiteral("supports_custom_install_path")).toBool()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("This Integration Package version does not support a custom installation location.");
            step.remediation = QStringLiteral("Choose a newer Integration Package or install to Packages/com.harken.core or Assets/HarkenSDK.");
            return;
        }
        if (manifest.value(QStringLiteral("schema_version")).toInt() != 3 ||
            manifest.value(QStringLiteral("package_name")).toString() !=
                QLatin1String(kPackageName)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The selected Integration Package installation manifest is missing or invalid.");
            step.cause = QStringLiteral("%1 does not declare the expected schema and package identity.")
                             .arg(QDir(result.integrationSourcePath).filePath(QLatin1String(kIntegrationManifest)));
            step.remediation = QStringLiteral("Choose a complete HarkenIntegration/Unity/Harken package, or repair the selected Integration Package source before integrating.");
            return;
        }

        QStringList missing;
        QStringList missingOptionalSourceFiles;
        const QJsonArray required = manifest.value(QStringLiteral("required_files")).toArray();
        for (const QJsonValue& value : required) {
            const QString relative = value.toString();
            if (!isSafeRelativePath(relative)) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("The Integration Package manifest contains an unsafe file path.");
                step.cause = relative;
                return;
            }
            if (relative.isEmpty() ||
                !existingArtifact(QDir(result.integrationSourcePath).filePath(relative))) {
                if (UnityInstallation::isDocumentationOrSample(relative))
                    missingOptionalSourceFiles << relative;
                else
                    missing << relative;
            }
        }

        const QString hostPlatform = hostPlatformName();
        const QJsonObject expectedByHost =
            manifest.value(QStringLiteral("expected_platforms_by_editor_host")).toObject();
        QSet<QString> expectedPlatforms;
        for (const QJsonValue& value : expectedByHost.value(hostPlatform).toArray()) {
            const QString platform = value.toString();
            if (!platform.isEmpty()) expectedPlatforms.insert(platform);
        }
        QStringList bundledPlatforms;
        QStringList optionalMissingPlatforms;
        QStringList missingExpectedPlatforms;
        QStringList missingExpectedRuntimeDetails;
        QStringList missingBlockingPlatforms;
        QStringList missingBlockingRuntimeDetails;
        QSet<QString> declaredPlatforms;
        const QJsonArray plugins = manifest.value(QStringLiteral("native_plugins")).toArray();
        for (const QJsonValue& value : plugins) {
            const QJsonObject plugin = value.toObject();
            const QString platform = plugin.value(QStringLiteral("platform")).toString();
            const bool isExpectedPlatform = expectedPlatforms.contains(platform);
            const bool isHostPlatform = platform == hostPlatform;
            if (!platform.isEmpty()) declaredPlatforms.insert(platform);
            bool found = false;
            QStringList alternatives;
            for (const QJsonValue& alternative :
                 plugin.value(QStringLiteral("alternatives")).toArray()) {
                const QString relative = alternative.toString();
                if (!isSafeRelativePath(relative)) {
                    step.status = StepStatus::Failed;
                    step.summary = QStringLiteral("The Integration Package manifest contains an unsafe runtime path.");
                    step.cause = relative;
                    return;
                }
                alternatives << relative;
                if (!isHostPlatform && !relative.isEmpty())
                    declaredOptionalRuntimePaths.insert(relative);
                if (existingArtifact(QDir(result.integrationSourcePath).filePath(relative)))
                    found = true;
            }
            if (found) {
                bundledPlatforms << platform;
            } else if (isHostPlatform) {
                missingBlockingPlatforms << platform;
                missing << QStringLiteral("%1 native runtime (%2)")
                               .arg(platform, alternatives.join(QStringLiteral(" or ")));
                missingBlockingRuntimeDetails << missingRuntimeDetail(
                    result.integrationSourcePath, platform, alternatives);
            } else if (isExpectedPlatform) {
                missingExpectedPlatforms << platform;
                missingExpectedRuntimeDetails << missingRuntimeDetail(
                    result.integrationSourcePath, platform, alternatives);
            } else {
                optionalMissingPlatforms << platform;
            }
        }
        if (plugins.isEmpty()) missing << QStringLiteral("native platform declarations");
        if (hostPlatform.isEmpty()) {
            missing << QStringLiteral("supported editor host platform identity");
        } else if (expectedPlatforms.isEmpty()) {
            missing << QStringLiteral("expected Unity target policy for %1").arg(hostPlatform);
        }
        for (const QString& platform : expectedPlatforms) {
            if (!declaredPlatforms.contains(platform))
                missing << QStringLiteral("%1 native runtime declaration").arg(platform);
        }

        sourceFiles = relativeFiles(result.integrationSourcePath);
        if (sourceFiles.isEmpty()) missing << QStringLiteral("Integration Package file inventory");
        if (!missing.isEmpty()) {
            step.status = StepStatus::Failed;
            if (!missingBlockingPlatforms.isEmpty()) {
                step.summary = QStringLiteral("The selected Integration Package source is missing the verified game runtime for: %1. The Unity project was not changed.")
                                   .arg(missingBlockingPlatforms.join(QStringLiteral(", ")));
                step.cause = QStringLiteral("These runtime files must come from the selected Integration Package source, not from the Unity project. Integration stopped before copying anything because the Harken integration cannot verify an Integration Package assembled from incomplete source files.");
                step.remediation = QStringLiteral("Choose or drop HarkenIntegration, HarkenIntegration/Unity/Harken, or a complete external Unity package. If this is the Integration Package bundled with Harken Studio, repair or reinstall the editor package first. Do not copy unknown runtime files from an existing Unity project.");
                step.details = missingBlockingRuntimeDetails;
                if (missing.size() > missingBlockingPlatforms.size()) {
                    step.details << QStringLiteral("Other missing Harken Integration Package content: %1")
                                        .arg(missing.join(QStringLiteral("; ")));
                }
            } else {
                step.summary = QStringLiteral("The selected Unity Integration Package source is incomplete. The Unity project was not changed.");
                step.cause = QStringLiteral("Required Harken Integration Package content is missing from the selected source: %1")
                                 .arg(missing.join(QStringLiteral("; ")));
                step.remediation = QStringLiteral("Choose or drop a complete Unity/Harken package, then run Check only again. This problem cannot be repaired inside the Unity project.");
                step.details = missing;
            }
            return;
        }

        for (const QString& relative : sourceFiles) {
            const FileDigest digest = sha256File(
                QDir(result.integrationSourcePath).filePath(relative));
            if (digest.status != ReadStatus::Ok) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("A selected Integration Package source file could not be read.");
                step.cause = QDir(result.integrationSourcePath).filePath(relative);
                step.remediation = QStringLiteral("Check the selected Integration Package folder permissions and contents, then retry.");
                return;
            }
            sourceHashes.insert(relative, digest.hash);
            const QByteArray textHash = UnityInstallation::normalizedTextHash(
                QDir(result.integrationSourcePath).filePath(relative));
            if (!textHash.isEmpty()) sourceTextHashes.insert(relative, textHash);
        }
        sourceVersion = packageVersion(result.integrationSourcePath);
        installedContentHashes = sourceHashes;
        step.summary = QStringLiteral("Validated Integration Package %1: %2 file(s); the %3 runtime required for this editor is present.")
                           .arg(sourceVersion, QString::number(sourceFiles.size()),
                                hostPlatform);
        step.details << QStringLiteral("Native runtimes available in this Integration Package source: %1")
                            .arg(bundledPlatforms.join(QStringLiteral(", ")));
        if (!missingOptionalSourceFiles.isEmpty())
            step.details << QStringLiteral("Missing documentation/sample files (does not block Integration Package updates):")
                         << missingOptionalSourceFiles.join(QLatin1Char('\n'));
        if (!missingExpectedPlatforms.isEmpty()) {
            step.status = StepStatus::Warning;
            step.summary += QStringLiteral(" Target runtimes not included: %1. Available platforms can still be integrated.")
                                .arg(missingExpectedPlatforms.join(QStringLiteral(", ")));
            step.cause = QStringLiteral("The %1 files in the selected Integration Package source are complete, but the listed target platforms have no verified runtime in this source. Each target is independent, so their absence does not prevent %1 integration.")
                             .arg(hostPlatform);
            step.remediation = QStringLiteral("You may continue integrating and use the available platforms. Before building for a missing target, select a complete Integration Package source that includes its verified runtime, then run Check only again. Do not copy unknown runtime files from another Unity project.");
            step.details << missingExpectedRuntimeDetails;
        }
        if (!optionalMissingPlatforms.isEmpty()) {
            step.status = StepStatus::Warning;
            step.summary += QStringLiteral(" Runtimes that require another editor operating system are not included: %1.")
                                .arg(optionalMissingPlatforms.join(QStringLiteral(", ")));
            if (step.cause.isEmpty()) {
                step.cause = QStringLiteral("Those targets cannot be completed on %1 under the Harken integration policy, so their missing runtimes do not block this integration.")
                                 .arg(hostPlatform);
            }
            if (!step.remediation.isEmpty()) step.remediation += QLatin1Char('\n');
            step.remediation += QStringLiteral("Before building targets that require another operating system, open the Unity project with Harken Studio there and run Check and integrate again. Apple targets must be completed on macOS.");
            step.details << QStringLiteral("Target runtimes deferred to another operating system: %1")
                                .arg(optionalMissingPlatforms.join(QStringLiteral(", ")));
        }
        step.artifacts << result.integrationSourcePath;
    });

    runStep([&](StepResult& step) {
        QJsonObject targetSnapshot;
        QString snapshotError;
        result.planHash = integrationChangeFingerprint(result.unityProjectPath, result.integrationSourcePath,
                                               targetIntegration, sourceHashes, &targetSnapshot, &snapshotError);
        const auto validateReview = qScopeGuard([&]() {
            if (step.status == StepStatus::Failed) return;
            if (result.planHash.isEmpty()) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("The Integration Package files could not be checked safely.");
                step.cause = snapshotError;
            } else if (!request.approvedPlanHash.isEmpty() && request.approvedPlanHash != result.planHash) {
                result.requiresConfirmation = true;
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("Integration Package files changed after the safety check. Run the check again before continuing.");
            }
        });
        const bool targetExists = QDir(targetIntegration).exists()
            && !QDir(targetIntegration).entryList(QDir::AllEntries | QDir::Hidden
                | QDir::System | QDir::NoDotAndDotDot).isEmpty();
        QVector<FileChange> changes;
        result.targetInstalled = targetExists;
        if (!targetExists) {
            if (!VNH::checkIntegrationDirectoryChanges({targetIntegration}, {targetIntegration}, &step.cause)) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("Integration Package file write access check failed.");
                return;
            }
            result.filesAdded = sourceFiles.size();
            result.changesDetected = true;
            for (const QString& relative : sourceFiles)
                changes.push_back({QStringLiteral("add"), relative});
            step.details = describeFileChanges(
                changes, result.integrationInstallPath);
            result.fileChanges = changes;
            step.summary = QStringLiteral("No embedded Harken Integration Package is installed; %1 file(s) are ready to install.")
                               .arg(result.filesAdded);
            return;
        }

        OwnershipRecord previousManaged = previousManagedFiles(targetIntegration);
        const JsonRead stateRead = readJsonObject(QDir(targetIntegration).filePath(QLatin1String(kInstalledState)));
        retainedTarget = previousManaged.status == ReadStatus::Ok && previousManaged.managedFiles.isEmpty()
            && stateRead.object.value(QStringLiteral("installation_state")).toString() == QStringLiteral("removed");
        if (previousManaged.status == ReadStatus::Missing && request.preserveUnownedTarget
            && UnityInstallation::discover(result.unityProjectPath).installations.isEmpty()) {
            previousManaged.status = ReadStatus::Ok;
            retainedTarget = true;
        }
        if (previousManaged.status != ReadStatus::Ok) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The installed Integration Package could not be compared because its ownership record is missing, invalid or unreadable.");
            step.cause = QStringLiteral("%1 could not establish which files belong to Harken. Nothing was compared or changed.")
                             .arg(QDir(targetIntegration).filePath(QLatin1String(kInstalledState)));
            step.remediation = QStringLiteral("Restore the ownership record from backup, or resolve file access errors, then check again.");
            return;
        }
        const QJsonObject installedState = readJsonObject(
            QDir(targetIntegration).filePath(QLatin1String(kInstalledState))).object;
        const QJsonObject installedHashes = installedState.value(QStringLiteral("sha256")).toObject();
        const QJsonObject installedTextHashes = installedState.value(QStringLiteral("text_lf_sha256")).toObject();
        const QJsonObject actualHashes = targetSnapshot.value(QStringLiteral("files")).toObject();
        if (retainedTarget) {
            result.targetInstalled = false;
            if (!removalSnapshot(targetIntegration, &retainedSnapshot, &snapshotError)) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("The Integration Package files could not be checked safely.");
                step.cause = snapshotError;
                return;
            }
            const QSet<QString> existingKeys = comparisonPathKeys(actualHashes.keys());
            for (const QString& relative : sourceFiles)
                retainedFilesOverlap |= existingKeys.contains(comparisonPathKey(relative));
        }
        for (const QString& relative : previousManaged.managedFiles) {
            if (result.planHash.isEmpty()) break;
            if (!actualHashes.contains(relative)) {
                if (UnityInstallation::isDocumentationOrSample(relative))
                    result.missingOptionalFiles << relative;
                else
                    result.missingFiles << relative;
            }
            else if (installedHashes.contains(relative)
                     && actualHashes.value(relative).toString().compare(
                         installedHashes.value(relative).toString(), Qt::CaseInsensitive) != 0) {
                if (UnityInstallation::matchesTextHashIgnoringLineEndings(
                        QDir(targetIntegration).filePath(relative), installedHashes.value(relative).toString().toLatin1(),
                        installedTextHashes.value(relative).toString().toLatin1()))
                    result.lineEndingFiles << relative;
                else if (UnityInstallation::matchesTextHashIgnoringEncoding(
                             QDir(targetIntegration).filePath(relative), installedHashes.value(relative).toString().toLatin1(),
                             installedTextHashes.value(relative).toString().toLatin1()))
                    result.encodingFiles << relative;
                else
                    result.modifiedFiles << relative;
            }
        }
        result.modifiedFiles.sort();
        result.missingFiles.sort();
        result.missingOptionalFiles.sort();
        const QSet<QString> previousManagedKeys =
            comparisonPathKeys(previousManaged.managedFiles);
        QStringList targetFiles = allRelativeFiles(targetIntegration);
        targetFiles.removeAll(QLatin1String(kInstalledState));
        const QSet<QString> sourceKeys = comparisonPathKeys(sourceFiles);
        QSet<QString> sourceRuntimeIdentities;
        QMap<QString, QString> sourceRuntimePaths;
        for (const QString& relative : sourceFiles) {
            const QString identity = harkenRuntimeIdentity(relative);
            if (!identity.isEmpty()) {
                sourceRuntimeIdentities.insert(identity);
                sourceRuntimePaths.insert(identity, relative);
            }
        }
        QSet<QString> optionalRuntimeIdentities;
        for (const QString& relative : declaredOptionalRuntimePaths) {
            const QString identity = harkenRuntimeIdentity(relative);
            if (!identity.isEmpty()) optionalRuntimeIdentities.insert(identity);
        }
        QStringList unityMetadataFiles;
        QStringList optionalRuntimeFiles;
        QStringList projectSpecificFiles;
        int filesAdded = 0;
        int filesChanged = 0;
        QStringList unreadableTargets;
        for (const QString& relative : sourceFiles) {
            const FileDigest digest = sha256File(
                QDir(targetIntegration).filePath(relative));
            if (digest.status == ReadStatus::Missing) {
                ++filesAdded;
                changes.push_back({QStringLiteral("add"), relative});
            } else if (digest.unreadable()) {
                unreadableTargets << relative;
            } else if (!digest.matches(sourceHashes.value(relative))) {
                const bool lineEndingsOnly = UnityInstallation::matchesTextHashIgnoringLineEndings(
                    QDir(targetIntegration).filePath(relative), sourceHashes.value(relative), sourceTextHashes.value(relative));
                const bool encodingOnly = !lineEndingsOnly && UnityInstallation::matchesTextHashIgnoringEncoding(
                    QDir(targetIntegration).filePath(relative), sourceHashes.value(relative), sourceTextHashes.value(relative));
                if (lineEndingsOnly || encodingOnly) {
                    textFormatPreservedFiles << relative;
                    auto& differences = encodingOnly ? result.encodingFiles : result.lineEndingFiles;
                    if (!differences.contains(relative)) differences << relative;
                    installedContentHashes[relative] = digest.hash;
                } else {
                    ++filesChanged;
                    changes.push_back({QStringLiteral("replace"), relative});
                }
            }
        }
        if (!unreadableTargets.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("%1 installed Integration Package file(s) could not be read, so the comparison is incomplete.")
                               .arg(unreadableTargets.size());
            step.cause = QStringLiteral("These files exist inside %1 but cannot be opened, so the Harken integration cannot tell whether they still match the Integration Package source. Nothing was compared or changed.")
                             .arg(targetIntegration);
            step.remediation = QStringLiteral("Close Unity and any process holding these files, then run Check only again.");
            step.details = unreadableTargets;
            return;
        }
        result.filesAdded = filesAdded;
        result.filesChanged = filesChanged;
        for (const QString& relative : targetFiles) {
            if (discardableOsMetadata(relative)) continue;
            if (sourceKeys.contains(comparisonPathKey(relative))) continue;
            QString runtimePath = relative;
            if (runtimePath.endsWith(QStringLiteral(".meta"),
                                     Qt::CaseInsensitive)) {
                runtimePath.chop(5);
            }
            const QString runtimeIdentity = harkenRuntimeIdentity(runtimePath);
            const bool obsoleteRuntimeAlias =
                !retainedTarget && !runtimeIdentity.isEmpty() &&
                sourceRuntimeIdentities.contains(runtimeIdentity) &&
                !sourceKeys.contains(comparisonPathKey(runtimePath));
            if (obsoleteRuntimeAlias && relative != runtimePath) {
                const QString destination = sourceRuntimePaths.value(runtimeIdentity);
                const QString destinationMeta = destination + QStringLiteral(".meta");
                if (!QFileInfo::exists(QDir(targetIntegration).filePath(destination)) &&
                    !QFileInfo::exists(QDir(targetIntegration).filePath(destinationMeta)) &&
                    !sourceKeys.contains(comparisonPathKey(destinationMeta))) {
                    if (runtimeMetadataMoves.contains(destinationMeta)) {
                        step.status = StepStatus::Failed;
                        step.summary = QStringLiteral("Multiple old runtime metadata files map to %1.").arg(destinationMeta);
                        step.remediation = QStringLiteral("Keep the intended plugin GUID and platform settings, then retry. Nothing was changed.");
                        return;
                    }
                    runtimeMetadataMoves.insert(destinationMeta, relative);
                }
            }
            if (previousManagedKeys.contains(comparisonPathKey(relative)) ||
                obsoleteRuntimeAlias) {
                staleFiles << relative;
                changes.push_back({QStringLiteral("remove"), relative});
            } else {
                preservedFiles << relative;
                changes.push_back({QStringLiteral("preserve"), relative});
                if (relative.endsWith(QStringLiteral(".meta"), Qt::CaseInsensitive))
                    unityMetadataFiles << relative;
                else if (matchesDeclaredArtifact(relative, declaredOptionalRuntimePaths) ||
                         (!runtimeIdentity.isEmpty() &&
                          optionalRuntimeIdentities.contains(runtimeIdentity)))
                    optionalRuntimeFiles << relative;
                else
                    projectSpecificFiles << relative;
            }
        }
        step.details = describeFileChanges(
            changes, result.integrationInstallPath);
        result.fileChanges = changes;
        if (retainedTarget) {
            step.details << QStringLiteral("Existing files are outside Integration Package ownership. Files absent from the selected source will be preserved.");
            if (retainedFilesOverlap)
                step.details << QStringLiteral("Files also present in the selected source will become managed Integration Package files. Approval is required, and a permanent backup of the existing files will be created first.");
        }
        result.staleFilesToRemove = staleFiles.size();
        result.lineEndingFiles.sort();
        result.encodingFiles.sort();
        for (const QString& relative : result.encodingFiles) result.lineEndingFiles.removeAll(relative);
        if (!result.lineEndingFiles.isEmpty())
            step.details << QStringLiteral("%1 Integration Package text file(s) differ only in LF/CRLF line endings; contents are unchanged.")
                                .arg(result.lineEndingFiles.size())
                         << result.lineEndingFiles.join(QLatin1Char('\n'));
        if (!result.encodingFiles.isEmpty()) {
            step.status = StepStatus::Warning;
            step.details << QStringLiteral("Review recommended: %1 Integration Package text file(s) use a different encoding; text contents match after normalizing encoding and line endings. Integration Package updates can continue.")
                                .arg(result.encodingFiles.size())
                         << result.encodingFiles.join(QLatin1Char('\n'));
        }
        if (!result.missingOptionalFiles.isEmpty())
            step.details << QStringLiteral("Missing documentation/sample files (does not block Integration Package updates):")
                         << result.missingOptionalFiles.join(QLatin1Char('\n'));
        result.targetOnlyFilesPreserved = preservedFiles.size();
        result.changesDetected = result.filesAdded > 0 ||
                                 result.filesChanged > 0 ||
                                 !staleFiles.isEmpty() ||
                                 !projectSpecificFiles.isEmpty() || retainedTarget;

        const bool managedDiffers = result.filesAdded > 0 ||
                                    result.filesChanged > 0 ||
                                    !staleFiles.isEmpty() || retainedTarget;
        if (!managedDiffers) {
            result.targetAlreadyCurrent = true;
            if (preservedFiles.isEmpty()) {
                step.summary = QStringLiteral("The installed Integration Package exactly matches selected Integration Package source %1.")
                                   .arg(sourceVersion);
            } else if (projectSpecificFiles.isEmpty()) {
                step.summary = QStringLiteral("All Harken-managed files match Integration Package %1; %2 Unity metadata file(s) and %3 optional target runtime file(s) are retained as expected.")
                                   .arg(sourceVersion).arg(unityMetadataFiles.size())
                                   .arg(optionalRuntimeFiles.size());
            } else {
                step.status = StepStatus::Warning;
                step.summary = QStringLiteral("All Harken-managed files match Integration Package %1; %2 project-specific file(s) need review, while %3 Unity metadata and %4 optional runtime file(s) are retained normally.")
                                   .arg(sourceVersion).arg(projectSpecificFiles.size())
                                   .arg(unityMetadataFiles.size())
                                   .arg(optionalRuntimeFiles.size());
                step.cause = QStringLiteral("The project-specific files are outside Harken Integration Package ownership, so the check cannot determine whether they are still needed.");
                step.remediation = QStringLiteral("Review only the project-specific files listed below. Unity-generated metadata requires no action.");
            }
            return;
        }

        for (const FileChange& change : changes) {
            if (change.action != QStringLiteral("preserve"))
                filesToChange.append(QDir(targetIntegration).filePath(change.relative));
        }
        filesToChange.append(QDir(targetIntegration).filePath(QLatin1String(kInstalledState)));
        QString accessError;
        if (!VNH::checkIntegrationFilesWritable(filesToChange, &accessError)
            || !VNH::checkIntegrationDirectoryChanges({targetIntegration}, {targetIntegration}, &accessError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Integration Package file write access check failed.");
            step.cause = accessError;
            return;
        }
        const QString counts = joinedCountSummary(
            result.filesAdded, result.filesChanged, staleFiles.size(),
            projectSpecificFiles.size(), unityMetadataFiles.size(),
            optionalRuntimeFiles.size());
        if (request.checkOnly) {
            step.status = StepStatus::Warning;
            step.summary = QStringLiteral("Check found Integration Package differences: %1.").arg(counts);
            step.cause = QStringLiteral("Check-only mode did not change %1.")
                             .arg(result.integrationInstallPath);
            step.remediation = QStringLiteral("Review the file list before confirming the Integration Package change.");
            return;
        }
        if (!request.confirmTargetChanges || (retainedTarget && request.approvedPlanHash.isEmpty())) {
            result.requiresConfirmation = true;
            step.status = StepStatus::Warning;
            step.summary = QStringLiteral("The existing Integration Package differs from selected Integration Package source %1: %2.")
                               .arg(sourceVersion, counts);
            step.cause = QStringLiteral("Updating will replace Harken-managed files. Target-only files will be retained.");
            step.remediation = QStringLiteral("Review this file list, then explicitly approve the update or cancel without changing the Integration Package.");
            return;
        }
        step.summary = QStringLiteral("Approved Integration Package update: %1.").arg(counts);
    });

    runStep([&](StepResult& step) {
        if (request.checkOnly) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Check-only mode requested; no Integration Package files were changed.");
            return;
        }
        if (result.requiresConfirmation) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("No Integration Package files were changed; explicit approval is required.");
            return;
        }
        if (result.targetAlreadyCurrent) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Installation is already current; no files were copied.");
            return;
        }

        QString safetyError;
        if (!UnityInstallation::validateTarget(result.unityProjectPath, targetIntegration, &safetyError, false, request.preserveUnownedTarget)
            || !UnityInstallation::validateTree(result.integrationSourcePath, &safetyError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package paths changed after checking. No files were installed.");
            step.cause = safetyError;
            return;
        }
        QDir packages(QFileInfo(targetIntegration).absolutePath());
        const QString targetName = QFileInfo(targetIntegration).fileName();
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString stageName = QStringLiteral(".%1.staging-%2").arg(targetName, token);
        const QString backupName = QStringLiteral(".%1.backup-%2").arg(targetName, token);
        const QString stageRoot = packages.filePath(stageName);
        if (!QDir().mkpath(stageRoot)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not create an Integration Package staging directory.");
            step.cause = stageRoot;
            step.remediation = QStringLiteral("Check installation directory permissions and free disk space.");
            return;
        }

        QString error;
        if (retainedTarget) {
            for (const auto& directory : retainedSnapshot.value(QStringLiteral("directories")).toArray()) {
                if (!QDir().mkpath(QDir(stageRoot).filePath(directory.toString()))) {
                    step.status = StepStatus::Failed;
                    step.summary = QStringLiteral("Could not preserve project directories. No project files were changed.");
                    step.cause = QDir(stageRoot).filePath(directory.toString());
                    QDir(stageRoot).removeRecursively();
                    return;
                }
            }
        }
        if (retainedFilesOverlap) {
            const QString backup = defaultIntegrationBackupPath(result.unityProjectPath);
            const QString snapshot = QDir(backup).filePath(QStringLiteral("UnityProject/") + result.integrationInstallPath);
            QJsonObject copied;
            bool backedUp = !backup.isEmpty() && !QFileInfo::exists(backup)
                && !UnityInstallation::containsPath(result.unityProjectPath, backup)
                && !UnityInstallation::containsPath(backup, result.unityProjectPath)
                && UnityInstallation::validatePath(backup, &error);
            for (const auto& directory : retainedSnapshot.value(QStringLiteral("directories")).toArray())
                if (backedUp) backedUp = QDir().mkpath(QDir(snapshot).filePath(directory.toString()));
            backedUp = backedUp
                && copyFiles(targetIntegration, snapshot, retainedSnapshot.value(QStringLiteral("files")).toObject().keys(), &error)
                && copyFilePermissions(targetIntegration, snapshot, retainedSnapshot.value(QStringLiteral("files")).toObject().keys(), &error)
                && (retainedSnapshot.value(QStringLiteral("root_meta")).toString().isEmpty()
                    || copyFileAtomically(targetIntegration + QStringLiteral(".meta"), snapshot + QStringLiteral(".meta"), &error))
                && removalSnapshot(snapshot, &copied, &error) && copied == retainedSnapshot
                && VNH::writeIntegrationFile(QDir(backup).filePath(QStringLiteral("backup.json")),
                    QJsonDocument(QJsonObject{{QStringLiteral("operation"), QStringLiteral("install_over_retained_files")},
                        {QStringLiteral("project"), result.unityProjectPath},
                        {QStringLiteral("install_path"), result.integrationInstallPath},
                        {QStringLiteral("inventory"), retainedSnapshot}}).toJson(QJsonDocument::Indented), &error);
            if (!backedUp) {
                QDir(stageRoot).removeRecursively();
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("Existing project files could not be backed up. No project files were changed.");
                step.cause = error;
                return;
            }
            step.details << QStringLiteral("A permanent backup of the existing project files was created before installation:") << backup;
            step.artifacts << backup;
        }
        if (!copyFiles(result.integrationSourcePath, stageRoot, sourceFiles, &error) ||
            !copyFiles(targetIntegration, stageRoot, textFormatPreservedFiles, &error) ||
            (!preservedFiles.isEmpty() &&
             !copyFiles(targetIntegration, stageRoot, preservedFiles, &error)) ||
            !writeInstalledState(stageRoot, sourceVersion, sourceFiles,
                                 installedContentHashes, sourceTextHashes, &error, sourceReleaseHint(result.integrationSourcePath))) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Failed to stage the Unity Integration Package update.");
            step.cause = error;
            step.remediation = QStringLiteral("Check file locks, permissions, and disk space, then retry.");
            return;
        }
        if (retainedTarget && !copyFilePermissions(targetIntegration, stageRoot, preservedFiles, &error)) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not preserve existing file permissions. No project files were changed.");
            step.cause = error;
            return;
        }
        QStringList stagingMismatches;
        QStringList stagingUnreadable;
        for (auto it = runtimeMetadataMoves.cbegin(); it != runtimeMetadataMoves.cend(); ++it) {
            const QString previous = QDir(targetIntegration).filePath(it.value());
            const QString staged = QDir(stageRoot).filePath(it.key());
            const FileDigest digest = sha256File(previous);
            if (!QFile::copy(previous, staged) ||
                !sha256File(staged).matches(digest.hash)) {
                QDir(stageRoot).removeRecursively();
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("Could not preserve native plugin metadata for %1.").arg(it.key());
                step.remediation = QStringLiteral("Check file locks and permissions, then retry. The existing Integration Package was not changed.");
                return;
            }
        }
        for (const QString& relative : sourceFiles) {
            const FileDigest digest = sha256File(
                QDir(stageRoot).filePath(relative));
            if (digest.unreadable())
                stagingUnreadable << relative;
            else if (!digest.matches(installedContentHashes.value(relative))
                     || (sourceTextHashes.contains(relative)
                         && !UnityInstallation::matchesTextHashIgnoringLineEndings(
                             QDir(stageRoot).filePath(relative), sourceHashes.value(relative), sourceTextHashes.value(relative))
                         && !UnityInstallation::matchesTextHashIgnoringEncoding(
                             QDir(stageRoot).filePath(relative), sourceHashes.value(relative), sourceTextHashes.value(relative))))
                stagingMismatches << relative;
        }
        if (!stagingMismatches.isEmpty() || !stagingUnreadable.isEmpty()) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The staged Unity Integration Package failed checksum verification.");
            step.cause = stagingUnreadable.isEmpty()
                ? QStringLiteral("Staging mismatches: %1")
                      .arg(stagingMismatches.join(QStringLiteral(", ")))
                : QStringLiteral("Staged files that could not be read: %1")
                      .arg(stagingUnreadable.join(QStringLiteral(", ")));
            step.remediation = QStringLiteral("Repair or reselect the Integration Package source, check the storage device, then retry. The existing Unity Integration Package was not changed.");
            return;
        }

        QJsonObject currentTarget;
        const QByteArray currentHash = integrationChangeFingerprint(result.unityProjectPath,
            result.integrationSourcePath, targetIntegration, sourceHashes, &currentTarget, &safetyError);
        if (currentHash.isEmpty() || currentHash != result.planHash) {
            QDir(stageRoot).removeRecursively();
            result.requiresConfirmation = true;
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Integration Package files changed after the safety check. Run the check again before continuing.");
            step.cause = safetyError;
            return;
        }
        const bool hadTarget = QDir(targetIntegration).exists();
        if (!UnityInstallation::validateTarget(result.unityProjectPath, targetIntegration, &safetyError, false, request.preserveUnownedTarget)) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Integration Package installation changed while staging. No files were replaced.");
            step.cause = safetyError;
            return;
        }
        QString moveError;
        if (!VNH::checkIntegrationFilesWritable(filesToChange, &moveError)
            || !VNH::checkIntegrationDirectoryChanges({targetIntegration}, {targetIntegration, stageRoot}, &moveError)) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Integration Package file write access check failed.");
            step.cause = moveError;
            return;
        }
        if (hadTarget && !VNH::renameIntegrationDirectory(packages.filePath(targetName), packages.filePath(backupName), &moveError)) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not back up the existing Unity Integration Package.");
            step.cause = moveError;
            step.remediation = QStringLiteral("Check file write protection, directory permissions and processes holding files open at %1. If checkout or unlocking is required, do that first, then retry. The existing Integration Package was not changed.").arg(targetIntegration);
            return;
        }
        if (hadTarget) updateBackupRoot = packages.filePath(backupName);
        if (!VNH::renameIntegrationDirectory(packages.filePath(stageName), packages.filePath(targetName), &moveError)) {
            QString restoreError;
            const bool restored = !hadTarget || VNH::renameIntegrationDirectory(packages.filePath(backupName), packages.filePath(targetName), &restoreError);
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not activate the staged Unity Integration Package.");
            step.cause = restored ? QStringLiteral("The original installation state was restored.")
                                  : QStringLiteral("The previous Integration Package could not be restored. Its backup is listed below.");
            step.cause += QLatin1Char('\n') + moveError;
            if (!restoreError.isEmpty()) step.cause += QLatin1Char('\n') + restoreError;
            if (hadTarget && !restored) step.artifacts << updateBackupRoot;
            step.remediation = QStringLiteral("Check %1 directory locks and permissions, then retry.")
                                   .arg(packages.absolutePath());
            return;
        }
        installationActivated = true;

        result.integrationFilesCopied = sourceFiles.size();
        result.targetInstalled = true;
        step.summary = QStringLiteral("Installed Integration Package %1 from a verified staging directory; copied %2 managed file(s) and preserved %3 target-only file(s).")
                           .arg(sourceVersion).arg(result.integrationFilesCopied)
                           .arg(preservedFiles.size());
        step.artifacts << targetIntegration;
    });

    runStep([&](StepResult& step) {
        if (result.requiresConfirmation) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Verification will run after the Integration Package update is approved.");
            return;
        }
        const bool managedChangesDetected = result.filesAdded > 0 ||
                                            result.filesChanged > 0 ||
                                            result.staleFilesToRemove > 0;
        if (request.checkOnly && !result.targetInstalled) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("No installed Unity Integration Package exists to verify.");
            return;
        }
        if (request.checkOnly && managedChangesDetected) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Installed Integration Package verification is represented by the differences reported above.");
            return;
        }
        QStringList mismatches;
        QStringList unreadable;
        const auto rollback = [&]() {
            if (!installationActivated) return;
            QDir parent(QFileInfo(targetIntegration).absolutePath());
            const QString targetName = QFileInfo(targetIntegration).fileName();
            const QString failedName = QStringLiteral(".%1.failed-%2").arg(targetName,
                QUuid::createUuid().toString(QUuid::WithoutBraces));
            const bool displaced = !QFileInfo::exists(targetIntegration)
                || VNH::renameIntegrationDirectory(parent.filePath(targetName), parent.filePath(failedName));
            if (QFileInfo::exists(parent.filePath(failedName))) step.artifacts << parent.filePath(failedName);
            const bool restored = displaced && (updateBackupRoot.isEmpty()
                || VNH::renameIntegrationDirectory(parent.filePath(QFileInfo(updateBackupRoot).fileName()), parent.filePath(targetName)));
            result.targetInstalled = QDir(targetIntegration).exists();
            result.integrationFilesCopied = 0;
            if (restored) {
                step.remediation = QStringLiteral("The original installation state was restored. Review the unverified files listed below and retry with a complete Integration Package source.");
            } else {
                step.remediation = QStringLiteral("Automatic recovery failed. Close the editor, preserve the listed folders, and restore the previous Integration Package from the backup before retrying.");
                if (!updateBackupRoot.isEmpty()) step.artifacts << updateBackupRoot;
            }
        };
        for (const QString& relative : sourceFiles) {
            const FileDigest digest = sha256File(
                QDir(targetIntegration).filePath(relative));
            if (digest.unreadable())
                unreadable << relative;
            else if (!digest.matches(installedContentHashes.value(relative)))
                mismatches << relative;
        }
        if (!unreadable.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Installed Integration Package files could not be read, so their integrity is unverified.");
            step.cause = QStringLiteral("Files that exist but cannot be opened: %1")
                             .arg(unreadable.join(QStringLiteral(", ")));
            step.remediation = QStringLiteral("Close Unity and any process holding these files, then run Check only again.");
            step.details = unreadable;
            rollback();
            return;
        }
        if (!mismatches.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Installed Integration Package files do not match the verified source package.");
            step.cause = QStringLiteral("Missing or checksum-mismatched files: %1")
                             .arg(mismatches.join(QStringLiteral(", ")));
            step.remediation = QStringLiteral("Close Unity and rerun integration to repair the embedded package.");
            step.details = mismatches;
            rollback();
            return;
        }
        result.targetCurrent = true;
        step.summary = QStringLiteral("Verified the content integrity of %1 Harken-managed Integration Package file(s).")
                           .arg(sourceFiles.size());
        step.artifacts << targetIntegration;
        if (!updateBackupRoot.isEmpty() && !QDir(updateBackupRoot).removeRecursively()) {
            step.status = StepStatus::Warning;
            step.details << QStringLiteral("The Integration Package passed verification, but its old backup could not be removed.");
            step.artifacts << updateBackupRoot;
        }
    });

    runStep([&](StepResult& step) {
        if (request.unityProjectPath.trimmed().isEmpty()) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("No report was written because the Unity project was not valid.");
            return;
        }
        const QString reportDir = integrationReportDirectory(result.unityProjectPath, &step.cause);
        if (reportDir.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not prepare the Hub report directory.");
            return;
        }
        result.jsonReportPath = QDir(reportDir).filePath(QStringLiteral("latest.json"));
        result.textReportPath = QDir(reportDir).filePath(QStringLiteral("latest.txt"));
        step.status = StepStatus::Succeeded;
        step.summary = QStringLiteral("Wrote machine-readable and human-readable Integration Package installation reports.");
        step.artifacts << result.jsonReportPath << result.textReportPath;

        QJsonObject root;
        root[QStringLiteral("schema_version")] = 2;
        root[QStringLiteral("generated_at_utc")] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        root[QStringLiteral("success")] = !blocked && !result.requiresConfirmation;
        root[QStringLiteral("check_only")] = request.checkOnly;
        root[QStringLiteral("changes_detected")] = result.changesDetected;
        root[QStringLiteral("requires_confirmation")] = result.requiresConfirmation;
        root[QStringLiteral("target_current")] = result.targetCurrent;
        root[QStringLiteral("unity_project_path")] = result.unityProjectPath;
        root[QStringLiteral("sdk_source_path")] = result.integrationSourcePath;
        root[QStringLiteral("sdk_install_path")] = result.integrationInstallPath;
        root[QStringLiteral("sdk_version")] = sourceVersion;
        root[QStringLiteral("files_added")] = result.filesAdded;
        root[QStringLiteral("files_changed")] = result.filesChanged;
        root[QStringLiteral("stale_files_to_remove")] = result.staleFilesToRemove;
        root[QStringLiteral("target_only_files_preserved")] =
            result.targetOnlyFilesPreserved;
        QJsonArray steps;
        for (const StepResult& item : result.steps) steps.append(stepToJson(item));
        root[QStringLiteral("steps")] = steps;

        QString jsonFileError;
        if (!writeIntegrationReport(result.jsonReportPath,
                QJsonDocument(root).toJson(QJsonDocument::Indented), &jsonFileError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The integration report could not be written.");
            step.cause = jsonFileError;
            step.remediation = QStringLiteral("Check write permission for the Hub report directory: %1").arg(reportDir);
            return;
        }

        QString reportText = QStringLiteral("Harken Unity Integration Package installation report\n\n");
        reportText += QStringLiteral("Generated: %1\nIntegration Package version: %2\nIntegration Package source: %3\nUnity project: %4\nConfirmation required: %5\n\n")
                        .arg(root[QStringLiteral("generated_at_utc")].toString(),
                             sourceVersion, result.integrationSourcePath,
                             result.unityProjectPath,
                             result.requiresConfirmation ? QStringLiteral("yes")
                                                         : QStringLiteral("no"));
        for (const StepResult& item : result.steps) {
            reportText += QStringLiteral("%1 — %2\n\n%3\n\n")
                            .arg(item.title, item.status == StepStatus::Warning ? QStringLiteral("review recommended") : statusName(item.status), item.summary);
            if (!item.cause.isEmpty())
                reportText += QStringLiteral("Cause: %1\n\n").arg(item.cause);
            if (!item.remediation.isEmpty())
                reportText += QStringLiteral("How to fix: %1\n\n").arg(item.remediation);
            if (!item.details.isEmpty())
                reportText += QStringLiteral("Details:\n%1\n\n")
                                .arg(jsonArrayText(item.details));
        }
        QString textFileError;
        if (!writeIntegrationReport(result.textReportPath,
                reportText.toUtf8(), &textFileError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("JSON report was written, but the text report failed.");
            step.cause = textFileError;
            step.remediation = QStringLiteral("Check write permission for the Hub report directory: %1").arg(reportDir);
            return;
        }
    }, true);

    while (nextStep < result.steps.size()) runStep([](StepResult&) {});
    result.success = !result.requiresConfirmation;
    for (const StepResult& step : result.steps) {
        if (step.status == StepStatus::Failed) result.success = false;
        if (step.status == StepStatus::Warning) result.hasWarnings = true;
    }
    return result;
}

} // namespace VNX
