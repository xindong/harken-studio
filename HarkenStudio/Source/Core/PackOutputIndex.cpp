#include "ProjectPackExporter.h"

#include "PackSchemaGenerated.h"
#include "ProjectSchemaValidator.h"

#include <harken_studio_build/PackImage.h>
#include <harken_studio_build/PackFormatV2.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QThread>

#include <filesystem>

namespace VNX {
namespace {

QJsonObject readJson(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool readIndexJson(const QString& path, QJsonObject* index)
{
    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDelayMs = 50;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.readAll();
            if (file.error() == QFileDevice::NoError) {
                *index = QJsonDocument::fromJson(bytes).object();
                return true;
            }
        }
        if (attempt != kMaxAttempts) QThread::msleep(kRetryDelayMs);
    }
    return false;
}

QJsonObject indexEntry(const ProjectPlatform& platform)
{
    return {{QStringLiteral("id"), ProjectPlatformSemantics::idString(platform.id)},
            {QStringLiteral("name"), platform.name},
            {QStringLiteral("basePlatform"), ProjectPlatformSemantics::basePlatformName(platform.basePlatform)},
            {QStringLiteral("path"), platform.name}};
}

bool validOutput(const QString& baseDir, const ProjectPlatform& platform)
{
    const QDir dir(ProjectPackExporter::platformOutputDir(baseDir, platform));
    if (QFileInfo(dir.path()).isSymLink()
        || QFile::exists(dir.filePath(QStringLiteral(".harken-pack-publishing")))) return false;
    const auto manifest = readJson(dir.filePath(HarkenEditor::Bridge::packManifestFileName()));
    if (!ProjectSchemaValidator::validatePackManifest(PackSchema::kPackManifestSchemaVersion,
            manifest, QStringLiteral("pack manifest")).isEmpty()
        || manifest.value(QStringLiteral("platformId")).toString() != ProjectPlatformSemantics::idString(platform.id)
        || manifest.value(QStringLiteral("platformName")).toString() != platform.name
        || manifest.value(QStringLiteral("basePlatform")).toString() != ProjectPlatformSemantics::basePlatformName(platform.basePlatform)) return false;
    const auto rows = manifest.value(QStringLiteral("packs")).toArray();
    QSet<QString> ids;
    QSet<QString> paths;
    bool hasInit = false;
    for (const auto& value : rows) {
        const auto row = value.toObject();
        const QString id = row.value(QStringLiteral("packStableId")).toString();
        const QString path = row.value(QStringLiteral("path")).toString();
        if (row.value(QStringLiteral("platformId")) != manifest.value(QStringLiteral("platformId"))
            || ids.contains(id) || paths.contains(path)
            || path.contains(QChar('\\')) || path.contains(QChar(':')) || QDir::isAbsolutePath(path)) return false;
        for (const auto& part : path.split(QChar('/')))
            if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) return false;
        const QFileInfo info(dir.filePath(path));
        if (!info.isFile() || info.isSymLink()
            || !info.canonicalFilePath().startsWith(dir.canonicalPath() + QChar('/'))) return false;
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly)) return false;
        const auto bytes = file.readAll();
        bool hashOk = false;
        const auto hash = row.value(QStringLiteral("contentHash")).toString().toULongLong(&hashOk);
        if (!hashOk || hash != harkenstudiobuild::Fnv1a64(bytes.constData(), static_cast<std::size_t>(bytes.size()))) return false;
        harkenstudiobuild::PackCanonicalImage image;
        bool idOk = false;
        const auto stableId = id.toULongLong(&idOk);
        if (!idOk || stableId == 0
            || harkenstudiobuild::ParsePackCanonicalImage(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                static_cast<std::uint64_t>(bytes.size()), &image) != harkenstudiobuild::kPackOk
            || image.packStableId != stableId) return false;
        if (id == manifest.value(QStringLiteral("initPackStableId")).toString()) {
            if (image.packKind != harkenstudiobuild::kPackKindInit) return false;
            hasInit = true;
        }
        ids.insert(id);
        paths.insert(path);
    }
    for (const auto& value : rows)
        for (const auto& dependency : value.toObject().value(QStringLiteral("dependencies")).toArray())
            if (!ids.contains(dependency.toString())) return false;
    return hasInit;
}

bool writeIndex(const QString& baseDir, const QJsonArray& entries, QString* error)
{
    const QJsonObject index{{QStringLiteral("schema_version"), PackSchema::kPackManifestSchemaVersion},
                            {QStringLiteral("platforms"), entries}};
    const auto errors = ProjectSchemaValidator::validatePackManifest(
        PackSchema::kPackManifestSchemaVersion, index, QStringLiteral("pack index"));
    if (!errors.isEmpty()) {
        if (error) *error = errors.join(QChar('\n'));
        return false;
    }
    const QString path = QDir(baseDir).filePath(HarkenEditor::Bridge::packIndexFileName());
    const auto bytes = QJsonDocument(index).toJson(QJsonDocument::Indented);
    constexpr int kMaxAttempts = 3;
    constexpr int kRetryDelayMs = 50;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
            return true;
        if (attempt == kMaxAttempts) break;
        QThread::msleep(kRetryDelayMs);
    }
    if (error) *error = QStringLiteral("Cannot publish Pack index: %1").arg(path);
    return false;
}

