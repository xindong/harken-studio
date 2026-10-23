
#include "PathUtils.h"
#include "AudioFormatCatalog.h"
#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QRegularExpression>

namespace VNX {

QString PathUtils::buildPacksDirName()
{
    return QStringLiteral("BuildPacks");
}

QString PathUtils::normalizePackBuildOutputDir(const QString& path, const QString& projectFilePath)
{
    QString normalized = path.trimmed();
    normalized.replace('\\', '/');
    if (normalized.isEmpty()) return {};
    normalized = QDir::cleanPath(normalized);
#if defined(Q_OS_WIN)
    if (normalized.size() >= 2 && normalized.at(1) == QLatin1Char(':')
        && (normalized.size() < 3 || normalized.at(2) != QLatin1Char('/')
            || normalized.at(0).toLower() < QLatin1Char('a')
            || normalized.at(0).toLower() > QLatin1Char('z')))
        return normalized;
#endif
    if (!projectFilePath.isEmpty() && QDir::isAbsolutePath(normalized))
        normalized = QDir(QFileInfo(projectFilePath).absolutePath()).relativeFilePath(normalized);
    return normalized;
}

bool PathUtils::isValidPackBuildOutputDir(const QString& path, const QString& projectFilePath,
                                         bool allowObjectBuckets)
{
    const QString normalized = normalizePackBuildOutputDir(path);
    QString directory = normalized;
#if defined(Q_OS_WIN)
    if (directory.size() >= 3 && directory.at(0).toLower() >= QLatin1Char('a')
        && directory.at(0).toLower() <= QLatin1Char('z')
        && directory.at(1) == QLatin1Char(':') && directory.at(2) == QLatin1Char('/'))
        directory.remove(0, 2);
#endif
    return (normalized.isEmpty()
        || (normalized != QStringLiteral(".") && !hasUnsafeCharacters(directory)))
        && !isProjectRootDirectory(packBuildOutputRoot(projectFilePath, normalized), projectFilePath)
        && (allowObjectBuckets
            || !isProjectObjectDirectory(packBuildOutputRoot(projectFilePath, normalized), projectFilePath));
}

bool PathUtils::isProjectObjectDirectory(const QString& path, const QString& projectFilePath)
{
    if (projectFilePath.isEmpty()) return false;
#if defined(Q_OS_WIN)
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    const auto resolved = [](const QString& path) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        QFileInfo ancestor(absolute);
        while (!ancestor.exists()) {
            const QString parent = ancestor.absolutePath();
            if (parent == ancestor.absoluteFilePath()) return absolute;
            ancestor.setFile(parent);
        }
        const QString canonical = ancestor.canonicalFilePath();
        return canonical.isEmpty() ? absolute : QDir::cleanPath(QDir(canonical).filePath(
            QDir(ancestor.absoluteFilePath()).relativeFilePath(absolute)));
    };
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString canonical = resolved(absolute);
    const QDir project(QFileInfo(projectFilePath).absolutePath());
    for (const auto* name : {"Actions", "Layers", "Busses", "Variables", "Attenuations"}) {
        const QString bucket = project.filePath(QLatin1String(name));
        const QString canonicalBucket = resolved(bucket);
        if (absolute.compare(bucket, sensitivity) == 0
            || absolute.startsWith(bucket + QLatin1Char('/'), sensitivity)
            || canonical.compare(canonicalBucket, sensitivity) == 0
            || canonical.startsWith(canonicalBucket + QLatin1Char('/'), sensitivity))
            return true;
    }
    return false;
}

QString PathUtils::packBuildOutputLocationError()
{
    return QObject::tr("Pack output cannot use the project root or object bucket directories "
        "(Actions, Layers, Busses, Variables, Attenuations). "
        "Choose a directory outside these buckets, such as BuildPacks.");
}

bool PathUtils::isProjectRootDirectory(const QString& path, const QString& projectFilePath)
{
    if (projectFilePath.isEmpty()) return false;
    const QFileInfo output(path);
    const QFileInfo project(QFileInfo(projectFilePath).absolutePath());
#if defined(Q_OS_WIN)
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    if (output.absoluteFilePath().compare(project.absoluteFilePath(), sensitivity) == 0) return true;
    const QString canonical = output.canonicalFilePath();
    return !canonical.isEmpty() && canonical.compare(project.canonicalFilePath(), sensitivity) == 0;
}

QString PathUtils::packBuildOutputRoot(const QString& projectFilePath,
                                      const QString& directory)
{
    const QString normalized = normalizePackBuildOutputDir(directory);
    return toAbsolutePath(normalized.isEmpty() ? buildPacksDirName() : normalized,
                          QFileInfo(projectFilePath).absolutePath());
}

QString PathUtils::waveformBuildOutputRoot(const QString& projectFilePath,
                                          const QString& directory)
{
    const QString normalized = normalizePackBuildOutputDir(directory);
    return toAbsolutePath(normalized.isEmpty()
        ? buildPacksDirName() + QStringLiteral("/Waveforms") : normalized,
        QFileInfo(projectFilePath).absolutePath());
}

bool PathUtils::isValidWaveformBuildOutputDir(const QString& path, const QString& projectFilePath)
{
    const QString normalized = normalizePackBuildOutputDir(path);
    return isValidPackBuildOutputDir(normalized.isEmpty()
        ? buildPacksDirName() + QStringLiteral("/Waveforms") : normalized, projectFilePath);
}

