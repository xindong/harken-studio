#include "RuntimeContentBundleBuilder.h"
#include "RuntimeContentBundleBuilderInternal.h"

#include "Tooling/Harkenizer/Core/CurrentWorldArtifactWriter.h"
#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"
#include "Tooling/Harkenizer/Core/CanonicalPackSetExport.h"
#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"
#include "ProjectTypes.h"
#include "Components/VAction.h"
#include "RuntimeBridge/PackagingBridge.h"
#include <harken_studio_build/PackFormat.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDirIterator>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace VNX {
namespace {

QString fileSha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
        hash.addData(file.read(1024 * 1024));
    return QString::fromLatin1(hash.result().toHex());
}

bool copyFile(const QString& source, const QString& target)
{
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::remove(target);
    return QFile::copy(source, target);
}

bool verifyPublications(const RuntimeContentBundleResult& bundle, const QString& root, QString* error)
{
    for (const auto& publication : bundle.requiredPublications) {
        const QString path = QDir(root).filePath(publication.path);
        const QString identity = fileSha256(path);
        if (identity.isEmpty() || identity != publication.identitySha256) {
            if (error) *error = QStringLiteral("Publication does not match the captured bundle: %1").arg(path);
            return false;
        }
    }
    return true;
}

QString uint64String(std::uint64_t value)
{
    return QStringLiteral("0x%1").arg(value, 16, 16, QLatin1Char('0'));
}

std::vector<RuntimeContentAction> actions(const ProjectData& project,
                                        const std::set<harkenstudio::ObjectId>& included)
{
    std::vector<RuntimeContentAction> result;
    std::function<void(const ProjectItem*)> visit = [&](const ProjectItem* item) {
        if (item == nullptr)
            return;
        if (item->type == ProjectItem::Type::Action
            && !harkenstudio::IsNull(item->runtimeId()) && included.count(item->runtimeId()) != 0
            && item->comp<VAction>() != nullptr) {
            result.push_back({item->name, item->runtimeId()});
        }
        for (const auto& child : item->children)
            visit(child.get());
    };
    if (project.root)
        visit(project.root.get());
    for (const ProjectFileBinding* binding : project.allFileBindings()) {
        if (binding != nullptr && binding->rootItem)
            visit(binding->rootItem.get());
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.stableId < right.stableId;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left,
                                                             const auto& right) {
                     return left.stableId == right.stableId;
                 }),
                 result.end());
    return result;
}

QByteArray manifestBytes(const RuntimeContentBundleResult& result)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("bundleId"), result.bundleId);
    root.insert(QStringLiteral("platformId"), result.platform);
    root.insert(QStringLiteral("platformName"), result.platformName);
    root.insert(QStringLiteral("basePlatform"), ProjectPlatformSemantics::basePlatformName(result.basePlatform));
    // Preview envelope only. Games load orderedPacks; currentWorld may be
    // omitted, deleted, or rebuilt without changing the pack contract.
    if (!result.currentWorldPath.isEmpty()) {
        root.insert(QStringLiteral("currentWorld"), result.currentWorldPath);
    }
    root.insert(QStringLiteral("worldSemanticFingerprint"),
                uint64String(result.worldSemanticFingerprint));
    QJsonArray packs;
    for (const auto& pack : result.orderedPacks) {
        QJsonObject row;
        row.insert(QStringLiteral("path"), pack.path);
        row.insert(QStringLiteral("platformId"), result.platform);
        row.insert(QStringLiteral("size"), pack.size);
        row.insert(QStringLiteral("sha256"), pack.sha256);
        packs.append(row);
    }
    root.insert(QStringLiteral("orderedPacks"), packs);
    QJsonArray actionRows;
    for (const auto& action : result.actions) {
        QJsonObject row;
        row.insert(QStringLiteral("name"), action.name);
        row.insert(QStringLiteral("stableId"), objectIdString(action.stableId));
        actionRows.append(row);
    }
    root.insert(QStringLiteral("actions"), actionRows);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

}

bool activateRuntimeContentBundleDirectory(
    const QString& source, const QString& target, QString* error)
{
#ifdef Q_OS_WIN
    const QString nativeSource = QDir::toNativeSeparators(source);
    const QString nativeTarget = QDir::toNativeSeparators(target);
    constexpr int kMaxAttempts = 8;
    constexpr int kRetryDelayMs = 50;
    DWORD nativeError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (MoveFileW(reinterpret_cast<LPCWSTR>(nativeSource.utf16()),
                      reinterpret_cast<LPCWSTR>(nativeTarget.utf16())) != FALSE)
            return true;
        nativeError = GetLastError();
        if ((nativeError != ERROR_ACCESS_DENIED && nativeError != ERROR_SHARING_VIOLATION)
            || attempt + 1 == kMaxAttempts)
            break;
        QThread::msleep(kRetryDelayMs);
    }