bool preserveDirectorySpelling(const QString& baseDir, const ProjectPlatform& platform)
{
    QDir root(baseDir);
    const std::filesystem::path requested(root.filePath(platform.name).toStdU16String());
    for (const auto& entry : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (entry.isSymLink() || entry.fileName() == platform.name) continue;
        if (entry.fileName().normalized(QString::NormalizationForm_C).toCaseFolded() != platform.name.normalized(QString::NormalizationForm_C).toCaseFolded()) continue;
        std::error_code error;
        if (!std::filesystem::equivalent(std::filesystem::path(entry.absoluteFilePath().toStdU16String()), requested, error)) continue;
        const auto temporary = QStringLiteral(".harken-pack-rename-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!root.rename(entry.fileName(), temporary)) return false;
        if (root.rename(temporary, platform.name)) return true;
        root.rename(temporary, entry.fileName());
        return false;
    }
    return true;
}

}

std::shared_ptr<QLockFile> ProjectPackExporter::LockPackOutputRoot(const QString& baseDir, QString* error)
{
    if (baseDir.isEmpty() || !QDir().mkpath(baseDir)) {
        if (error) *error = QStringLiteral("Cannot create Pack output root: %1").arg(baseDir);
        return {};
    }
    auto lock = std::make_shared<QLockFile>(QDir(baseDir).filePath(QStringLiteral(".harken-pack-build.lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock()) {
        if (error) *error = QStringLiteral("Pack output is being built by another request: %1").arg(baseDir);
        return {};
    }
    return lock;
}

bool ProjectPackExporter::WritePackIndex(const QString& baseDir,
                                        const QVector<ProjectPlatform>& platforms, QString* error)
{
    if (baseDir.isEmpty() || !ProjectPlatformSemantics::validatePlatforms(platforms, error)) return false;
    QJsonArray entries;
    for (const auto& platform : platforms) {
        if (!validOutput(baseDir, platform) || !preserveDirectorySpelling(baseDir, platform)) {
            if (error) *error = QStringLiteral("Pack output is incomplete or belongs to another platform: %1").arg(platform.name);
            return false;
        }
        entries.append(indexEntry(platform));
    }
    return writeIndex(baseDir, entries, error);
}

bool ProjectPackExporter::PublishPackIndex(const QString& baseDir,
                                          const QVector<ProjectPlatform>& platforms,
                                          const QStringList& requestedIds,
                                          const QStringList& succeededIds, QString* error)
{
    if (baseDir.isEmpty() || !ProjectPlatformSemantics::validatePlatforms(platforms, error)) return false;
    if (!QDir().mkpath(baseDir)) {
        if (error) *error = QStringLiteral("Cannot create Pack output root: %1").arg(baseDir);
        return false;
    }
    QLockFile lock(QDir(baseDir).filePath(QStringLiteral(".harken-pack-index.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(2000)) {
        if (error) *error = QStringLiteral("Pack index is being published by another build");
        return false;
    }
    const QDir output(baseDir);
    const QString currentPath = output.filePath(HarkenEditor::Bridge::packIndexFileName());
    const QString legacyPath = output.filePath(QStringLiteral("HarkenPackIndex.json"));
    const bool currentExists = QFileInfo::exists(currentPath);
    const QString previousPath = currentExists ? currentPath : legacyPath;
    QJsonArray oldEntries;
    if (QFileInfo::exists(previousPath)) {
        QJsonObject previous;
        const bool readable = readIndexJson(previousPath, &previous);
        if (!readable) {
            if (error) *error = QStringLiteral("Cannot read Pack index: %1").arg(previousPath);
            return false;
        }
        if (readable) {
            const auto errors = ProjectSchemaValidator::validatePackManifest(
                PackSchema::kPackManifestSchemaVersion, previous, QStringLiteral("pack index"));
            if (errors.isEmpty()) oldEntries = previous.value(QStringLiteral("platforms")).toArray();
            else if (currentExists) {
                if (error) *error = errors.join(QChar('\n'));
                return false;
            }
        }
    }
    QJsonArray entries;
    bool complete = true;
    for (const auto& id : succeededIds) {
        if (!ProjectPlatformSemantics::find(platforms, QUuid(id))) {
            complete = false;
            if (error) *error = QStringLiteral("Pack platform was deleted during the build: %1").arg(id);
        }
    }
    for (const auto& platform : platforms) {
        const auto id = ProjectPlatformSemantics::idString(platform.id);
        const bool succeeded = succeededIds.contains(id);
        const auto entry = indexEntry(platform);
        if (!succeeded && (requestedIds.contains(id) || !oldEntries.contains(entry))) continue;
        if (validOutput(baseDir, platform) && (!succeeded || preserveDirectorySpelling(baseDir, platform))) {
            entries.append(entry);
        } else if (succeeded) {
            complete = false;
            if (error) *error = QStringLiteral("Pack output is incomplete or its platform changed during the build: %1").arg(platform.name);
        }
    }
    return writeIndex(baseDir, entries, error) && complete;
}

}
