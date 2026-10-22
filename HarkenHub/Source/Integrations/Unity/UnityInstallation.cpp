#include "Integrations/Unity/UnityInstallation.h"
#include "Integrations/IntegrationProgress.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringDecoder>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <filesystem>

namespace VNX::UnityInstallation {
namespace {

QString pathKey(const QString& path)
{
    QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    key = key.toCaseFolded();
#endif
    return key;
}

struct DiscoveryBudget
{
    QSet<QString> directories;
    QString error;
    QElapsedTimer externalTimer;
    qsizetype externalEntries = 0;
    qint64 externalBytes = 0;
    bool external = false;

    bool stop(const QString& path)
    {
        if (error.isEmpty())
            error = QStringLiteral("SDK discovery exceeded its safety limits at %1. Check local package references and reduce the scanned content.").arg(path);
        return false;
    }

    bool consume(const QString& path, int depth = 0, qint64 bytes = 0)
    {
        VNH::IntegrationProgress::checkpoint();
        if (external) {
            if (!externalTimer.isValid()) externalTimer.start();
            externalBytes += bytes;
            if (++externalEntries > 100000 || externalBytes > 32 * 1024 * 1024
                || externalTimer.elapsed() > 10000) {
                error = QStringLiteral("External package discovery exceeded its safety limits at %1. Point file: dependencies at individual package directories and remove unrelated content before retrying.").arg(path);
                return false;
            }
        }
        if (depth > 64 || bytes > 1024 * 1024)
            return stop(path);
        return error.isEmpty();
    }
};

bool readObject(const QString& path, QJsonObject* object, QString* error,
                DiscoveryBudget& budget)
{
    if (!budget.consume(path)) {
        *error = budget.error;
        return false;
    }
    if (!validatePath(path, error)) return false;
    if (!QFileInfo(path).isFile()) {
        *error = QStringLiteral("Unsupported filesystem entry: %1").arg(path);
        return false;
    }
    QFile file(path);
    QJsonParseError parseError;
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray contents = file.read(1024 * 1024 + 1);
    if (contents.size() > 1024 * 1024 || !budget.consume(path, 0, contents.size())) {
        budget.stop(path);
        *error = budget.error;
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Invalid JSON object: %1").arg(path);
        return false;
    }
    *object = document.object();
    return true;
}

bool sdkName(const QString& name)
{
    return name == QStringLiteral("com.harken.core");
}

bool ignoredDirectory(const QString& name)
{
    return name.startsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('~'));
}

bool walk(const QString& root, bool importedOnly,
          const std::function<void(const QFileInfo&)>& visit, QString* error,
          DiscoveryBudget* budget = nullptr, int depth = 0)
{
    if (budget) {
        if (!budget->consume(root, depth)) {
            *error = budget->error;
            return false;
        }
        const QString key = pathKey(root);
        if (budget->directories.contains(key)) return true;
        budget->directories.insert(key);
    }
    if (!validatePath(root, error)) return false;
    const QFileInfo rootInfo(root);
    if (!rootInfo.isDir() || !rootInfo.isReadable()) {
        *error = QStringLiteral("Cannot inspect directory: %1").arg(root);
        return false;
    }
    std::error_code enumerationError;
    std::filesystem::directory_iterator cursor(
        std::filesystem::path(root.toStdU16String()), enumerationError);
    const std::filesystem::directory_iterator end;
    while (!enumerationError && cursor != end) {
        const QFileInfo entry(QString::fromStdU16String(cursor->path().u16string()));
        VNH::IntegrationProgress::inspectedEntry();
        if (budget && !budget->consume(entry.absoluteFilePath(), depth)) {
            *error = budget->error;
            return false;
        }
        cursor.increment(enumerationError);
        if (importedOnly && entry.isDir() && ignoredDirectory(entry.fileName()))
            continue;
        if (entry.isSymLink() || entry.isJunction()) {
            *error = QStringLiteral("Linked paths cannot be inspected safely: %1")
                         .arg(entry.absoluteFilePath());
            return false;
        }
        if (entry.isDir()) {
            if (!walk(entry.absoluteFilePath(), importedOnly, visit, error, budget, depth + 1)) return false;
        } else if (entry.isFile()) {
            visit(entry);
        } else {
            *error = QStringLiteral("Unsupported filesystem entry: %1")
                         .arg(entry.absoluteFilePath());
            return false;
        }
    }
    if (enumerationError) {
        *error = QStringLiteral("Cannot enumerate directory %1: %2")
                     .arg(root, QString::fromStdString(enumerationError.message()));
        return false;
    }
    return true;
}

}

