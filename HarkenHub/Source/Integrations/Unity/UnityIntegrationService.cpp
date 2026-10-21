#include "Integrations/IntegrationFileSystem.h"
#include "Integrations/Unity/UnityIntegrationService.h"
#include "Integrations/IntegrationProgress.h"
#include "Integrations/Unity/UnityInstallation.h"

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
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <functional>
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
    {"sdk_preflight", "Validate Harken Unity SDK source"},
    {"compare_sdk", "Compare installed Unity SDK"},
    {"install_sdk", "Install or update Unity SDK"},
    {"verify", "Verify installed SDK"},
    {"report", "Write integration report"},
};

constexpr StepSpec kRemovalStepSpecs[] = {
    {"unity_preflight", "Validate Unity project"},
    {"inspect_installation", "Review installed Harken SDK"},
    {"remove_integration", "Back up and remove Harken SDK"},
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
    if (!request.sdkInstallPath.trimmed().isEmpty()) return request.sdkInstallPath;
    return request.installToAssets
        ? QStringLiteral("Assets/HarkenSDK")
        : UnityInstallation::defaultRelativePath();
}

bool lockUnityProject(const QString& root, std::unique_ptr<QLockFile>* lock,
                      QString* error)
{
    const QString reportDir = QDir(root).filePath(QStringLiteral("Library/HarkenIntegration"));
    if (!UnityInstallation::validatePath(reportDir, error)) return false;
    for (const QString& name : {QStringLiteral("latest.json"), QStringLiteral("latest.md"),
                               QStringLiteral("latest-removal.json"), QStringLiteral("latest-removal.md"),
                               QStringLiteral("pending-migration.json")}) {
        if (!UnityInstallation::validatePath(QDir(reportDir).filePath(name), error)) return false;
    }
    if (!QDir().mkpath(reportDir)) {
        *error = QStringLiteral("Cannot create directory: %1").arg(reportDir);
        return false;
    }
    const QString lockPath = QDir(reportDir).filePath(QStringLiteral("integration.lock"));
    if (!UnityInstallation::validatePath(lockPath, error)) return false;
    *lock = std::make_unique<QLockFile>(lockPath);
    (*lock)->setStaleLockTime(0);
    if (!(*lock)->tryLock()) {
        *error = QStringLiteral("Another integration operation is using this Unity project.");
        return false;
    }
    return true;
}

