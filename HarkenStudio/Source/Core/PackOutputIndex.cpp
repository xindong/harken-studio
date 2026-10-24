#include "ProjectPackExporter.h"
#include "CanonicalPackSetExport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>

#include <filesystem>

namespace VNX {
namespace {

bool validOutput(const QString& baseDir, const ProjectPlatform& platform, QString* error,
                 const QVector<PackArtifactMetadata>& artifacts)
{
    const QDir dir(ProjectPackExporter::platformOutputDir(baseDir, platform));
    if (QFileInfo(dir.path()).isSymLink()
        || QFile::exists(dir.filePath(QStringLiteral(".harken-pack-publishing")))) return false;
    QJsonArray packs;
    QStringList managedFiles;
    const QString platformId = ProjectPlatformSemantics::idString(platform.id);
    QStringList expectedPaths;
    for (const auto& artifact : artifacts)
        if (artifact.platform == platformId)
            expectedPaths.append(dir.relativeFilePath(artifact.outputPath));
    if (!artifacts.isEmpty() && expectedPaths.isEmpty()) {
        if (error) *error = QStringLiteral("No generated Pack artifacts for platform: %1").arg(platform.name);
        return false;
    }
    return ReadPackPublication(dir.path(), {}, ProjectPlatformSemantics::idString(platform.id),
        &packs, &managedFiles, error, true, expectedPaths);
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
    if (!RecoverPackOutput(baseDir, error)) return {};
    return lock;
}

bool ProjectPackExporter::ValidatePackOutputs(const QString& baseDir,
                                              const QVector<ProjectPlatform>& platforms, QString* error,
                                              const QVector<PackArtifactMetadata>& artifacts)
{
    if (baseDir.isEmpty() || !ProjectPlatformSemantics::validatePlatforms(platforms, error)) return false;
    for (const auto& platform : platforms) {
        if (!validOutput(baseDir, platform, error, artifacts) || !preserveDirectorySpelling(baseDir, platform)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("Pack output is incomplete or belongs to another platform: %1").arg(platform.name);
            return false;
        }
    }
    return true;
}

bool ProjectPackExporter::FinalizePackOutputs(const QString& baseDir,
                                              const QVector<ProjectPlatform>& platforms,
                                              const QStringList& requestedIds,
                                              const QStringList& succeededIds, QString* error,
                                              const QVector<PackArtifactMetadata>& artifacts)
{
    if (baseDir.isEmpty() || !ProjectPlatformSemantics::validatePlatforms(platforms, error)) return false;
    QVector<ProjectPlatform> completed;
    for (const auto& id : succeededIds) {
        const auto* platform = ProjectPlatformSemantics::find(platforms, QUuid(id));
        if (!platform || !requestedIds.contains(id)) {
            if (error) *error = QStringLiteral("Pack platform changed during the build: %1").arg(id);
            return false;
        }
        completed.append(*platform);
    }
    if (completed.isEmpty()) return true;
    return ValidatePackOutputs(baseDir, completed, error, artifacts);
}

}