bool isDocumentationOrSample(const QString& relative)
{
    return relative.startsWith(QStringLiteral("Documentation~/"))
        || relative.startsWith(QStringLiteral("Samples~/"))
        || relative == QStringLiteral("Documentation~.meta")
        || relative == QStringLiteral("Samples~.meta")
        || relative == QStringLiteral("README.md")
        || relative == QStringLiteral("CHANGELOG.md");
}

bool matchesTextHashIgnoringLineEndings(const QString& path, const QByteArray& expected)
{
    static const QSet<QString> textExtensions = {
        QStringLiteral("cs"), QStringLiteral("asmdef"), QStringLiteral("asmref"),
        QStringLiteral("json"), QStringLiteral("meta"), QStringLiteral("md"),
        QStringLiteral("txt"), QStringLiteral("xml"), QStringLiteral("yaml"),
        QStringLiteral("yml"), QStringLiteral("prefab"), QStringLiteral("unity")};
    if (!textExtensions.contains(QFileInfo(path).suffix().toLower())) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash lf(QCryptographicHash::Sha256);
    QCryptographicHash crlf(QCryptographicHash::Sha256);
    QStringDecoder decoder(QStringDecoder::Utf8);
    bool pendingCr = false;
    while (!file.atEnd()) {
        VNH::IntegrationProgress::checkpoint();
        QByteArray block = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError || block.contains('\0')) return false;
        (void)static_cast<QString>(decoder(block));
        if (decoder.hasError()) return false;
        if (pendingCr) block.prepend('\r');
        pendingCr = block.endsWith('\r');
        if (pendingCr) block.chop(1);
        block.replace("\r\n", "\n");
        lf.addData(block);
        block.replace("\n", "\r\n");
        crlf.addData(block);
    }
    (void)static_cast<QString>(decoder(QByteArrayView("\0", 1)));
    if (decoder.hasError()) return false;
    if (pendingCr) {
        lf.addData(QByteArrayView("\r", 1));
        crlf.addData(QByteArrayView("\r", 1));
    }
    return lf.result().toHex() == expected.toLower() || crlf.result().toHex() == expected.toLower();
}

QString defaultRelativePath()
{
    return QStringLiteral("Packages/com.harken.core");
}

QString migrationJournalPath(const QString& projectRoot)
{
    return QDir(projectRoot).filePath(QStringLiteral("Library/HarkenIntegration/pending-migration.json"));
}

bool hasPendingMigration(const QString& projectRoot)
{
    const QFileInfo journal(migrationJournalPath(projectRoot));
    return journal.exists() || journal.isSymLink() || journal.isJunction();
}

bool samePath(const QString& left, const QString& right)
{
    return pathKey(left) == pathKey(right);
}

bool containsPath(const QString& parent, const QString& child)
{
    const QString prefix = pathKey(parent);
    const QString value = pathKey(child);
    return value == prefix || value.startsWith(prefix.endsWith(QLatin1Char('/'))
        ? prefix : prefix + QLatin1Char('/'));
}

bool safeRelativePath(const QString& input)
{
    QString path = input;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(QLatin1Char(':')))
        return false;
    for (const QString& part : path.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')))
            return false;
        for (const QChar character : part) {
            if (character.unicode() < 32 || QStringLiteral("<>\"|?*").contains(character))
                return false;
        }
        const QString base = part.section(QLatin1Char('.'), 0, 0).toUpper();
        if (base == QStringLiteral("CON") || base == QStringLiteral("PRN")
            || base == QStringLiteral("AUX") || base == QStringLiteral("NUL")
            || ((base.startsWith(QStringLiteral("COM")) || base.startsWith(QStringLiteral("LPT")))
                && base.size() == 4 && base.back() >= QLatin1Char('1')
                && base.back() <= QLatin1Char('9')))
            return false;
    }
    return true;
}

