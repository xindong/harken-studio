#include "ProjectDataPolicy.h"

#include "AssetPipeline/PathUtils.h"
#include "ProjectLocator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace VNX {

namespace {

#if defined(Q_OS_WIN)
constexpr Qt::CaseSensitivity kPathCaseSensitivity = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCaseSensitivity = Qt::CaseSensitive;
#endif

}

ProjectDataPolicy::ProjectDataPolicy(const QString& packBuildOutputDir,
                                   const QString& buildWaveformOutputDir)
    : packOutputPaths_{PathUtils::buildPacksDirName(), QStringLiteral("GeneratedPacks")}
{
    for (const auto& value : {packBuildOutputDir, buildWaveformOutputDir}) {
        const QString path = PathUtils::normalizePackBuildOutputDir(value);
        if (isSafeRelativePath(path)) packOutputPaths_.append(path);
    }
}

ProjectDataPolicy ProjectDataPolicy::forProjectFile(const QString& projectFilePath)
{
    const ProjectLocation location = ProjectLocator::locate(projectFilePath);
    QFile file(location.projectFilePath);
    if (!location.ok() || !file.open(QIODevice::ReadOnly))
        return ProjectDataPolicy();
    const QJsonObject content = QJsonDocument::fromJson(file.readAll()).object()
                                    .value(QStringLiteral("content")).toObject();
    const QString path = PathUtils::normalizePackBuildOutputDir(
        content.value(QStringLiteral("packBuildOutputDir")).toString());
    if (!PathUtils::isValidPackBuildOutputDir(path, location.projectFilePath))
        return ProjectDataPolicy();
    const QString waveformPath = content.value(QStringLiteral("buildWaveformOutputDir")).toString();
    const QDir projectDirectory(QFileInfo(location.projectFilePath).absolutePath());
    return ProjectDataPolicy(projectDirectory.relativeFilePath(
        PathUtils::packBuildOutputRoot(location.projectFilePath, path)),
        PathUtils::isValidWaveformBuildOutputDir(waveformPath, location.projectFilePath)
            ? projectDirectory.relativeFilePath(PathUtils::waveformBuildOutputRoot(location.projectFilePath, waveformPath))
            : QString());
}

QString ProjectDataPolicy::normalizeRelativePath(const QString& path)
{
    QString normalized = QDir::fromNativeSeparators(QDir::cleanPath(path));
    while (normalized.startsWith(QStringLiteral("./")))
        normalized.remove(0, 2);
    return normalized;
}

bool ProjectDataPolicy::isSafeRelativePath(const QString& relativePath)
{
    const QString path = normalizeRelativePath(relativePath);
    return !path.isEmpty() && path != QStringLiteral(".")
        && !QDir::isAbsolutePath(path) && path != QStringLiteral("..")
        && !path.startsWith(QStringLiteral("../"));
}

ProjectDataClass ProjectDataPolicy::classify(const QString& relativePath) const
{
    const QString path = normalizeRelativePath(relativePath);
    if (!isSafeRelativePath(path))
        return ProjectDataClass::Unknown;
    if (path.startsWith(QStringLiteral(".harken-project-"))
        || path.startsWith(QStringLiteral(".harken-upgrade-")))
        return ProjectDataClass::TransactionControl;
    if (path == QStringLiteral(".trash") || path.startsWith(QStringLiteral(".trash/"))
        || path == QStringLiteral(".cache/editor_layout.json"))
        return ProjectDataClass::Preserved;
    if (path == QStringLiteral(".cache") || path.startsWith(QStringLiteral(".cache/")))
        return ProjectDataClass::Derived;
    for (const QString& outputPath : packOutputPaths_) {
        if (path.compare(outputPath, kPathCaseSensitivity) == 0
            || path.startsWith(outputPath + QLatin1Char('/'), kPathCaseSensitivity))
            return ProjectDataClass::Derived;
    }
    if (path.endsWith(QStringLiteral(".hsp"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".vp"), Qt::CaseInsensitive)
        || path == QStringLiteral("AudioSources")
        || path.startsWith(QStringLiteral("AudioSources/")))
        return ProjectDataClass::Raw;
    return ProjectDataClass::Unknown;
}

bool ProjectDataPolicy::includeInContentFingerprint(const QString& relativePath,
                                                   bool isDirectory) const
{
    if (isDirectory) {
        const QString prefix = normalizeRelativePath(relativePath) + QLatin1Char('/');
        for (const QString& outputPath : packOutputPaths_) {
            if (outputPath.startsWith(prefix, kPathCaseSensitivity))
                return false;
        }
    }
    return protectByDefault(relativePath);
}

bool ProjectDataPolicy::protectByDefault(const QString& relativePath) const
{
    const QString path = normalizeRelativePath(relativePath);
    const ProjectDataClass dataClass = classify(path);
    return (dataClass == ProjectDataClass::Raw || dataClass == ProjectDataClass::Unknown)
        && path != QStringLiteral("AudioSources")
        && !path.startsWith(QStringLiteral("AudioSources/"));
}

}
