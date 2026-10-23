#include "PackArtifactCache.h"

#include "AssetPipeline/PathUtils.h"
#include "ProjectTypes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QDebug>

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
    cachePath_ = QDir(PathUtils::getDotCacheDir(QFileInfo(project.projectPath).absolutePath()))
        .filePath(QStringLiteral("pack-artifacts.json"));
    QFile file(cachePath_);
    if (file.open(QIODevice::ReadOnly)) {
        const auto root = QJsonDocument::fromJson(file.readAll()).object();
        if (root.value(QStringLiteral("version")).toInt() == 1) {
            const auto platforms = project.platformIds();
            for (const auto& value : root.value(QStringLiteral("artifacts")).toArray()) {
                const auto row = value.toObject();
                PackArtifactMetadata artifact;
                artifact.unitKey = row.value(QStringLiteral("unitKey")).toString();
                artifact.platform = row.value(QStringLiteral("platform")).toString();
                artifact.language = row.value(QStringLiteral("language")).toString();
                artifact.packName = row.value(QStringLiteral("packName")).toString();
                artifact.outputPath = row.value(QStringLiteral("outputPath")).toString();
                artifact.bytes = row.value(QStringLiteral("bytes")).toInteger(-1);
                artifact.version = row.value(QStringLiteral("formatVersion")).toInt();
                const auto modified = row.value(QStringLiteral("modified"));
                if (!platforms.contains(artifact.platform) || artifact.unitKey.isEmpty()
                    || artifact.packName.isEmpty() || !QDir::isAbsolutePath(artifact.outputPath)
                    || artifact.bytes < 0 || artifact.version == 0 || !modified.isDouble())
                    continue;
                artifact.modified = QDateTime::fromMSecsSinceEpoch(modified.toInteger());
                artifacts_.append(std::move(artifact));
            }
        }
    }
    emit changed();
}

void PackArtifactCache::closeProject()
{
    cachePath_.clear();
    artifacts_.clear();
    emit changed();
}

void PackArtifactCache::publish(const QString& platform,
                               const QVector<PackArtifactMetadata>& artifacts,
                               bool updateSelectedOnly)
{
    if (cachePath_.isEmpty()) return;
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
    save();
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
    save();
    emit changed();
}

void PackArtifactCache::save() const
{
    if (cachePath_.isEmpty()) return;
    QJsonArray rows;
    for (const auto& artifact : artifacts_) {
        rows.append(QJsonObject{
            {QStringLiteral("unitKey"), artifact.unitKey},
            {QStringLiteral("platform"), artifact.platform},
            {QStringLiteral("language"), artifact.language},
            {QStringLiteral("packName"), artifact.packName},
            {QStringLiteral("outputPath"), artifact.outputPath},
            {QStringLiteral("bytes"), artifact.bytes},
            {QStringLiteral("modified"), artifact.modified.toMSecsSinceEpoch()},
            {QStringLiteral("formatVersion"), static_cast<qint64>(artifact.version)}});
    }
    const QByteArray content = QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1}, {QStringLiteral("artifacts"), rows}}).toJson(QJsonDocument::Compact);
    QSaveFile file(cachePath_);
    if (!QDir().mkpath(QFileInfo(cachePath_).absolutePath())
        || !file.open(QIODevice::WriteOnly) || file.write(content) != content.size()
        || !file.commit())
        qWarning() << "Could not save pack artifact cache:" << cachePath_ << file.errorString();
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
