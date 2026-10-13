
#pragma once

#include <QString>
#include <QDir>
#include <QFileInfo>

namespace VNX {

class PathUtils
{
public:

    static QString toRelativePath(const QString& absolutePath, const QString& projectRoot);

    static QString toAbsolutePath(const QString& relativePath, const QString& projectRoot);

    static QString normalizeToUnix(const QString& path);

    static QString normalizeToNative(const QString& path);


    static QString getAudioSourcesDir(const QString& projectRoot);

    static QString getCacheDir(const QString& projectRoot);

    static QString getPlatformCacheDir(const QString& projectRoot, const QString& platformName);


    static QString getDotCacheDir(const QString& projectRoot);

    static QString getProjectIndexCacheDir(const QString& projectRoot);

    static QString getAssetRegistryCacheDir(const QString& projectRoot);

    static QString getMediaCacheDir(const QString& projectRoot);

    static QString getWaveformCacheDir(const QString& projectRoot);

    static QString getSearchIndexCacheDir(const QString& projectRoot);

    static QString getTrashDir(const QString& projectRoot);

    static bool ensureDirectory(const QString& path);

    // Quarantine instead of delete: moves the file into
    // .trash/<timestamp>/<relativePath> so reconcile/cleanup passes never
    // destroy audio irrecoverably. Returns false when the source is absent or
    // the move failed (the file is then left in place, never removed).
    static bool moveToTrash(const QString& projectRoot,
                            const QString& absolutePath,
                            const QString& relativePath);

    static QString calculateImportDestination(const QString& externalPath,
                                               const QString& projectRoot);


    static bool isWithinProject(const QString& path, const QString& projectRoot);

    static bool hasUnsafeCharacters(const QString& path);

    static QString getExtension(const QString& path);

    static bool isSupportedAudioFormat(const QString& path);


    static constexpr const char* AUDIO_SOURCES_DIR = "AudioSources";

    static constexpr const char* DOT_CACHE_DIR = ".cache";
    static constexpr const char* TRASH_DIR = ".trash";
    static constexpr const char* PROJECT_INDEX_SUBDIR = "project_index";
    static constexpr const char* ASSET_REGISTRY_SUBDIR = "asset_registry";
    static constexpr const char* MEDIA_CACHE_SUBDIR = "media";
    static constexpr const char* WAVEFORM_CACHE_SUBDIR = "waveforms";
    static constexpr const char* SEARCH_INDEX_SUBDIR = "search_index";

    static QString buildPacksDirName();
    static QString normalizePackBuildOutputDir(const QString& path);
    static bool isValidPackBuildOutputDir(const QString& path, const QString& projectFilePath = {},
                                          bool allowObjectBuckets = false);
    static bool isProjectRootDirectory(const QString& path, const QString& projectFilePath);
    static bool isProjectObjectDirectory(const QString& path, const QString& projectFilePath);
    static QString packBuildOutputLocationError();
    static QString packBuildOutputRoot(const QString& projectFilePath,
                                       const QString& relativeDir);
};

}