bool validatePath(const QString& path, QString* error)
{
    QFileInfo cursor(QFileInfo(path).absoluteFilePath());
    for (;;) {
        if (cursor.isSymLink() || cursor.isJunction()) {
            *error = QStringLiteral("Symbolic links and directory junctions are not supported: %1")
                         .arg(cursor.absoluteFilePath());
            return false;
        }
        const QString parent = cursor.absolutePath();
        if (parent == cursor.absoluteFilePath()) break;
        cursor.setFile(parent);
    }
    return true;
}

bool validateTree(const QString& path, QString* error)
{
    return walk(path, false, [](const QFileInfo&) {}, error);
}

bool resolveTarget(const QString& projectRoot, const QString& input,
                   QString* target, QString* error)
{
    QString relative = input.trimmed();
    if (relative.isEmpty()) relative = defaultRelativePath();
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (QDir::isAbsolutePath(relative))
        relative = QDir(projectRoot).relativeFilePath(relative);
    if (!safeRelativePath(relative)) {
        *error = QStringLiteral("Choose a safe directory inside the Unity project: %1").arg(input);
        return false;
    }
    const QStringList parts = relative.split(QLatin1Char('/'));
    bool supported = relative == defaultRelativePath();
    if (parts.front() == QStringLiteral("Assets") && parts.size() > 1) {
        supported = true;
        for (const QString& part : parts) {
            const QString lower = part.toLower();
            if (ignoredDirectory(part) || lower == QStringLiteral("editor")
                || lower == QStringLiteral("resources") || lower == QStringLiteral("streamingassets")
                || lower == QStringLiteral("editor default resources") || lower == QStringLiteral("gizmos"))
                supported = false;
        }
    }
    if (!supported) {
        *error = QStringLiteral("Install to Packages/com.harken.core or an Assets subdirectory outside Editor, Resources, StreamingAssets and other special Unity folders.");
        return false;
    }
    *target = QDir(projectRoot).absoluteFilePath(relative);
    if (!validatePath(*target, error)) return false;
    const QFileInfo info(*target);
    if (info.exists() && !info.isDir()) {
        *error = QStringLiteral("The installation location is not a directory: %1").arg(*target);
        return false;
    }
    return true;
}