bool prepareInstallation(const UnityIntegrationService::Request& request,
                         UnityIntegrationService::Result& result,
                         QString* target, std::unique_ptr<QLockFile>* lock,
                         QString* error)
{
    const QString root = result.unityProjectPath;
    if (!UnityInstallation::resolveTarget(root, requestedInstallPath(request), target, error))
        return false;
    result.sdkInstallPath = QDir(root).relativeFilePath(*target);
    if (!UnityInstallation::validatePath(*target + QStringLiteral(".meta"), error)
        || !lockUnityProject(root, lock, error)) return false;
    if (!UnityInstallation::validateTarget(root, *target, error, request.removeIntegration)) return false;
    if ((!request.checkOnly || request.removeIntegration)
        && QFileInfo::exists(QDir(root).filePath(QStringLiteral("Temp/UnityLockfile")))) {
        *error = QStringLiteral("Close Unity before installing, updating or removing the SDK.");
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

QString missingRuntimeDetail(const QString& sdkRoot, const QString& platform,
                             const QStringList& alternatives)
{
    QStringList locations;
    for (const QString& relative : alternatives)
        locations << QDir::toNativeSeparators(QDir(sdkRoot).filePath(relative));
    return QStringLiteral("Missing component: %1 game runtime\n"
                          "Expected inside the selected SDK source: %2\n"
                          "Purpose: %3\n"
                          "What this means: This SDK source cannot safely prepare the Unity project for %1 builds until a verified runtime is restored.")
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

FileExplanation explainSdkFile(const QString& relative)
{
    const QString path = normalizePath(relative);
    if (path.endsWith(QStringLiteral(".meta"), Qt::CaseInsensitive)) {
        return {
            QStringLiteral("unity_metadata"),
            QStringLiteral("Unity identity and import settings"),
            QStringLiteral("These files let Unity keep stable asset references and remember how neighboring SDK files should be imported."),
            QStringLiteral("Keeping them unchanged preserves existing Unity references and platform import settings.")};
    }
    if (path == QStringLiteral("package.json")) {
        return {
            QStringLiteral("package_identity"),
            QStringLiteral("Package identity and compatibility"),
            QStringLiteral("Describes the Harken package name, version, Unity compatibility, and optional samples to Unity Package Manager."),
            QStringLiteral("Unity will recognize and load the selected Harken SDK version with its current package settings.")};
    }
    if (path == QLatin1String(kIntegrationManifest)) {
        return {
            QStringLiteral("integration_safety"),
            QStringLiteral("Integration safety rules"),
            QStringLiteral("Defines which SDK files are mandatory and which Unity target runtimes should be available on each editor operating system."),
            QStringLiteral("Future checks can detect an incomplete or damaged Harken SDK before changing project files.")};
    }
    if (path == QStringLiteral("Runtime/Harken.Core.asmdef") ||
        path == QStringLiteral("Runtime/Unity/Harken.Unity.asmdef")) {
        return {
            QStringLiteral("managed_assembly"),
            QStringLiteral("Unity C# assembly settings"),
            QStringLiteral("Tells Unity how to compile the Harken C# runtime and host-adapter assemblies and which dependencies they need."),
            QStringLiteral("Unity scripts can compile against the Harken API using the settings required by this SDK version.")};
    }
    if (path.startsWith(QStringLiteral("Runtime/Generated/"))) {
        return {
            QStringLiteral("generated_binding"),
            QStringLiteral("C# connection to the native audio engine"),
            QStringLiteral("These generated files define native function calls, shared data types, constants, callbacks, and compatibility checks."),
            QStringLiteral("Unity scripts and the native Harken runtime will use matching function and data definitions.")};
    }
    if (path.startsWith(QStringLiteral("Runtime/Sdk/"))) {
        return {
            QStringLiteral("managed_api"),
            QStringLiteral("C# gameplay API"),
            QStringLiteral("These files provide the C# API for engine lifecycle, actions, emitters, listeners, Packs, gameplay values, voices, and callbacks."),
            QStringLiteral("Unity gameplay code will use the behavior and API contract supplied by this SDK version.")};
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
            QStringLiteral("Provides an optional example showing how a Unity project can call the Harken SDK."),
            QStringLiteral("The example becomes available for learning or import; it does not change game behavior unless the user imports it.")};
    }
    if (path.endsWith(QStringLiteral("README.md"), Qt::CaseInsensitive) ||
        path.endsWith(QStringLiteral("CHANGELOG.md"), Qt::CaseInsensitive) ||
        path.endsWith(QStringLiteral("LICENSE.md"), Qt::CaseInsensitive)) {
        return {
            QStringLiteral("documentation"),
            QStringLiteral("Documentation and release information"),
            QStringLiteral("Provides documentation, release notes, or licensing information for this part of the Harken SDK."),
            QStringLiteral("Users will see documentation that matches the installed SDK; runtime behavior is unchanged.")};
    }
    return {
        QStringLiteral("supporting_content"),
        QStringLiteral("Supporting package content"),
        QStringLiteral("Provides supporting content used by the embedded Harken Unity package."),
        QStringLiteral("The installed package will contain the supporting data expected by this SDK version.")};
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
        return QStringLiteral("This content is missing from the Unity project and will be added from the verified SDK.");
    }
    if (action == QStringLiteral("replace")) {
        return QStringLiteral("The installed content differs from the verified SDK source and will be updated to the selected version.");
    }
    if (action == QStringLiteral("remove")) {
        return QStringLiteral("A previous Harken SDK installation managed this content, but the current SDK no longer uses it; removing it avoids obsolete code or runtime conflicts.");
    }
    return QStringLiteral("This content exists only in the Unity project and is not managed by the selected SDK source, so it will remain unchanged.");
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
        const FileExplanation explanation = explainSdkFile(change.relative);
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

bool writeInstalledState(const QString& stageRoot,
                         const QString& version,
                         const QStringList& managedFiles,
                         const QMap<QString, QByteArray>& hashes,
                         QString* error)
{
    QJsonObject root;
    root[QStringLiteral("schema_version")] =
        kIntegrationStateVersion;
    root[QStringLiteral("package_name")] = QLatin1String(kPackageName);
    root[QStringLiteral("package_version")] = version;
    root[QStringLiteral("installed_at_utc")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    root[QStringLiteral("managed_files")] = QJsonArray::fromStringList(managedFiles);
    QJsonObject fileHashes;
    for (auto it = hashes.cbegin(); it != hashes.cend(); ++it)
        fileHashes[it.key()] = QString::fromLatin1(it.value());
    root[QStringLiteral("sha256")] = fileHashes;

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

bool migrationSnapshot(const QString& root, QJsonObject* snapshot, QString* error)
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
            *error = QStringLiteral("The SDK contains a path that cannot be migrated safely.");
            return false;
        }
        if (entries.fileInfo().isDir()) {
            directories << relative;
        } else {
            const FileDigest digest = sha256File(path);
            if (digest.status != ReadStatus::Ok) {
                *error = QStringLiteral("Cannot read SDK file for migration: %1").arg(path);
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

bool matchesMigrationSnapshot(const QString& root, const QJsonObject& expected, QString* error)
{
    QJsonObject actual;
    if (!migrationSnapshot(root, &actual, error)) return false;
    if (actual != expected) {
        *error = QStringLiteral("SDK contents changed during migration. No existing files will be overwritten.");
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
    if (QFileInfo::exists(QDir(project).filePath(QStringLiteral("Temp/UnityLockfile")))) {
        *error = QStringLiteral("Close Unity before migrating or recovering the SDK.");
        return false;
    }
    if (!UnityInstallation::validateTarget(project, from, error)
        || !UnityInstallation::validatePath(from + QStringLiteral(".meta"), error)) return false;
    if (UnityInstallation::containsPath(from, to) || UnityInstallation::containsPath(to, from)) {
        *error = QStringLiteral("The current and new SDK locations must be separate directories without containment.");
        return false;
    }
    if (!emptyMigrationDestination(to, error)) return false;
    const auto discovery = UnityInstallation::discover(project);
    if (!discovery.errors.isEmpty()) {
        *error = discovery.errors.join(QLatin1Char('\n'));
        return false;
    }
    if (!discovery.sdkReferences.isEmpty()) {
        *error = QStringLiteral("Resolve SDK package dependencies before migration to avoid loading another SDK or breaking a package reference.");
        return false;
    }
    for (const QString& reference : discovery.localPackagePaths) {
        if (UnityInstallation::containsPath(from, reference) || UnityInstallation::containsPath(reference, from)
            || UnityInstallation::containsPath(to, reference) || UnityInstallation::containsPath(reference, to)) {
            *error = QStringLiteral("Resolve SDK package dependencies before migration to avoid loading another SDK or breaking a package reference.");
            return false;
        }
    }
    const OwnershipRecord ownership = previousManagedFiles(from);
    if (ownership.status != ReadStatus::Ok || ownership.managedFiles.isEmpty()) {
        *error = QStringLiteral("A valid SDK ownership record is required for migration.");
        return false;
    }
    const JsonRead manifest = readJsonObject(QDir(from).filePath(QLatin1String(kIntegrationManifest)));
    const JsonRead package = readJsonObject(QDir(from).filePath(QStringLiteral("package.json")));
    if (manifest.status != ReadStatus::Ok || package.status != ReadStatus::Ok
        || manifest.object.value(QStringLiteral("schema_version")).toInt() != 3
        || manifest.object.value(QStringLiteral("package_name")).toString() != QLatin1String(kPackageName)
        || package.object.value(QStringLiteral("name")).toString() != QLatin1String(kPackageName)) {
        *error = QStringLiteral("The installed SDK identity or integration manifest is invalid.");
        return false;
    }
    const QString relative = QDir(project).relativeFilePath(to);
    if (relative != UnityInstallation::defaultRelativePath() && relative != QStringLiteral("Assets/HarkenSDK")
        && !manifest.object.value(QStringLiteral("supports_custom_install_path")).toBool()) {
        *error = QStringLiteral("This SDK version does not support a custom installation location.");
        return false;
    }
    const QJsonObject dependencies = package.object.value(QStringLiteral("dependencies")).toObject();
    for (auto it = dependencies.begin(); it != dependencies.end(); ++it) {
        if (it.value().toString().startsWith(QStringLiteral("file:"))) {
            *error = QStringLiteral("The installed SDK has local package dependencies. Resolve them before migration.");
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

bool recoverSdkMigration(const QString& project, const QJsonObject& record, bool apply, QString* error)
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
        if (error->isEmpty()) *error = QStringLiteral("The SDK migration record is invalid. Restore the verified backup manually.");
        return false;
    }
    if (QFileInfo::exists(QDir(project).filePath(QStringLiteral("Temp/UnityLockfile")))) {
        *error = QStringLiteral("Close Unity before migrating or recovering the SDK.");
        return false;
    }
    if (!validatePath(from + QStringLiteral(".meta"), error)
        || !validatePath(to + QStringLiteral(".meta"), error)) return false;
    const auto sourceOwnership = previousManagedFiles(from);
    const bool atSource = sourceOwnership.status == ReadStatus::Ok && !sourceOwnership.managedFiles.isEmpty();
    const QString current = atSource ? from : to;
    const QString other = atSource ? to : from;
    const auto ownership = atSource ? sourceOwnership : previousManagedFiles(to);
    if (ownership.status != ReadStatus::Ok || ownership.managedFiles.isEmpty()
        || (QDir(other).exists() && !QDir(other).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))) {
        *error = QStringLiteral("SDK migration recovery found conflicting or missing files. Preserve both locations and restore the verified backup manually.");
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
        *error = QStringLiteral("SDK directory metadata changed. Recovery will not overwrite Unity GUIDs.");
        return false;
    }
    if (!apply) return true;
    QDir filesystem;
    if ((!atSource && QDir(from).exists() && !filesystem.rmdir(from))
        || (metaAtTarget && !QFile::rename(toMeta, fromMeta))
        || (!atSource && !VNH::renameIntegrationDirectory(filesystem.filePath(to), filesystem.filePath(from)))
        || (record.value(QStringLiteral("target_existed")).toBool() && !QDir().mkpath(to))) {
        *error = QStringLiteral("The SDK could not be restored to its previous location. Close programs using these files and retry recovery.");
        return false;
    }
    if (!matchesMigrationSnapshot(from, record.value(QStringLiteral("inventory")).toObject(), error)) return false;
    if ((!expectedMeta.isEmpty() && !sha256File(fromMeta).matches(expectedMeta))
        || (expectedMeta.isEmpty() && QFileInfo::exists(fromMeta)) || QFileInfo::exists(toMeta)) {
        *error = QStringLiteral("SDK directory metadata changed. Recovery will not overwrite Unity GUIDs.");
        return false;
    }
    if (!QFile::remove(migrationJournalPath(project))) {
        *error = QStringLiteral("The SDK migration record could not be cleared. Retry recovery before continuing.");
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
    return QStringLiteral("%1 missing SDK file(s) will be added; %2 existing SDK file(s) "
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

QString UnityIntegrationService::locateSdkSource(const QString& preferredPath)
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
        if (baseName.compare(QStringLiteral("HarkenSDK"), Qt::CaseInsensitive) == 0)
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
        const QByteArray fromEnv = qgetenv("HARKEN_UNITY_SDK_PATH");
        if (!fromEnv.isEmpty()) candidates << QString::fromLocal8Bit(fromEnv);

        QDir cursor(QCoreApplication::applicationDirPath());
        candidates << cursor.filePath(QStringLiteral("HarkenSDK/Unity/Harken"));
        candidates << cursor.filePath(QStringLiteral("HarkenSDK"));
        for (int i = 0; i < 6; ++i) {
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
        finishStep(StepStatus::Failed, QStringLiteral("SDK migration could not be completed."));
        return result;
    };
    beginStep("migration_review", "Review SDK migration");
    if (request.unityProjectPath.trimmed().isEmpty()
        || !QDir(project).exists(QStringLiteral("Assets"))
        || !QDir(project).exists(QStringLiteral("Packages"))
        || !QDir(project).exists(QStringLiteral("ProjectSettings")))
        return fail(QStringLiteral("Selected directory is not a Unity project root."));
    if (!lockUnityProject(project, &operationLock, &error)) return fail(error);
    if (request.removeIntegration || request.checkOnly)
        return fail(QStringLiteral("Migration cannot be combined with installation, removal or check-only mode."));

    QJsonObject record;
    QString from;
    QString to;
    if (request.recoverMigration) {
        const JsonRead journal = readJsonObject(migrationJournalPath(project));
        if (journal.status != ReadStatus::Ok)
            return fail(QStringLiteral("The SDK migration record is invalid. Restore the verified backup manually."));
        record = journal.object;
        if (!recoverSdkMigration(project, record, false, &error)) return fail(error);
        result.sdkInstallPath = record.value(QStringLiteral("source")).toString();
        result.migrationTargetPath = record.value(QStringLiteral("target")).toString();
        result.migrationBackupPath = record.value(QStringLiteral("backup")).toString();
    } else {
        if (request.migrationTargetPath.trimmed().isEmpty())
            return fail(QStringLiteral("Select a new SDK installation location."));
        if (!resolveTarget(project, requestedInstallPath(request), &from, &error)
            || !resolveTarget(project, request.migrationTargetPath, &to, &error)
            || !validateMigration(project, from, to, &error)) return fail(error);
        result.sdkInstallPath = QDir(project).relativeFilePath(from);
        result.migrationTargetPath = QDir(project).relativeFilePath(to);
        result.migrationBackupPath = request.migrationBackupPath.trimmed().isEmpty()
            ? defaultIntegrationBackupPath(project) : cleanAbsolutePath(request.migrationBackupPath);
        if (!validateMigrationBackup(project, result.migrationBackupPath, &error)) return fail(error);
        QJsonObject inventory;
        if (!migrationSnapshot(from, &inventory, &error)) return fail(error);
        QByteArray metaHash;
        if (QFileInfo::exists(from + QStringLiteral(".meta"))) {
            const FileDigest digest = sha256File(from + QStringLiteral(".meta"));
            if (digest.status != ReadStatus::Ok)
                return fail(QStringLiteral("The SDK directory metadata could not be read."));
            metaHash = digest.hash;
        }
        record = QJsonObject{
            {QStringLiteral("schema_version"), 1}, {QStringLiteral("project"), project},
            {QStringLiteral("source"), result.sdkInstallPath},
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
    result.steps.back().artifacts = {QDir(project).filePath(result.sdkInstallPath),
                                    QDir(project).filePath(result.migrationTargetPath), result.migrationBackupPath};
    if (!request.confirmMigration) {
        result.requiresConfirmation = true;
        finishStep(StepStatus::Warning, request.recoverMigration
            ? QStringLiteral("Recovery will restore the SDK to its previous location without changing project relationships.")
            : QStringLiteral("Migration will preserve all installed files, Unity GUIDs and project relationships."));
        return result;
    }
    if (request.migrationPlanHash.isEmpty() || request.migrationPlanHash != result.migrationPlanHash)
        return fail(QStringLiteral("The migration plan changed after review. Review the migration again."));
    finishStep(StepStatus::Succeeded, QStringLiteral("The migration plan passed safety checks."));
    if (request.recoverMigration) {
        beginStep("recover_migration", "Recover SDK migration");
        if (!recoverSdkMigration(project, record, true, &error)) return fail(error);
        result.migrationRecovered = true;
        result.success = true;
        finishStep(StepStatus::Succeeded, QStringLiteral("The SDK was restored to its previous location. Project relationships were kept."));
        return result;
    }

    beginStep("migration_backup", "Back up installed SDK");
    const QString backup = QDir(result.migrationBackupPath).filePath(QStringLiteral("SDK"));
    const QJsonObject inventory = record.value(QStringLiteral("inventory")).toObject();
    const QByteArray metaHash = record.value(QStringLiteral("root_meta_sha256")).toString().toLatin1();
    const QString fromMeta = from + QStringLiteral(".meta");
    const QString toMeta = to + QStringLiteral(".meta");
    if (!validateMigration(project, from, to, &error)
        || !validateMigrationBackup(project, result.migrationBackupPath, &error)) return fail(error);
    if (!QDir().mkpath(backup)) return fail(QStringLiteral("The SDK migration backup could not be created."));
    for (const QJsonValue& directory : inventory.value(QStringLiteral("directories")).toArray()) {
        if (!QDir().mkpath(QDir(backup).filePath(directory.toString())))
            return fail(QStringLiteral("The SDK migration backup could not be created."));
    }
    if (!copyFiles(from, backup, inventory.value(QStringLiteral("files")).toObject().keys(), &error)
        || (!metaHash.isEmpty() && !copyFileAtomically(fromMeta, backup + QStringLiteral(".meta"), &error))
        || !matchesMigrationSnapshot(backup, inventory, &error)) return fail(error);
    if (!metaHash.isEmpty() && !sha256File(backup + QStringLiteral(".meta")).matches(metaHash))
        return fail(QStringLiteral("SDK directory metadata backup verification failed."));
    if (!VNH::writeIntegrationFile(QDir(result.migrationBackupPath).filePath(QStringLiteral("migration.json")), recordBytes, &error))
        return fail(error);
    result.steps.back().artifacts << result.migrationBackupPath;
    finishStep(StepStatus::Succeeded, QStringLiteral("All installed SDK files and directory metadata were backed up and verified."));

    beginStep("migrate_sdk", "Move installed SDK");
    if (!validateMigration(project, from, to, &error)
        || !matchesMigrationSnapshot(from, inventory, &error)) return fail(error);
    if ((!metaHash.isEmpty() && !sha256File(fromMeta).matches(metaHash))
        || (metaHash.isEmpty() && QFileInfo::exists(fromMeta)))
        return fail(QStringLiteral("SDK directory metadata changed. Migration was cancelled."));
    if (!QDir().mkpath(QFileInfo(to).absolutePath()))
        return fail(QStringLiteral("The SDK migration destination could not be created."));
    if (!VNH::writeIntegrationFile(migrationJournalPath(project), recordBytes, &error)) return fail(error);
    const auto rollback = [&](const QString& cause) {
        QString recoveryError;
        if (recoverSdkMigration(project, record, true, &recoveryError))
            return fail(cause + QLatin1Char('\n') + QStringLiteral("The SDK was restored to its previous location. Project relationships were kept."));
        result.steps.back().remediation = QStringLiteral("Use Recover to finish the interrupted migration before installing, updating or removing the SDK.");
        result.steps.back().artifacts << result.migrationBackupPath;
        return fail(cause + QLatin1Char('\n') + recoveryError);
    };
    QDir filesystem;
    if ((QDir(to).exists() && !filesystem.rmdir(to)) || !VNH::renameIntegrationDirectory(filesystem.filePath(from), filesystem.filePath(to)))
        return rollback(QStringLiteral("The SDK directory could not be moved."));
    if (!metaHash.isEmpty() && !QFile::rename(fromMeta, toMeta))
        return rollback(QStringLiteral("The SDK directory metadata could not be moved."));
    finishStep(StepStatus::Succeeded, QStringLiteral("The existing SDK directory was moved without replacing its contents."));
    beginStep("verify_migration", "Verify SDK migration");
    if (!matchesMigrationSnapshot(to, inventory, &error)) return rollback(error);
    if ((!metaHash.isEmpty() && !sha256File(toMeta).matches(metaHash))
        || (metaHash.isEmpty() && QFileInfo::exists(toMeta)) || QFileInfo::exists(fromMeta)
        || QFileInfo::exists(from))
        return rollback(QStringLiteral("SDK directory metadata changed. Migration was cancelled."));
    const auto discovery = discover(project);
    if (!discovery.errors.isEmpty() || discovery.installations.size() != 1
        || !samePath(discovery.installations.front().path, to) || !discovery.sdkReferences.isEmpty())
        return rollback(QStringLiteral("The Unity project changed during migration. Review SDK paths and dependencies again."));
    if (!QFile::remove(migrationJournalPath(project)))
        return rollback(QStringLiteral("The SDK migration record could not be cleared. Retry recovery before continuing."));
    result.sdkInstallPath = result.migrationTargetPath;
    result.integrationMigrated = true;
    result.success = true;
    finishStep(StepStatus::Succeeded, QStringLiteral("SDK migration completed. All installed files, Unity GUIDs and project relationships were kept."));
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
    bool unityValidated = false;
    QString blockingTitle;
    QString targetSdk;
    std::unique_ptr<QLockFile> operationLock;
    QStringList removalFiles;
    QStringList preservedFiles;
    QStringList sdkBackupFiles;
    QStringList installedFiles;

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
        if (!prepareInstallation(request, result, &targetSdk, &operationLock, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The SDK installation location is not safe to use.");
            step.cause = error;
            step.remediation = QStringLiteral("Resolve the reported path or SDK conflict, then check again.");
            return;
        }
        unityValidated = true;
        step.summary = QStringLiteral("Unity project layout and write access are valid.");
        step.artifacts << root;
    });

    runStep([&](StepResult& step) {
        if (!QDir(targetSdk).exists()) {
            step.summary = QStringLiteral("No embedded Harken SDK is installed in this Unity project.");
            return;
        }
        result.integrationFound = true;
        result.targetInstalled = true;

        const QString statePath = QDir(targetSdk).filePath(
            QLatin1String(kInstalledState));
        const JsonRead state = readJsonObject(statePath);
        const OwnershipRecord ownership = previousManagedFiles(targetSdk);
        const QSet<QString> managedFileKeys =
            comparisonPathKeys(ownership.managedFiles);
        if (state.unreadable()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The Harken ownership record could not be read.");
            step.cause = QStringLiteral("%1 exists but cannot be opened, so the Harken integration cannot tell which files it owns.")
                             .arg(statePath);
            step.remediation = QStringLiteral("Close Unity and any process holding this file, then run removal again. Nothing was removed.");
            return;
        }
        if (ownership.status != ReadStatus::Ok ||
            state.object.value(QStringLiteral("package_name")).toString() !=
                QLatin1String(kPackageName) || ownership.managedFiles.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The installed package has no trustworthy Harken ownership record.");
            step.cause = QStringLiteral("The Harken integration cannot distinguish managed SDK files from project-specific files inside %1.")
                             .arg(targetSdk);
            step.remediation = QStringLiteral("Use Unity or source control to back up this folder, then remove it manually. One-click removal is intentionally blocked to avoid deleting user files.");
            return;
        }

        QSet<QString> declaredRuntimes;
        const QString manifestPath = QDir(targetSdk).filePath(
            QLatin1String(kIntegrationManifest));
        const JsonRead manifest = readJsonObject(manifestPath);
        if (manifest.unreadable()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The installed integration manifest could not be read.");
            step.cause = QStringLiteral("%1 exists but cannot be opened, so the runtime files declared by the Harken SDK cannot be enumerated.")
                             .arg(manifestPath);
            step.remediation = QStringLiteral("Close Unity and any process holding this file, then run removal again. Nothing was removed.");
            return;
        }
        for (const QJsonValue& value :
             manifest.object.value(QStringLiteral("native_plugins")).toArray()) {
            const QJsonObject plugin = value.toObject();
            for (const QJsonValue& alternative :
                 plugin.value(QStringLiteral("alternatives")).toArray()) {
                const QString relative = alternative.toString();
                if (!isSafeRelativePath(relative)) {
                    step.status = StepStatus::Failed;
                    step.summary = QStringLiteral("The installed SDK manifest contains an unsafe runtime path.");
                    step.cause = relative;
                    return;
                }
                declaredRuntimes.insert(normalizePath(relative));
            }
        }

        installedFiles = allRelativeFiles(targetSdk);
        QSet<QString> removeSet;
        removeSet.insert(QLatin1String(kInstalledState));
        for (const QString& relative : installedFiles) {
            if (discardableOsMetadata(relative) ||
                managedFileKeys.contains(comparisonPathKey(relative)) ||
                matchesDeclaredArtifact(relative, declaredRuntimes) ||
                !harkenRuntimeIdentity(relative).isEmpty()) {
                removeSet.insert(relative);
            }
        }
        for (const QString& relative : installedFiles) {
            if (!relative.endsWith(QStringLiteral(".meta"),
                                   Qt::CaseInsensitive)) {
                continue;
            }
            QString base = relative;
            base.chop(5);
            if (removeSet.contains(base) ||
                matchesDeclaredArtifact(base, declaredRuntimes)) {
                removeSet.insert(relative);
            }
        }
        for (const QString& relative : installedFiles) {
            if (!relative.endsWith(QStringLiteral(".meta"),
                                   Qt::CaseInsensitive) ||
                removeSet.contains(relative)) {
                continue;
            }
            QString directory = relative;
            directory.chop(5);
            if (!QFileInfo(QDir(targetSdk).filePath(directory)).isDir()) continue;
            const QString prefix = directory + QLatin1Char('/');
            bool hasPreservedChild = false;
            for (const QString& candidate : installedFiles) {
                if (!removeSet.contains(candidate) &&
                    candidate != relative && candidate.startsWith(prefix)) {
                    hasPreservedChild = true;
                    break;
                }
            }
            if (!hasPreservedChild) removeSet.insert(relative);
        }

        for (const QString& relative : installedFiles) {
            if (removeSet.contains(relative))
                removalFiles << relative;
            else
                preservedFiles << relative;
        }
        result.filesRemoved = removalFiles.size();
        result.targetOnlyFilesPreserved = preservedFiles.size();
        sdkBackupFiles = ownership.managedFiles.values();
        sdkBackupFiles.sort(Qt::CaseSensitive);
        result.removalBackupPath = request.removalBackupPath.trimmed().isEmpty()
            ? defaultIntegrationBackupPath(result.unityProjectPath)
            : cleanAbsolutePath(request.removalBackupPath);
        step.summary = QStringLiteral("Ready to remove %1 Harken SDK/runtime file(s) and preserve %2 project-specific file(s).")
                           .arg(result.filesRemoved)
                           .arg(result.targetOnlyFilesPreserved);
        step.details << QStringLiteral("Installed package: %1").arg(targetSdk);
        if (!removalFiles.isEmpty()) {
            step.details << QStringLiteral("Harken SDK and runtime files that will be removed:\n%1")
                                .arg(groupedLocations(
                                    removalFiles,
                                    result.sdkInstallPath)
                                         .join(QLatin1Char('\n')));
        }
        if (!preservedFiles.isEmpty()) {
            step.details << QStringLiteral("Project-specific files that will be preserved:\n%1")
                                .arg(groupedLocations(
                                    preservedFiles,
                                    result.sdkInstallPath)
                                         .join(QLatin1Char('\n')));
        }
        step.details << QStringLiteral("Safety backup: %1\n"
                                       "To integrate again later, select or drop this SDK source: %2")
                            .arg(QDir::toNativeSeparators(result.removalBackupPath),
                                 QDir::toNativeSeparators(
                                     QDir(result.removalBackupPath)
                                         .filePath(QStringLiteral("ReintegrationSDK"))));
        step.artifacts << targetSdk;
        step.artifacts << result.removalBackupPath;
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
            step.cause = QStringLiteral("Removing the embedded package changes which Harken code and native runtimes Unity can load.");
            step.remediation = QStringLiteral("Review the listed paths and backup location, then approve removal or cancel without changing the Unity project.");
            return;
        }

        if (result.removalBackupPath.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("A safe backup location could not be created.");
            step.cause = QStringLiteral("The operating system did not provide a writable application-data location.");
            step.remediation = QStringLiteral("Check the local application-data folder and try again.");
            return;
        }
        if (QFileInfo::exists(result.removalBackupPath)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The planned backup folder already exists.");
            step.cause = result.removalBackupPath;
            step.remediation = QStringLiteral("Run the removal review again to create a new timestamped backup location.");
            return;
        }

        QString safetyError;
        if (!UnityInstallation::validateTarget(result.unityProjectPath, targetSdk, &safetyError, true)
            || !UnityInstallation::validatePath(result.removalBackupPath, &safetyError)
            || UnityInstallation::containsPath(result.unityProjectPath, result.removalBackupPath)
            || UnityInstallation::containsPath(result.removalBackupPath, result.unityProjectPath)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The SDK or backup location is not safe to use.");
            step.cause = safetyError.isEmpty()
                ? QStringLiteral("Choose a backup directory outside the Unity project.") : safetyError;
            return;
        }
        const QString targetLabel = result.sdkInstallPath;
        const QString unitySnapshot = QDir(result.removalBackupPath).filePath(
            QStringLiteral("UnityProject/%1").arg(targetLabel));
        const QString backupSdk = QDir(result.removalBackupPath).filePath(
            QStringLiteral("ReintegrationSDK"));
        const QString packageMeta = targetSdk + QStringLiteral(".meta");
        const QString backupPackageMeta = QDir(result.removalBackupPath).filePath(
            QStringLiteral("UnityProject/%1.meta").arg(targetLabel));
        QString backupError;
        const auto discardIncompleteBackup = [&]() {
            QDir(result.removalBackupPath).removeRecursively();
        };
        const auto packageMetaCopyVerified = [&]() {
            if (!QFileInfo::exists(packageMeta)) return true;
            const FileDigest original = sha256File(packageMeta);
            const FileDigest copied = sha256File(backupPackageMeta);
            if (original.status != ReadStatus::Ok ||
                copied.status == ReadStatus::Unreadable) {
                backupError = QStringLiteral("Cannot read %1 while verifying the backup copy of the package metadata")
                                  .arg(original.status == ReadStatus::Ok
                                           ? backupPackageMeta : packageMeta);
                return false;
            }
            if (!copied.matches(original.hash)) {
                backupError = QStringLiteral("Backup verification failed for %1")
                                  .arg(packageMeta);
                return false;
            }
            return true;
        };
        if (!copyFiles(targetSdk, unitySnapshot, installedFiles, &backupError) ||
            !copyFiles(targetSdk, backupSdk, sdkBackupFiles, &backupError) ||
            (QFileInfo::exists(packageMeta) &&
             !copyFileAtomically(packageMeta, backupPackageMeta, &backupError)) ||
            !verifyCopiedFiles(targetSdk, unitySnapshot, installedFiles,
                               &backupError) ||
            !verifyCopiedFiles(targetSdk, backupSdk, sdkBackupFiles, &backupError) ||
            !packageMetaCopyVerified()) {
            discardIncompleteBackup();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The safety backup could not be completed, so nothing was removed.");
            step.cause = backupError;
            step.remediation = QStringLiteral("Check free disk space and write permission for the application-data folder, then retry.");
            return;
        }

        const JsonRead backupPackage = readJsonObject(
            QDir(backupSdk).filePath(QStringLiteral("package.json")));
        const JsonRead backupManifest = readJsonObject(
            QDir(backupSdk).filePath(QLatin1String(kIntegrationManifest)));
        if (backupPackage.unreadable() || backupManifest.unreadable()) {
            discardIncompleteBackup();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The safety backup could not be read back, so nothing was removed.");
            step.cause = QStringLiteral("The backed-up package identity or integration manifest exists but cannot be opened under %1.")
                             .arg(backupSdk);
            step.remediation = QStringLiteral("Check the application-data folder for file locks and permissions, then retry.");
            return;
        }
        if (backupPackage.object.value(QStringLiteral("name")).toString() !=
                QLatin1String(kPackageName) || backupManifest.object.isEmpty()) {
            discardIncompleteBackup();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The safety backup is not a reusable Harken SDK, so nothing was removed.");
            step.cause = QStringLiteral("The backed-up package identity or integration manifest is missing.");
            step.remediation = QStringLiteral("Run Check only and repair the installed SDK before trying to remove it.");
            return;
        }

        const QString generatedAt =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QJsonObject backupRecord;
        backupRecord[QStringLiteral("schema_version")] = 1;
        backupRecord[QStringLiteral("created_at_utc")] = generatedAt;
        backupRecord[QStringLiteral("source_unity_project")] =
            result.unityProjectPath;
        backupRecord[QStringLiteral("unity_package_snapshot")] = unitySnapshot;
        backupRecord[QStringLiteral("reintegrate_sdk_source")] = backupSdk;
        backupRecord[QStringLiteral("harken_sdk_files")] = sdkBackupFiles.size();
        backupRecord[QStringLiteral("unity_package_files")] = installedFiles.size();
        const QByteArray instructions =
            QStringLiteral("Harken Unity integration safety backup\n\n"
                           "Created: %1\n"
                           "Original Unity project: %2\n\n"
                           "To integrate Harken again:\n"
                           "1. Open the Harken Unity integration window.\n"
                           "2. Select or drag this folder into SDK source:\n"
                           "   %3\n"
                           "3. Select the Unity project and click Check and integrate.\n\n"
                           "UnityProject contains an exact snapshot copied from the selected Unity project before removal.\n"
                           "ReintegrationSDK was extracted from that snapshot for the SDK source field.\n"
                           "No files were taken from the Harken Studio installation.\n"
                           "Files not owned by Harken were also kept unchanged in the Unity project.\n")
                .arg(generatedAt, result.unityProjectPath, backupSdk)
                .toUtf8();
        if (!VNH::writeIntegrationFile(
                QDir(result.removalBackupPath).filePath(
                    QStringLiteral("backup.json")),
                QJsonDocument(backupRecord).toJson(QJsonDocument::Indented),
                &backupError) ||
            !VNH::writeIntegrationFile(
                QDir(result.removalBackupPath).filePath(
                    QStringLiteral("README-FIRST.txt")),
                instructions, &backupError)) {
            discardIncompleteBackup();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The safety backup record could not be written, so nothing was removed.");
            step.cause = backupError;
            step.remediation = QStringLiteral("Check free disk space and write permission for the application-data folder, then retry.");
            return;
        }
        result.removalBackupCreated = true;
        step.artifacts << result.removalBackupPath << backupSdk;

        QDir packages(QFileInfo(targetSdk).absolutePath());
        const QString targetName = QFileInfo(targetSdk).fileName();
        const QString stageName = QStringLiteral(".%1.removing-%2")
                                      .arg(targetName,
                                           QUuid::createUuid().toString(
                                               QUuid::WithoutBraces));
        const QString stageRoot = packages.filePath(stageName);
        if (!VNH::renameIntegrationDirectory(packages.filePath(targetName), packages.filePath(stageName))) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not detach the installed Harken SDK.");
            step.cause = targetSdk;
            step.remediation = QStringLiteral("Close Unity and any process using files under %1, then retry.")
                                   .arg(targetLabel);
            return;
        }

        QString copyError;
        if (!preservedFiles.isEmpty() &&
            !copyFiles(stageRoot, targetSdk, preservedFiles, &copyError)) {
            QDir(targetSdk).removeRecursively();
            VNH::renameIntegrationDirectory(packages.filePath(stageName), packages.filePath(targetName));
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not preserve project-specific files, so removal was cancelled.");
            step.cause = copyError;
            step.remediation = QStringLiteral("Check project permissions and free disk space. The original package was restored.");
            return;
        }

        if (!QDir(stageRoot).removeRecursively()) {
            step.status = StepStatus::Warning;
            step.summary = QStringLiteral("The active integration was removed, but temporary cleanup is incomplete.");
            step.remediation = QStringLiteral("After closing Unity, delete the hidden temporary folder listed below if it remains.");
            step.artifacts << stageRoot;
        } else {
            step.summary = QStringLiteral("Backed up and removed the Harken SDK and native runtimes; preserved %1 project-specific file(s).")
                               .arg(result.targetOnlyFilesPreserved);
        }
        if (preservedFiles.isEmpty()) QFile::remove(targetSdk + QStringLiteral(".meta"));
        result.integrationRemoved = true;
        result.targetInstalled = false;
        result.targetCurrent = false;
    });

    runStep([&](StepResult& step) {
        if (!unityValidated) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("No removal report was written because the Unity project was not valid.");
            return;
        }
        const QString reportDir = QDir(result.unityProjectPath)
                                      .filePath(QStringLiteral("Library/HarkenIntegration"));
        result.jsonReportPath = QDir(reportDir).filePath(
            QStringLiteral("latest-removal.json"));
        result.markdownReportPath = QDir(reportDir).filePath(
            QStringLiteral("latest-removal.md"));

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
        root[QStringLiteral("sdk_install_path")] = result.sdkInstallPath;
        root[QStringLiteral("removal_backup_path")] = result.removalBackupPath;
        root[QStringLiteral("files_removed")] = result.filesRemoved;
        root[QStringLiteral("project_files_preserved")] =
            result.targetOnlyFilesPreserved;
        QJsonArray steps;
        for (const StepResult& item : result.steps) steps.append(stepToJson(item));
        root[QStringLiteral("steps")] = steps;

        QString jsonFileError;
        if (!VNH::writeIntegrationFile(result.jsonReportPath,
                QJsonDocument(root).toJson(QJsonDocument::Indented), &jsonFileError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The removal report could not be written.");
            step.cause = jsonFileError;
            step.remediation = QStringLiteral("Check write permission for Unity Library/HarkenIntegration.");
            return;
        }

        QString markdown = QStringLiteral("# Harken Unity integration removal report\n\n");
        markdown += QStringLiteral("- Generated: %1\n- Unity project: `%2`\n- Safety backup: `%3`\n- Integration removed: %4\n- Harken files removed: %5\n- Project-specific files preserved: %6\n\n")
                        .arg(root[QStringLiteral("generated_at_utc")].toString(),
                             result.unityProjectPath,
                             result.removalBackupPath,
                             result.integrationRemoved ? QStringLiteral("yes")
                                                       : QStringLiteral("no"))
                        .arg(result.filesRemoved)
                        .arg(result.targetOnlyFilesPreserved);
        QString markdownFileError;
        if (!VNH::writeIntegrationFile(result.markdownReportPath,
                markdown.toUtf8(), &markdownFileError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("JSON report was written, but the Markdown removal report failed.");
            step.cause = markdownFileError;
            step.remediation = QStringLiteral("Check write permission for Unity Library/HarkenIntegration.");
            return;
        }
        step.summary = QStringLiteral("Wrote machine-readable and human-readable removal reports.");
        step.artifacts << result.jsonReportPath << result.markdownReportPath;
    }, true);

    while (nextStep < result.steps.size()) runStep([](StepResult&) {});
    result.success = !blocked && !result.requiresConfirmation;
    for (const StepResult& step : result.steps) {
        if (step.status == StepStatus::Failed) result.success = false;
        if (step.status == StepStatus::Warning) result.hasWarnings = true;
    }
    return result;
}

UnityIntegrationService::Result UnityIntegrationService::run(const Request& request)
{
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
    bool unityValidated = false;
    QString blockingTitle;
    QString targetSdk;
    std::unique_ptr<QLockFile> operationLock;
    QString sourceVersion;
    QStringList sourceFiles;
    QStringList preservedFiles;
    QStringList staleFiles;
    QMap<QString, QString> runtimeMetadataMoves;
    QMap<QString, QByteArray> sourceHashes;
    QSet<QString> declaredOptionalRuntimePaths;
    QString updateBackupRoot;
    bool installationActivated = false;

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
        if (!prepareInstallation(request, result, &targetSdk, &operationLock, &error)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The SDK installation location is not safe to use.");
            step.cause = error;
            step.remediation = QStringLiteral("Resolve the reported path or SDK conflict, then check again.");
            return;
        }
        unityValidated = true;
        step.summary = QStringLiteral("Unity project layout and write access are valid.");
        step.artifacts << root;
    });

    runStep([&](StepResult& step) {
        result.sdkSourcePath = locateSdkSource(request.sdkSourcePath);
        if (result.sdkSourcePath.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Harken Unity SDK could not be located.");
            if (!request.sdkSourcePath.trimmed().isEmpty()) {
                step.cause = QStringLiteral("The selected folder is not a valid com.harken.core SDK source: %1")
                                 .arg(cleanAbsolutePath(request.sdkSourcePath));
                step.remediation = QStringLiteral("Choose HarkenSDK, HarkenSDK/Unity/Harken, or a complete external Unity package root, or clear the SDK source field to use automatic discovery.");
            } else {
                step.cause = QStringLiteral("No valid com.harken.core package was found beside Harken Studio, in a repository parent, or at HARKEN_UNITY_SDK_PATH.");
                step.remediation = QStringLiteral("Use the SDK source field to choose or drop HarkenSDK, Unity/Harken, or a complete external Unity package root.");
            }
            return;
        }

        const QString sourceManifestPath = QDir(result.sdkSourcePath)
                                               .filePath(QLatin1String(kIntegrationManifest));
        QString safetyError;
        if (!UnityInstallation::validateTree(result.sdkSourcePath, &safetyError)
            || UnityInstallation::containsPath(result.sdkSourcePath, targetSdk)
            || UnityInstallation::containsPath(targetSdk, result.sdkSourcePath)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The SDK source and installation location are not safe to use.");
            step.cause = safetyError.isEmpty()
                ? QStringLiteral("SDK source and target must be separate directories without containment.") : safetyError;
            return;
        }
        const JsonRead manifestRead = readJsonObject(sourceManifestPath);
        if (manifestRead.unreadable()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The selected SDK integration manifest could not be read.");
            step.cause = QStringLiteral("%1 exists but cannot be opened.")
                             .arg(sourceManifestPath);
            step.remediation = QStringLiteral("Close any process holding this file and check the SDK folder permissions, then run Check only again.");
            return;
        }
        const QJsonObject manifest = manifestRead.object;
        if (result.sdkInstallPath != UnityInstallation::defaultRelativePath()
            && result.sdkInstallPath != QStringLiteral("Assets/HarkenSDK")
            && !manifest.value(QStringLiteral("supports_custom_install_path")).toBool()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("This SDK version does not support a custom installation location.");
            step.remediation = QStringLiteral("Choose a newer SDK or install to Packages/com.harken.core or Assets/HarkenSDK.");
            return;
        }
        if (manifest.value(QStringLiteral("schema_version")).toInt() != 3 ||
            manifest.value(QStringLiteral("package_name")).toString() !=
                QLatin1String(kPackageName)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The selected SDK integration manifest is missing or invalid.");
            step.cause = QStringLiteral("%1 does not declare the expected schema and package identity.")
                             .arg(QDir(result.sdkSourcePath).filePath(QLatin1String(kIntegrationManifest)));
            step.remediation = QStringLiteral("Choose a complete HarkenSDK/Unity/Harken package, or repair the selected SDK source before integrating.");
            return;
        }

        QStringList missing;
        const QJsonArray required = manifest.value(QStringLiteral("required_files")).toArray();
        for (const QJsonValue& value : required) {
            const QString relative = value.toString();
            if (!isSafeRelativePath(relative)) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("The SDK manifest contains an unsafe file path.");
                step.cause = relative;
                return;
            }
            if (relative.isEmpty() ||
                !existingArtifact(QDir(result.sdkSourcePath).filePath(relative))) {
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
                    step.summary = QStringLiteral("The SDK manifest contains an unsafe runtime path.");
                    step.cause = relative;
                    return;
                }
                alternatives << relative;
                if (!isHostPlatform && !relative.isEmpty())
                    declaredOptionalRuntimePaths.insert(relative);
                if (existingArtifact(QDir(result.sdkSourcePath).filePath(relative)))
                    found = true;
            }
            if (found) {
                bundledPlatforms << platform;
            } else if (isHostPlatform) {
                missingBlockingPlatforms << platform;
                missing << QStringLiteral("%1 native runtime (%2)")
                               .arg(platform, alternatives.join(QStringLiteral(" or ")));
                missingBlockingRuntimeDetails << missingRuntimeDetail(
                    result.sdkSourcePath, platform, alternatives);
            } else if (isExpectedPlatform) {
                missingExpectedPlatforms << platform;
                missingExpectedRuntimeDetails << missingRuntimeDetail(
                    result.sdkSourcePath, platform, alternatives);
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

        sourceFiles = relativeFiles(result.sdkSourcePath);
        if (sourceFiles.isEmpty()) missing << QStringLiteral("SDK file inventory");
        if (!missing.isEmpty()) {
            step.status = StepStatus::Failed;
            if (!missingBlockingPlatforms.isEmpty()) {
                step.summary = QStringLiteral("The selected SDK source is missing the verified game runtime for: %1. The Unity project was not changed.")
                                   .arg(missingBlockingPlatforms.join(QStringLiteral(", ")));
                step.cause = QStringLiteral("These runtime files must come from the selected SDK source, not from the Unity project. Integration stopped before copying anything because the Harken integration cannot verify an SDK assembled from incomplete source files.");
                step.remediation = QStringLiteral("Choose or drop HarkenSDK, HarkenSDK/Unity/Harken, or a complete external Unity package. If this is the SDK bundled with Harken Studio, repair or reinstall the editor package first. Do not copy unknown runtime files from an existing Unity project.");
                step.details = missingBlockingRuntimeDetails;
                if (missing.size() > missingBlockingPlatforms.size()) {
                    step.details << QStringLiteral("Other missing Harken SDK content: %1")
                                        .arg(missing.join(QStringLiteral("; ")));
                }
            } else {
                step.summary = QStringLiteral("The selected Unity SDK source is incomplete. The Unity project was not changed.");
                step.cause = QStringLiteral("Required Harken SDK content is missing from the selected source: %1")
                                 .arg(missing.join(QStringLiteral("; ")));
                step.remediation = QStringLiteral("Choose or drop a complete Unity/Harken package, then run Check only again. This problem cannot be repaired inside the Unity project.");
                step.details = missing;
            }
            return;
        }

        for (const QString& relative : sourceFiles) {
            const FileDigest digest = sha256File(
                QDir(result.sdkSourcePath).filePath(relative));
            if (digest.status != ReadStatus::Ok) {
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("A selected SDK source file could not be read.");
                step.cause = QDir(result.sdkSourcePath).filePath(relative);
                step.remediation = QStringLiteral("Check the selected SDK folder permissions and contents, then retry.");
                return;
            }
            sourceHashes.insert(relative, digest.hash);
        }
        sourceVersion = packageVersion(result.sdkSourcePath);
        step.summary = QStringLiteral("Validated SDK %1: %2 file(s); the %3 runtime required for this editor is present.")
                           .arg(sourceVersion, QString::number(sourceFiles.size()),
                                hostPlatform);
        step.details << QStringLiteral("Native runtimes available in this SDK source: %1")
                            .arg(bundledPlatforms.join(QStringLiteral(", ")));
        if (!missingExpectedPlatforms.isEmpty()) {
            step.status = StepStatus::Warning;
            step.summary += QStringLiteral(" Target runtimes not included: %1. Available platforms can still be integrated.")
                                .arg(missingExpectedPlatforms.join(QStringLiteral(", ")));
            step.cause = QStringLiteral("The %1 files in the selected SDK source are complete, but the listed target platforms have no verified runtime in this source. Each target is independent, so their absence does not prevent %1 integration.")
                             .arg(hostPlatform);
            step.remediation = QStringLiteral("You may continue integrating and use the available platforms. Before building for a missing target, select a complete SDK source that includes its verified runtime, then run Check only again. Do not copy unknown runtime files from another Unity project.");
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
        step.artifacts << result.sdkSourcePath;
    });

    runStep([&](StepResult& step) {
        const bool targetExists = QDir(targetSdk).exists()
            && !QDir(targetSdk).entryList(QDir::AllEntries | QDir::Hidden
                | QDir::System | QDir::NoDotAndDotDot).isEmpty();
        QVector<FileChange> changes;
        result.targetInstalled = targetExists;
        if (!targetExists) {
            result.filesAdded = sourceFiles.size();
            result.changesDetected = true;
            for (const QString& relative : sourceFiles)
                changes.push_back({QStringLiteral("add"), relative});
            step.details = describeFileChanges(
                changes, result.sdkInstallPath);
            result.fileChanges = changes;
            step.summary = QStringLiteral("No embedded Harken SDK is installed; %1 file(s) are ready to install.")
                               .arg(result.filesAdded);
            return;
        }

        const OwnershipRecord previousManaged = previousManagedFiles(targetSdk);
        if (previousManaged.status != ReadStatus::Ok) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The installed SDK could not be compared because its ownership record is missing, invalid or unreadable.");
            step.cause = QStringLiteral("%1 could not establish which files belong to Harken. Nothing was compared or changed.")
                             .arg(QDir(targetSdk).filePath(QLatin1String(kInstalledState)));
            step.remediation = QStringLiteral("Restore the ownership record from backup, or resolve file access errors, then check again.");
            return;
        }
        const QSet<QString> previousManagedKeys =
            comparisonPathKeys(previousManaged.managedFiles);
        QStringList targetFiles = allRelativeFiles(targetSdk);
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
                QDir(targetSdk).filePath(relative));
            if (digest.status == ReadStatus::Missing) {
                ++filesAdded;
                changes.push_back({QStringLiteral("add"), relative});
            } else if (digest.unreadable()) {
                unreadableTargets << relative;
            } else if (!digest.matches(sourceHashes.value(relative))) {
                ++filesChanged;
                changes.push_back({QStringLiteral("replace"), relative});
            }
        }
        if (!unreadableTargets.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("%1 installed SDK file(s) could not be read, so the comparison is incomplete.")
                               .arg(unreadableTargets.size());
            step.cause = QStringLiteral("These files exist inside %1 but cannot be opened, so the Harken integration cannot tell whether they still match the SDK source. Nothing was compared or changed.")
                             .arg(targetSdk);
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
                !runtimeIdentity.isEmpty() &&
                sourceRuntimeIdentities.contains(runtimeIdentity) &&
                !sourceKeys.contains(comparisonPathKey(runtimePath));
            if (obsoleteRuntimeAlias && relative != runtimePath) {
                const QString destination = sourceRuntimePaths.value(runtimeIdentity);
                const QString destinationMeta = destination + QStringLiteral(".meta");
                if (!QFileInfo::exists(QDir(targetSdk).filePath(destination)) &&
                    !QFileInfo::exists(QDir(targetSdk).filePath(destinationMeta)) &&
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
            changes, result.sdkInstallPath);
        result.fileChanges = changes;
        result.staleFilesToRemove = staleFiles.size();
        result.targetOnlyFilesPreserved = preservedFiles.size();
        result.changesDetected = result.filesAdded > 0 ||
                                 result.filesChanged > 0 ||
                                 !staleFiles.isEmpty() ||
                                 !projectSpecificFiles.isEmpty();

        const bool managedDiffers = result.filesAdded > 0 ||
                                    result.filesChanged > 0 ||
                                    !staleFiles.isEmpty();
        if (!managedDiffers) {
            result.targetAlreadyCurrent = true;
            if (preservedFiles.isEmpty()) {
                step.summary = QStringLiteral("The installed SDK exactly matches selected SDK source %1.")
                                   .arg(sourceVersion);
            } else if (projectSpecificFiles.isEmpty()) {
                step.summary = QStringLiteral("All Harken-managed files match SDK %1; %2 Unity metadata file(s) and %3 optional target runtime file(s) are retained as expected.")
                                   .arg(sourceVersion).arg(unityMetadataFiles.size())
                                   .arg(optionalRuntimeFiles.size());
            } else {
                step.status = StepStatus::Warning;
                step.summary = QStringLiteral("All Harken-managed files match SDK %1; %2 project-specific file(s) need review, while %3 Unity metadata and %4 optional runtime file(s) are retained normally.")
                                   .arg(sourceVersion).arg(projectSpecificFiles.size())
                                   .arg(unityMetadataFiles.size())
                                   .arg(optionalRuntimeFiles.size());
                step.cause = QStringLiteral("The project-specific files are outside Harken SDK ownership, so the check cannot determine whether they are still needed.");
                step.remediation = QStringLiteral("Review only the project-specific files listed below. Unity-generated metadata requires no action.");
            }
            return;
        }

        const QString counts = joinedCountSummary(
            result.filesAdded, result.filesChanged, staleFiles.size(),
            projectSpecificFiles.size(), unityMetadataFiles.size(),
            optionalRuntimeFiles.size());
        if (request.checkOnly) {
            step.status = StepStatus::Warning;
            step.summary = QStringLiteral("Check found SDK differences: %1.").arg(counts);
            step.cause = QStringLiteral("Check-only mode did not change %1.")
                             .arg(result.sdkInstallPath);
            step.remediation = QStringLiteral("Review the file list before confirming the SDK change.");
            return;
        }
        if (!request.confirmTargetChanges) {
            result.requiresConfirmation = true;
            step.status = StepStatus::Warning;
            step.summary = QStringLiteral("The existing SDK differs from selected SDK source %1: %2.")
                               .arg(sourceVersion, counts);
            step.cause = QStringLiteral("Updating will replace Harken-managed files. Target-only files will be retained.");
            step.remediation = QStringLiteral("Review this file list, then explicitly approve the update or cancel without changing the SDK.");
            return;
        }
        step.summary = QStringLiteral("Approved SDK update: %1.").arg(counts);
    });

    runStep([&](StepResult& step) {
        if (request.checkOnly) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Check-only mode requested; no SDK files were changed.");
            return;
        }
        if (result.requiresConfirmation) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("No SDK files were changed; explicit approval is required.");
            return;
        }
        if (result.targetAlreadyCurrent) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Installation is already current; no files were copied.");
            return;
        }

        QString safetyError;
        if (!UnityInstallation::validateTarget(result.unityProjectPath, targetSdk, &safetyError)
            || !UnityInstallation::validateTree(result.sdkSourcePath, &safetyError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The SDK paths changed after checking. No files were installed.");
            step.cause = safetyError;
            return;
        }
        QDir packages(QFileInfo(targetSdk).absolutePath());
        const QString targetName = QFileInfo(targetSdk).fileName();
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString stageName = QStringLiteral(".%1.staging-%2").arg(targetName, token);
        const QString backupName = QStringLiteral(".%1.backup-%2").arg(targetName, token);
        const QString stageRoot = packages.filePath(stageName);
        if (!QDir().mkpath(stageRoot)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not create an SDK staging directory.");
            step.cause = stageRoot;
            step.remediation = QStringLiteral("Check installation directory permissions and free disk space.");
            return;
        }

        QString error;
        if (!copyFiles(result.sdkSourcePath, stageRoot, sourceFiles, &error) ||
            (!preservedFiles.isEmpty() &&
             !copyFiles(targetSdk, stageRoot, preservedFiles, &error)) ||
            !writeInstalledState(stageRoot, sourceVersion, sourceFiles,
                                 sourceHashes, &error)) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Failed to stage the Unity SDK update.");
            step.cause = error;
            step.remediation = QStringLiteral("Check file locks, permissions, and disk space, then retry.");
            return;
        }
        QStringList stagingMismatches;
        QStringList stagingUnreadable;
        for (auto it = runtimeMetadataMoves.cbegin(); it != runtimeMetadataMoves.cend(); ++it) {
            const QString previous = QDir(targetSdk).filePath(it.value());
            const QString staged = QDir(stageRoot).filePath(it.key());
            const FileDigest digest = sha256File(previous);
            if (!QFile::copy(previous, staged) ||
                !sha256File(staged).matches(digest.hash)) {
                QDir(stageRoot).removeRecursively();
                step.status = StepStatus::Failed;
                step.summary = QStringLiteral("Could not preserve native plugin metadata for %1.").arg(it.key());
                step.remediation = QStringLiteral("Check file locks and permissions, then retry. The existing SDK was not changed.");
                return;
            }
        }
        for (const QString& relative : sourceFiles) {
            const FileDigest digest = sha256File(
                QDir(stageRoot).filePath(relative));
            if (digest.unreadable())
                stagingUnreadable << relative;
            else if (!digest.matches(sourceHashes.value(relative)))
                stagingMismatches << relative;
        }
        if (!stagingMismatches.isEmpty() || !stagingUnreadable.isEmpty()) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The staged Unity SDK failed checksum verification.");
            step.cause = stagingUnreadable.isEmpty()
                ? QStringLiteral("Staging mismatches: %1")
                      .arg(stagingMismatches.join(QStringLiteral(", ")))
                : QStringLiteral("Staged files that could not be read: %1")
                      .arg(stagingUnreadable.join(QStringLiteral(", ")));
            step.remediation = QStringLiteral("Repair or reselect the SDK source, check the storage device, then retry. The existing Unity SDK was not changed.");
            return;
        }

        const bool hadTarget = QDir(targetSdk).exists();
        if (!UnityInstallation::validateTarget(result.unityProjectPath, targetSdk, &safetyError)) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The SDK installation changed while staging. No files were replaced.");
            step.cause = safetyError;
            return;
        }
        QString moveError;
        if (hadTarget && !VNH::renameIntegrationDirectory(packages.filePath(targetName), packages.filePath(backupName), &moveError)) {
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not back up the existing Unity SDK.");
            step.cause = moveError;
            step.remediation = QStringLiteral("Check file write protection, directory permissions and processes holding files open at %1. If checkout or unlocking is required, do that first, then retry. The existing SDK was not changed.").arg(targetSdk);
            return;
        }
        if (hadTarget) updateBackupRoot = packages.filePath(backupName);
        if (!VNH::renameIntegrationDirectory(packages.filePath(stageName), packages.filePath(targetName), &moveError)) {
            QString restoreError;
            const bool restored = !hadTarget || VNH::renameIntegrationDirectory(packages.filePath(backupName), packages.filePath(targetName), &restoreError);
            QDir(stageRoot).removeRecursively();
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Could not activate the staged Unity SDK.");
            step.cause = restored ? QStringLiteral("The original installation state was restored.")
                                  : QStringLiteral("The previous SDK could not be restored. Its backup is listed below.");
            step.cause += QLatin1Char('\n') + moveError;
            if (!restoreError.isEmpty()) step.cause += QLatin1Char('\n') + restoreError;
            if (hadTarget && !restored) step.artifacts << updateBackupRoot;
            step.remediation = QStringLiteral("Check %1 directory locks and permissions, then retry.")
                                   .arg(packages.absolutePath());
            return;
        }
        installationActivated = true;

        result.sdkFilesCopied = sourceFiles.size();
        result.targetInstalled = true;
        step.summary = QStringLiteral("Installed SDK %1 from a verified staging directory; copied %2 managed file(s) and preserved %3 target-only file(s).")
                           .arg(sourceVersion).arg(result.sdkFilesCopied)
                           .arg(preservedFiles.size());
        step.artifacts << targetSdk;
    });

    runStep([&](StepResult& step) {
        if (result.requiresConfirmation) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Verification will run after the SDK update is approved.");
            return;
        }
        const bool managedChangesDetected = result.filesAdded > 0 ||
                                            result.filesChanged > 0 ||
                                            result.staleFilesToRemove > 0;
        if (request.checkOnly && !result.targetInstalled) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("No installed Unity SDK exists to verify.");
            return;
        }
        if (request.checkOnly && managedChangesDetected) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("Installed SDK verification is represented by the differences reported above.");
            return;
        }
        QStringList mismatches;
        QStringList unreadable;
        const auto rollback = [&]() {
            if (!installationActivated) return;
            QDir parent(QFileInfo(targetSdk).absolutePath());
            const QString targetName = QFileInfo(targetSdk).fileName();
            const QString failedName = QStringLiteral(".%1.failed-%2").arg(targetName,
                QUuid::createUuid().toString(QUuid::WithoutBraces));
            const bool displaced = !QFileInfo::exists(targetSdk)
                || VNH::renameIntegrationDirectory(parent.filePath(targetName), parent.filePath(failedName));
            if (QFileInfo::exists(parent.filePath(failedName))) step.artifacts << parent.filePath(failedName);
            const bool restored = displaced && (updateBackupRoot.isEmpty()
                || VNH::renameIntegrationDirectory(parent.filePath(QFileInfo(updateBackupRoot).fileName()), parent.filePath(targetName)));
            result.targetInstalled = QDir(targetSdk).exists();
            result.sdkFilesCopied = 0;
            if (restored) {
                step.remediation = QStringLiteral("The original installation state was restored. Review the unverified files listed below and retry with a complete SDK source.");
            } else {
                step.remediation = QStringLiteral("Automatic recovery failed. Close the editor, preserve the listed folders, and restore the previous SDK from the backup before retrying.");
                if (!updateBackupRoot.isEmpty()) step.artifacts << updateBackupRoot;
            }
        };
        for (const QString& relative : sourceFiles) {
            const FileDigest digest = sha256File(
                QDir(targetSdk).filePath(relative));
            if (digest.unreadable())
                unreadable << relative;
            else if (!digest.matches(sourceHashes.value(relative)))
                mismatches << relative;
        }
        if (!unreadable.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Installed SDK files could not be read, so their integrity is unverified.");
            step.cause = QStringLiteral("Files that exist but cannot be opened: %1")
                             .arg(unreadable.join(QStringLiteral(", ")));
            step.remediation = QStringLiteral("Close Unity and any process holding these files, then run Check only again.");
            step.details = unreadable;
            rollback();
            return;
        }
        if (!mismatches.isEmpty()) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("Installed SDK files do not match the verified source package.");
            step.cause = QStringLiteral("Missing or checksum-mismatched files: %1")
                             .arg(mismatches.join(QStringLiteral(", ")));
            step.remediation = QStringLiteral("Close Unity and rerun integration to repair the embedded package.");
            step.details = mismatches;
            rollback();
            return;
        }
        result.targetCurrent = true;
        step.summary = QStringLiteral("Verified the content integrity of %1 Harken-managed SDK file(s).")
                           .arg(sourceFiles.size());
        step.artifacts << targetSdk;
        if (!updateBackupRoot.isEmpty() && !QDir(updateBackupRoot).removeRecursively()) {
            step.status = StepStatus::Warning;
            step.details << QStringLiteral("The SDK passed verification, but its old backup could not be removed.");
            step.artifacts << updateBackupRoot;
        }
    });

    runStep([&](StepResult& step) {
        if (!unityValidated) {
            step.status = StepStatus::Skipped;
            step.summary = QStringLiteral("No report was written because the Unity project was not valid.");
            return;
        }
        const QString reportDir = QDir(result.unityProjectPath)
                                      .filePath(QStringLiteral("Library/HarkenIntegration"));
        result.jsonReportPath = QDir(reportDir).filePath(QStringLiteral("latest.json"));
        result.markdownReportPath = QDir(reportDir).filePath(QStringLiteral("latest.md"));
        step.status = StepStatus::Succeeded;
        step.summary = QStringLiteral("Wrote machine-readable and human-readable SDK integration reports.");
        step.artifacts << result.jsonReportPath << result.markdownReportPath;

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
        root[QStringLiteral("sdk_source_path")] = result.sdkSourcePath;
        root[QStringLiteral("sdk_install_path")] = result.sdkInstallPath;
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
        if (!VNH::writeIntegrationFile(result.jsonReportPath,
                QJsonDocument(root).toJson(QJsonDocument::Indented), &jsonFileError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("The integration report could not be written.");
            step.cause = jsonFileError;
            step.remediation = QStringLiteral("Check write permission for Unity Library/HarkenIntegration.");
            return;
        }

        QString markdown = QStringLiteral("# Harken Unity SDK integration report\n\n");
        markdown += QStringLiteral("- Generated: %1\n- SDK version: %2\n- SDK source: `%3`\n- Unity project: `%4`\n- Confirmation required: %5\n\n")
                        .arg(root[QStringLiteral("generated_at_utc")].toString(),
                             sourceVersion, result.sdkSourcePath,
                             result.unityProjectPath,
                             result.requiresConfirmation ? QStringLiteral("yes")
                                                         : QStringLiteral("no"));
        for (const StepResult& item : result.steps) {
            markdown += QStringLiteral("## %1 — %2\n\n%3\n\n")
                            .arg(item.title, statusName(item.status), item.summary);
            if (!item.cause.isEmpty())
                markdown += QStringLiteral("Cause: %1\n\n").arg(item.cause);
            if (!item.remediation.isEmpty())
                markdown += QStringLiteral("How to fix: %1\n\n").arg(item.remediation);
            if (!item.details.isEmpty())
                markdown += QStringLiteral("Details:\n\n```text\n%1\n```\n\n")
                                .arg(jsonArrayText(item.details));
        }
        QString markdownFileError;
        if (!VNH::writeIntegrationFile(result.markdownReportPath,
                markdown.toUtf8(), &markdownFileError)) {
            step.status = StepStatus::Failed;
            step.summary = QStringLiteral("JSON report was written, but the Markdown report failed.");
            step.cause = markdownFileError;
            step.remediation = QStringLiteral("Check write permission for Unity Library/HarkenIntegration.");
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
