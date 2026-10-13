#include "AudioFormatCatalog.h"

namespace VNX {

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

}
