#include "PackArtifactCache.h"

#include "AssetPipeline/PathUtils.h"
#include "ProjectTypes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace VNX {

PackArtifactCache& PackArtifactCache::instance()
{
    static PackArtifactCache cache;
    return cache;
}

void PackArtifactCache::openProject(const ProjectData& project)
{
    artifacts_.clear();
    projectOpen_ = true;
    const QDir cache(PathUtils::getDotCacheDir(QFileInfo(project.projectPath).absolutePath()));
    QFile::remove(cache.filePath(QStringLiteral("pack-artifacts.json")));
    QFile file(cache.filePath(QStringLiteral("pack/builds.json")));
    if (file.open(QIODevice::ReadOnly)) {
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        if (root.value(QStringLiteral("version")).toInt() == 2) {
            const auto platforms = project.platformIds();
            const auto targets = root.value(QStringLiteral("targets")).toObject();
            for (auto target = targets.begin(); target != targets.end(); ++target) {
                const auto value = target.value().toObject();
                const auto platform = value.value(QStringLiteral("platform")).toString();
                if (!platforms.contains(platform) || !QDir::isAbsolutePath(target.key())) continue;
                const auto packs = value.value(QStringLiteral("packs")).toObject();
                for (auto pack = packs.begin(); pack != packs.end(); ++pack) {
                    const auto row = pack.value().toObject();
                    const auto key = row.value(QStringLiteral("key")).toString();
                    const QFileInfo relative(pack.key());
                    const QFileInfo output(QDir(target.key()).filePath(pack.key()));
                    if (key.isEmpty() || row.value(QStringLiteral("state")).toString().isEmpty()
                        || !output.isFile()) continue;
                    PackArtifactMetadata artifact;
                    artifact.unitKey = key;
                    artifact.platform = platform;
                    artifact.language = relative.path() == QStringLiteral(".") ? QString() : relative.path();
                    artifact.packName = relative.completeBaseName();
                    artifact.outputPath = output.absoluteFilePath();
                    artifact.bytes = output.size();
                    artifact.modified = output.lastModified();
                    artifact.version = HARKEN_PACK_SCHEMA_VERSION;
                    artifacts_.append(std::move(artifact));
                }
            }
        }
    }
    emit changed();
}

void PackArtifactCache::closeProject()
{
    projectOpen_ = false;
    artifacts_.clear();
    emit changed();
}

void PackArtifactCache::publish(const QString& platform,
                               const QVector<PackArtifactMetadata>& artifacts,
                               bool updateSelectedOnly)
{
    if (!projectOpen_) return;
    artifacts_.removeIf([&](const auto& artifact) {
        if (artifact.platform != platform) return false;
        if (!updateSelectedOnly) return true;
        return std::any_of(artifacts.begin(), artifacts.end(), [&](const auto& updated) {
            return updated.platform == platform && updated.unitKey == artifact.unitKey
                && updated.language == artifact.language;
        });
    });
    for (const auto& artifact : artifacts)
        if (artifact.platform == platform) artifacts_.append(artifact);
    emit changed();
}

void PackArtifactCache::removeUnselectedPlatforms(const QString& outputRoot,
                                                 const QStringList& platforms)
{
    const QDir directory(outputRoot);
    const auto removed = artifacts_.removeIf([&](const auto& artifact) {
        const auto relative = directory.relativeFilePath(artifact.outputPath);
        return !platforms.contains(artifact.platform) && !QDir::isAbsolutePath(relative)
            && !relative.startsWith(QStringLiteral("../"));
    });
    if (removed == 0) return;
    emit changed();
}

const PackArtifactMetadata* PackArtifactCache::find(
    const QString& platform, const QString& language,
    const QString& unitKey, const QString& outputPath) const
{
    const auto expected = QDir::cleanPath(outputPath);
    for (const auto& artifact : artifacts_)
        if (artifact.platform == platform && artifact.language == language
            && artifact.unitKey == unitKey && QDir::cleanPath(artifact.outputPath) == expected)
            return &artifact;
    return nullptr;
}

}