QString PathUtils::toRelativePath(const QString& absolutePath, const QString& projectRoot)
{
    QDir projectDir(projectRoot);
    QString relative = projectDir.relativeFilePath(absolutePath);

    return relative.replace('\\', '/');
}

QString PathUtils::toAbsolutePath(const QString& relativePath, const QString& projectRoot)
{
    QString normalized = relativePath;
    normalized.replace('\\', '/');

    QDir projectDir(projectRoot);
    return QDir::cleanPath(projectDir.absoluteFilePath(normalized));
}

QString PathUtils::normalizeToUnix(const QString& path)
{
    QString normalized = path;
    normalized.replace('\\', '/');

    while (normalized.contains(QStringLiteral("//"))) {
        normalized.replace(QStringLiteral("//"), QStringLiteral("/"));
    }

    if (normalized.length() > 1 && normalized.endsWith('/')) {
        normalized.chop(1);
    }

    return normalized;
}

QString PathUtils::normalizeToNative(const QString& path)
{
    return QDir::toNativeSeparators(path);
}


QString PathUtils::getAudioSourcesDir(const QString& projectRoot)
{
    return QDir(projectRoot).absoluteFilePath(AUDIO_SOURCES_DIR);
}

QString PathUtils::getCacheDir(const QString& projectRoot)
{
    return getMediaCacheDir(projectRoot);
}

QString PathUtils::getMediaCacheDir(const QString& projectRoot)
{
    return QDir(getDotCacheDir(projectRoot)).absoluteFilePath(MEDIA_CACHE_SUBDIR);
}

QString PathUtils::getWaveformCacheDir(const QString& projectRoot)
{
    return QDir(getDotCacheDir(projectRoot)).absoluteFilePath(WAVEFORM_CACHE_SUBDIR);
}

QString PathUtils::getSearchIndexCacheDir(const QString& projectRoot)
{
    return QDir(getDotCacheDir(projectRoot)).absoluteFilePath(SEARCH_INDEX_SUBDIR);
}

QString PathUtils::getPlatformCacheDir(const QString& projectRoot, const QString& platformName)
{
    return QDir(getCacheDir(projectRoot)).absoluteFilePath(platformName);
}


QString PathUtils::getDotCacheDir(const QString& projectRoot)
{
    return QDir(projectRoot).absoluteFilePath(DOT_CACHE_DIR);
}

QString PathUtils::getTrashDir(const QString& projectRoot)
{
    return QDir(projectRoot).absoluteFilePath(TRASH_DIR);
}

bool PathUtils::ensureDirectory(const QString& path)
{
    return QDir(path).exists() || QDir().mkpath(path);
}

bool PathUtils::moveToTrash(const QString& projectRoot,
                            const QString& absolutePath,
                            const QString& relativePath)
{
    if (!QFile::exists(absolutePath))
        return false;

    const QString sessionDir = QDir(getTrashDir(projectRoot)).absoluteFilePath(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QString destPath = toAbsolutePath(relativePath, sessionDir);
    if (!QDir().mkpath(QFileInfo(destPath).absolutePath()))
        return false;

    const QFileInfo destInfo(destPath);
    const QString base = destInfo.absolutePath() + '/' + destInfo.completeBaseName();
    const QString suffix = destInfo.suffix().isEmpty()
        ? QString() : '.' + destInfo.suffix();
    for (int i = 1; QFile::exists(destPath); ++i)
        destPath = base + QStringLiteral("_%1").arg(i) + suffix;

    return QFile::rename(absolutePath, destPath);
}

QString PathUtils::getProjectIndexCacheDir(const QString& projectRoot)
{
    return QDir(getDotCacheDir(projectRoot)).absoluteFilePath(PROJECT_INDEX_SUBDIR);
}

QString PathUtils::getAssetRegistryCacheDir(const QString& projectRoot)
{
    return QDir(getDotCacheDir(projectRoot)).absoluteFilePath(ASSET_REGISTRY_SUBDIR);
}

QString PathUtils::calculateImportDestination(const QString& externalPath,
                                               const QString& projectRoot)
{
    QString audioSourcesDir = getAudioSourcesDir(projectRoot);
    return QDir(audioSourcesDir).absoluteFilePath(QFileInfo(externalPath).fileName());
}


bool PathUtils::isWithinProject(const QString& path, const QString& projectRoot)
{
    QString absPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString absRoot = QDir::cleanPath(QFileInfo(projectRoot).absoluteFilePath());

    if (!absRoot.endsWith('/') && !absRoot.endsWith('\\')) {
        absRoot += '/';
    }

    return absPath.startsWith(absRoot, Qt::CaseInsensitive);
}

bool PathUtils::hasUnsafeCharacters(const QString& path)
{
    static QRegularExpression unsafeChars(QStringLiteral("[<>:\"|?*]"));
    return unsafeChars.match(path).hasMatch();
}

QString PathUtils::getExtension(const QString& path)
{
    QFileInfo info(path);
    return info.suffix().toLower();
}

bool PathUtils::isSupportedAudioFormat(const QString& path)
{
    return AudioFormatCatalog::containsExtension(getExtension(path));
}

}
