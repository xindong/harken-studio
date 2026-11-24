#include "AudioFormatCatalog.h"

#include "PathUtils.h"

namespace harken::studio {

const QStringList& AudioFormatCatalog::extensions()
{
    static const QStringList value{
        QStringLiteral("wav"),
        QStringLiteral("mp3"),
        QStringLiteral("flac"),
        QStringLiteral("ogg"),
        QStringLiteral("aiff"),
        QStringLiteral("aif")
    };
    return value;
}

QStringList AudioFormatCatalog::filePatterns()
{
    QStringList result;
    for (const QString& extension : extensions())
        result << (QStringLiteral("*.") + extension);
    return result;
}

bool AudioFormatCatalog::containsExtension(const QString& extension)
{
    return extensions().contains(extension.toLower());
}

QString AudioFormatCatalog::displayName(const QString& filePath)
{
    const QFileInfo file(PathUtils::normalizeToUnix(filePath));
    const QString stem = file.completeBaseName();
    return !stem.isEmpty() && containsExtension(file.suffix()) ? stem : file.fileName();
}

}