#else
    if (QDir().rename(source, target))
        return true;
#endif
    if (error != nullptr) {
#ifdef Q_OS_WIN
        *error = QStringLiteral(
                     "Runtime content staged bundle cannot be activated "
                     "(Windows error %1; sourceDir=%2, targetExists=%3, targetParentDir=%4; "
                     "source=%5; target=%6)")
                     .arg(nativeError)
                     .arg(QFileInfo(source).isDir() ? 1 : 0)
                     .arg(QFileInfo::exists(target) ? 1 : 0)
                     .arg(QFileInfo(QFileInfo(target).absolutePath()).isDir() ? 1 : 0)
                     .arg(source)
                     .arg(target);
#else
        *error = QStringLiteral("Runtime content staged bundle cannot be activated");
#endif
    }
    return false;
}

RuntimeContentBundleResult RuntimeContentBundleBuilder::build(
    ProjectData& inputProject,
    const QString& publishRoot,
    const QString& platform)
{
    RuntimeContentBundleResult result;
    result.platform = platform;
    const auto platformId = ProjectPlatformSemantics::parseId(platform);
    if (!platformId || !ProjectPlatformSemantics::find(inputProject.platforms, *platformId)) {
        result.error = QStringLiteral("Unknown project platform ID: '%1'").arg(platform);
        return result;
    }
    const auto snapshot = ProjectBuildSnapshot::capture(inputProject, &result.error);
    if (!snapshot) return result;
    const auto& project = snapshot->project;
    const auto& definition = *ProjectPlatformSemantics::find(project.platforms, *platformId);
    result.platformName = definition.name;
    result.basePlatform = definition.basePlatform;
    QDir root(publishRoot);
    if (!root.exists() && !QDir().mkpath(root.absolutePath())) {
        result.error = QStringLiteral("Runtime content root cannot be created");
        return result;
    }

    QTemporaryDir transaction(root.filePath(QStringLiteral(".bundle_XXXXXX")));
    if (!transaction.isValid()) {
        result.error = QStringLiteral("Runtime content staging directory cannot be created");
        return result;
    }
    const QString buildPath = QDir(transaction.path()).filePath(QStringLiteral("build"));
    QDir().mkpath(buildPath);
    const QDir buildDir(buildPath);

    ProjectPackExporter exporter;
    ProjectPackExporter::ExportOptions options;
    options.platform = result.platform;
    options.snapshot = snapshot;
    options.waveformOutputDir = buildDir.filePath(QStringLiteral("Waveforms"));
    const QString worldPath = QDir(buildPath).filePath(QStringLiteral("CurrentWorld.vnworld"));
    options.currentWorldOutputPath = worldPath;
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    const auto exported = exporter.ExportAllPacks(project, buildPath, options, &records);
    if (!exported.success) {
        result.error = exported.error;
        return result;
    }
    std::vector<QString> packPaths;
    for (const auto& record : records) {
        if (!record.success || !QFileInfo::exists(record.outputPath))
            continue;
        packPaths.push_back(QFileInfo(record.outputPath).absoluteFilePath());
    }
    std::sort(packPaths.begin(), packPaths.end());
    packPaths.erase(std::unique(packPaths.begin(), packPaths.end()), packPaths.end());
    if (packPaths.empty()) {
        result.error = QStringLiteral("Runtime content build produced no packs");
        return result;
    }
    QJsonArray publicationRows;
    QStringList publicationFiles;
    if (!ReadPackPublication(buildPath, project.projectId, result.platform,
                             &publicationRows, &publicationFiles, &result.error))
        return result;
    std::vector<QString> publicationPaths;
    for (const auto& row : publicationRows) {
        const auto publication = row.toObject();
        const QFileInfo file(publication.value(QStringLiteral("path")).toString());
        publicationPaths.push_back(buildDir.absoluteFilePath(file.filePath()));
        publicationPaths.push_back(buildDir.absoluteFilePath(
            QDir(file.path()).filePath(file.completeBaseName() + QStringLiteral(".json"))));
        const QDir companionDir(buildDir.filePath(file.path()));
        for (const auto& waveform : publication.value(QStringLiteral("companion")).toObject()
                 .value(QStringLiteral("waveforms")).toArray()) {
            const QString path = QDir::cleanPath(companionDir.absoluteFilePath(
                waveform.toObject().value(QStringLiteral("file")).toString()));
            const QString relative = buildDir.relativeFilePath(path);
            if (QDir::isAbsolutePath(relative) || relative.startsWith(QStringLiteral("../"))) {
                result.error = QStringLiteral("Waveform output escaped the content bundle: %1").arg(path);
                return result;
            }
            publicationPaths.push_back(path);
        }
    }
    std::sort(publicationPaths.begin(), publicationPaths.end());
    publicationPaths.erase(std::unique(publicationPaths.begin(), publicationPaths.end()), publicationPaths.end());

    const std::uint64_t fingerprint = exported.currentWorldSemanticFingerprint;
    if (fingerprint == 0) {
        result.error = QStringLiteral("Current World semantic fingerprint is unavailable");
        return result;
    }

    QCryptographicHash identity(QCryptographicHash::Sha256);
    identity.addData(result.platform.toUtf8());
    identity.addData(QByteArray::number(static_cast<qulonglong>(fingerprint)));
    identity.addData(fileSha256(worldPath).toLatin1());
    for (const QString& pack : publicationPaths) {
        const QString digest = fileSha256(pack);
        if (digest.isEmpty()) {
            result.error = QStringLiteral("Publication cannot be hashed: %1").arg(pack);
            return result;
        }
        identity.addData(buildDir.relativeFilePath(pack).toUtf8());
        identity.addData(digest.toLatin1());
    }
    result.bundleId = QString::fromLatin1(identity.result().toHex());
    const QString relativeBundle = QStringLiteral("Bundles/%1").arg(result.bundleId);
    const QString stagedBundle = QDir(transaction.path()).filePath(relativeBundle);
    QDir().mkpath(stagedBundle);

    const QString worldName = QStringLiteral("CurrentWorld.vnworld");
    if (!copyFile(worldPath, QDir(stagedBundle).filePath(worldName))) {
        result.error = QStringLiteral("Current World cannot be staged");
        return result;
    }
    result.currentWorldPath = relativeBundle + QLatin1Char('/') + worldName;
    for (const QString& pack : publicationPaths) {
        const QString relativePath = buildDir.relativeFilePath(pack);
        const QString target = QDir(stagedBundle).filePath(relativePath);
        if (!copyFile(pack, target)) {
            result.error = QStringLiteral("Pack cannot be staged: %1").arg(relativePath);
            return result;
        }
        const QString digest = fileSha256(target);
        if (digest.isEmpty()) {
            result.error = QStringLiteral("Staged Pack cannot be verified: %1").arg(relativePath);
            return result;
        }
        if (std::binary_search(packPaths.begin(), packPaths.end(), pack))
            result.orderedPacks.push_back(
            {relativeBundle + QLatin1Char('/') + relativePath,
             QFileInfo(target).size(), digest});
    }
    std::set<harkenstudio::ObjectId> builtActions;
    for (const auto& row : publicationRows) {
        const auto companion = row.toObject().value(QStringLiteral("companion")).toObject();
        if (!companion.contains(QStringLiteral("guid"))) continue;
        harkenstudio::ObjectId actionStableId;
        if (!parseObjectIdJson(companion.value(QStringLiteral("guid")), &actionStableId)
            || harkenstudio::IsNull(actionStableId)) {
            result.error = QStringLiteral("Pack companion contains an invalid Action identity");
            return result;
        }
        builtActions.insert(actionStableId);
    }
    result.actions = actions(project, builtActions);
    result.worldSemanticFingerprint = fingerprint;

    const QString activeBundle = root.filePath(relativeBundle);
    if (!QFileInfo::exists(activeBundle)) {
        if (!QDir().mkpath(QFileInfo(activeBundle).absolutePath())) {
            result.error = QStringLiteral("Runtime content bundle cannot be published");
            return result;
        }
        if (!activateRuntimeContentBundleDirectory(stagedBundle, activeBundle, &result.error))
            return result;
    }
    for (const QString& source : publicationPaths) {
        const QString target = QDir(activeBundle).filePath(buildDir.relativeFilePath(source));
        const QString digest = fileSha256(source);
        if (digest.isEmpty() || digest != fileSha256(target)) {
            result.error = QStringLiteral("Existing language publication does not match the built platform: %1").arg(target);
            return result;
        }
        result.requiredPublications.push_back({buildDir.relativeFilePath(source), digest});
    }
    for (const auto& pack : result.orderedPacks) {
        const QString path = root.filePath(pack.path);
        if (QFileInfo(path).size() != pack.size || fileSha256(path) != pack.sha256) {
            result.error = QStringLiteral("Existing content bundle does not match the built platform: %1").arg(path);
            return result;
        }
    }
    result.bundleDirectory = activeBundle;

    const QString manifestPath = root.filePath(QStringLiteral("HarkenContent.json"));
    QSaveFile manifest(manifestPath);
    manifest.setDirectWriteFallback(false);
    if (!manifest.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Runtime content manifest cannot be opened");
        return result;
    }
    const QByteArray bytes = manifestBytes(result);
    if (manifest.write(bytes) != bytes.size() || !manifest.commit()) {
        result.error = QStringLiteral("Runtime content manifest cannot be committed");
        return result;
    }
    result.manifestPath = manifestPath;
    result.warnings = exported.warnings;
    result.success = true;
    return result;
}