Discovery discover(const QString& projectRoot)
{
    Discovery result;
    DiscoveryBudget budget;
    QStringList footprints;
    QSet<QString> embeddedPackages;
    QVector<QPair<QString, QString>> references;
    const auto add = [&](const QString& path) {
        for (const Installation& existing : result.installations) {
            if (!budget.consume(path)) return;
            if (samePath(existing.path, path)) return;
        }
        result.installations.push_back({QDir::cleanPath(path), false});
    };
    const auto dependencies = [&](const QJsonObject& object, const QString& base, bool lockFile = false) {
        const QJsonObject values = object.value(QStringLiteral("dependencies")).toObject();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!budget.consume(base)) return;
            const QString value = it.value().isString() ? it.value().toString()
                : it.value().toObject().value(QStringLiteral("version")).toString();
            const auto resolved = it.value().toObject();
            const QString local = value.startsWith(QStringLiteral("file:"))
                ? QUrl::fromPercentEncoding(value.mid(5).toUtf8()) : QString();
            if (lockFile && sdkName(it.key())
                && resolved.value(QStringLiteral("source")).toString() == QStringLiteral("embedded")
                && resolved.value(QStringLiteral("depth")).toInt(-1) == 0
                && safeRelativePath(local) && !local.contains(QLatin1Char('/'))
                && !local.contains(QLatin1Char('\\')) && !local.startsWith(QLatin1Char('.')))
                continue;
            if (sdkName(it.key()))
                result.sdkReferences << QStringLiteral("%1: %2").arg(it.key(), value);
            if (value.startsWith(QStringLiteral("file:"))) {
                const QString path = QDir::cleanPath(QDir(base).absoluteFilePath(local));
                references.push_back({it.key(), path});
                result.localPackagePaths << path;
            } else if (sdkName(it.key())) {
                references.push_back({it.key(), {}});
            }
        }
    };
    const auto visit = [&](const QFileInfo& info) {
        const QString name = info.fileName();
        const QString path = info.absoluteFilePath();
        if (name == QStringLiteral("package.json")) {
            QJsonObject package;
            QString error;
            if (!readObject(path, &package, &error, budget)) return;
            const QString identity = package.value(QStringLiteral("name")).toString();
            if (sdkName(identity)) {
                add(info.absolutePath());
                if (samePath(QFileInfo(info.absolutePath()).absolutePath(),
                             QDir(projectRoot).filePath(QStringLiteral("Packages"))))
                    embeddedPackages.insert(identity);
            }
            dependencies(package, info.absolutePath());
        } else if (name == QStringLiteral("harken-integration.json")
                   || name == QStringLiteral(".harken-integration-state.json")) {
            add(info.absolutePath());
        } else if (name.endsWith(QStringLiteral(".asmdef"))) {
            if (name == QStringLiteral("Harken.Core.asmdef") || name == QStringLiteral("Harken.Unity.asmdef"))
                footprints << path;
            QJsonObject assembly;
            QString error;
            if (!readObject(path, &assembly, &error, budget)) return;
            const QString identity = assembly.value(QStringLiteral("name")).toString();
            if (identity == QStringLiteral("Harken.Core") || identity == QStringLiteral("Harken.Unity")) {
                footprints << path;
                QDir parent(info.absolutePath());
                if (identity.endsWith(QStringLiteral(".Unity")) && parent.dirName() == QStringLiteral("Unity"))
                    parent.cdUp();
                if (parent.dirName() == QStringLiteral("Runtime")) {
                    parent.cdUp();
                    add(parent.absolutePath());
                }
            }
        } else {
            QString lower = name.toLower();
            lower.remove(QLatin1Char('_'));
            if (lower == QStringLiteral("harkencore.dll")
                || lower == QStringLiteral("harken.core.dll")
                || lower == QStringLiteral("libharkencore.so")
                || lower == QStringLiteral("libharkencore.dylib")
                || lower == QStringLiteral("libharkencore.a"))
                footprints << path;
        }
    };
    const auto scan = [&](const QString& path) {
        QString error;
        if (!walk(path, true, visit, &error, &budget)) result.errors << error;
    };
    scan(QDir(projectRoot).filePath(QStringLiteral("Assets")));
    scan(QDir(projectRoot).filePath(QStringLiteral("Packages")));
    for (const QString& name : {QStringLiteral("manifest.json"), QStringLiteral("packages-lock.json")}) {
        const QString path = QDir(projectRoot).filePath(QStringLiteral("Packages/") + name);
        if (!QFileInfo::exists(path)) continue;
        QJsonObject manifest;
        QString error;
        if (!readObject(path, &manifest, &error, budget)) result.errors << error;
        else {
            const bool lockFile = name == QStringLiteral("packages-lock.json");
            dependencies(manifest, QFileInfo(path).absolutePath(), lockFile);
            if (lockFile) {
                const auto resolved = manifest.value(QStringLiteral("dependencies")).toObject();
                for (auto it = resolved.begin(); it != resolved.end(); ++it) {
                    if (!budget.consume(path)) break;
                    const auto declared = it.value().toObject().value(QStringLiteral("dependencies")).toObject();
                    for (auto dependency = declared.begin(); dependency != declared.end(); ++dependency) {
                        if (!budget.consume(path)) break;
                        if (!sdkName(dependency.key())) continue;
                        result.sdkReferences << QStringLiteral("%1 -> %2: %3")
                            .arg(it.key(), dependency.key(), dependency.value().toString());
                        references.push_back({dependency.key(), {}});
                    }
                }
            }
        }
    }
    for (qsizetype i = 0; i < references.size(); ++i) {
        const auto reference = references[i];
        if (!budget.consume(reference.second)) break;
        if (embeddedPackages.contains(reference.first)) continue;
        if (reference.second.isEmpty()) {
            add(QDir(projectRoot).filePath(QStringLiteral("Packages/") + reference.first));
            result.errors << QStringLiteral("An SDK is referenced by Unity Package Manager: %1. Resolve this dependency before using Hub installation.").arg(reference.first);
        } else if (containsPath(reference.second, projectRoot)) {
            result.errors << QStringLiteral("A local package path includes the Unity project: %1. Reference a package directory instead.").arg(reference.second);
        } else {
            budget.external = !containsPath(projectRoot, reference.second);
            if (budget.external && !QFileInfo::exists(QDir(reference.second).filePath(QStringLiteral("package.json")))) {
                result.errors << QStringLiteral("External package path has no package.json: %1. Point the file: dependency at the package directory.").arg(reference.second);
            } else {
                scan(reference.second);
            }
            budget.external = false;
        }
    }
    for (const QString& relative : {defaultRelativePath(), QStringLiteral("Assets/HarkenSDK")}) {
        const QString root = QDir(projectRoot).filePath(relative);
        const QString path = QDir(root).filePath(QStringLiteral("package.json"));
        if (!QFileInfo::exists(path)
            || std::any_of(result.installations.cbegin(), result.installations.cend(),
                           [&](const Installation& item) { return samePath(item.path, root); }))
            continue;
        QJsonObject package;
        QString error;
        if (!readObject(path, &package, &error, budget)) result.errors << error;
        else if (package.value(QStringLiteral("name")).toString().trimmed().isEmpty())
            result.errors << QStringLiteral("Cannot determine the SDK package identity: %1").arg(path);
    }
    for (const QString& footprint : footprints) {
        if (!budget.consume(footprint)) break;
        if (std::none_of(result.installations.cbegin(), result.installations.cend(),
                         [&](const Installation& item) {
                             return !budget.consume(footprint) || containsPath(item.path, footprint);
                         }))
            result.errors << QStringLiteral("SDK files without an identifiable installation root: %1").arg(footprint);
    }
    for (Installation& item : result.installations) {
        if (!budget.consume(item.path)) break;
        const QString statePath = QDir(item.path).filePath(QStringLiteral(".harken-integration-state.json"));
        if (!QFileInfo::exists(statePath)) continue;
        QJsonObject state;
        QString error;
        if (!readObject(statePath, &state, &error, budget)) continue;
        item.managed = state.value(QStringLiteral("schema_version")).toInt() == 1
            && state.value(QStringLiteral("package_name")).toString() == QStringLiteral("com.harken.core")
            && !state.value(QStringLiteral("managed_files")).toArray().isEmpty();
        for (const QJsonValue& value : state.value(QStringLiteral("managed_files")).toArray())
            item.managed &= value.isString() && safeRelativePath(value.toString());
    }
    if (!budget.error.isEmpty()) result.errors << budget.error;
    result.errors.removeDuplicates();
    result.sdkReferences.removeDuplicates();
    result.localPackagePaths.removeDuplicates();
    return result;
}

bool validateTarget(const QString& projectRoot, const QString& target,
                    QString* error, bool removing)
{
    if (hasPendingMigration(projectRoot)) {
        *error = QStringLiteral("An unfinished SDK migration must be recovered before continuing.");
        return false;
    }
    const Discovery discovery = discover(projectRoot);
    if (!discovery.errors.isEmpty()) {
        *error = discovery.errors.join(QLatin1Char('\n'));
        return false;
    }
    if (removing && !discovery.sdkReferences.isEmpty()) {
        *error = QStringLiteral("Resolve SDK package dependencies before removal; Unity could load the referenced SDK after the embedded copy is removed:\n%1")
                     .arg(discovery.sdkReferences.join(QLatin1Char('\n')));
        return false;
    }
    if (discovery.installations.size() > 1) {
        QStringList paths;
        for (const Installation& item : discovery.installations) paths << item.path;
        *error = QStringLiteral("Multiple SDK installations were found. Resolve these conflicts before continuing:\n%1")
                     .arg(paths.join(QLatin1Char('\n')));
        return false;
    }
    if (!discovery.installations.isEmpty()) {
        const Installation& item = discovery.installations.front();
        if (!samePath(item.path, target)) {
            *error = QStringLiteral("An SDK is already installed at %1. Use that location or remove it before installing elsewhere.").arg(item.path);
            return false;
        }
    } else if (QDir(target).exists()
               && !QDir(target).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty()) {
        *error = QStringLiteral("The installation directory contains unrelated files: %1").arg(target);
        return false;
    }
    return !QDir(target).exists() || validateTree(target, error);
}

}