bool RuntimeContentBundleBuilder::deploy(
    const RuntimeContentBundleResult& bundle,
    const QString& targetRoot,
    QString* error)
{
    if (!bundle.success || bundle.bundleId.isEmpty()
        || bundle.requiredPublications.empty()
        || !QFileInfo::exists(bundle.manifestPath)
        || !QFileInfo(bundle.bundleDirectory).isDir()) {
        if (error != nullptr)
            *error = QStringLiteral("Runtime content bundle is not publishable");
        return false;
    }
    if (!verifyPublications(bundle, bundle.bundleDirectory, error)) return false;
    QDir target(targetRoot);
    if (!target.exists() && !QDir().mkpath(target.absolutePath())) {
        if (error != nullptr)
            *error = QStringLiteral("Runtime content target cannot be created");
        return false;
    }
    const QString bundlesRoot = target.filePath(QStringLiteral("Bundles"));
    if (!QDir().mkpath(bundlesRoot)) {
        if (error != nullptr)
            *error = QStringLiteral("Runtime content bundle root cannot be created");
        return false;
    }
    const QString finalBundle = QDir(bundlesRoot).filePath(bundle.bundleId);
    if (!QFileInfo::exists(finalBundle)) {
        QTemporaryDir staged(target.filePath(QStringLiteral(".deploy_XXXXXX")));
        if (!staged.isValid()) {
            if (error != nullptr)
                *error = QStringLiteral("Runtime content deploy staging cannot be created");
            return false;
        }
        const QString stagedBundle = QDir(staged.path()).filePath(bundle.bundleId);
        if (!QDir().mkpath(stagedBundle)) {
            if (error != nullptr)
                *error = QStringLiteral("Runtime content staged bundle cannot be created");
            return false;
        }
        {
            QDirIterator files(bundle.bundleDirectory, QDir::Files,
                               QDirIterator::Subdirectories);
            while (files.hasNext()) {
                const QString source = files.next();
                const QString relative = QDir(bundle.bundleDirectory).relativeFilePath(source);
                if (relative.startsWith(QStringLiteral("../"))) {
                    if (error != nullptr)
                        *error = QStringLiteral("Runtime content source escaped its bundle");
                    return false;
                }
                const QString targetPath = QDir(stagedBundle).filePath(relative);
                if (!copyFile(source, targetPath)
                    || fileSha256(source) != fileSha256(targetPath)) {
                    if (error != nullptr)
                        *error = QStringLiteral("Runtime content file cannot be verified: %1")
                                     .arg(relative);
                    return false;
                }
            }
        }
        if (!verifyPublications(bundle, stagedBundle, error)) return false;
        if (!activateRuntimeContentBundleDirectory(stagedBundle, finalBundle, error)) return false;
    }

    for (const auto& pack : bundle.orderedPacks) {
        const QString path = target.filePath(pack.path);
        if (QFileInfo(path).size() != pack.size || fileSha256(path) != pack.sha256) {
            if (error) *error = QStringLiteral("Deployed content bundle does not match the captured platform: %1").arg(path);
            return false;
        }
    }
    if (!verifyPublications(bundle, bundle.bundleDirectory, error)
        || !verifyPublications(bundle, finalBundle, error)) return false;
    const QByteArray bytes = manifestBytes(bundle);
    QSaveFile active(target.filePath(QStringLiteral("HarkenContent.json")));
    active.setDirectWriteFallback(false);
    if (!active.open(QIODevice::WriteOnly)
        || active.write(bytes) != bytes.size()
        || !active.commit()) {
        if (error != nullptr)
            *error = QStringLiteral("Runtime content active manifest cannot be committed");
        return false;
    }
    return true;
}

}
