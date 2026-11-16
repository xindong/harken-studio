
#include "AudioAssetLabels.h"

#include "Core/AssetPipeline/PathUtils.h"

#include <QCoreApplication>

namespace VNX {

QString assetDisplayName(const AssetInfo& asset)
{
    const QString normalized = PathUtils::normalizeToUnix(asset.relativePath);
    const int slash = normalized.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? normalized : normalized.mid(slash + 1);
}

QString assetDisplayPath(const AssetInfo& asset)
{
    return PathUtils::normalizeToUnix(QString::fromLatin1(PathUtils::AUDIO_SOURCES_DIR)
        + QLatin1Char('/') + asset.relativePath);
}

QStringList assetFolderSegments(const AssetInfo& asset)
{
    const QString normalized = PathUtils::normalizeToUnix(asset.relativePath);
    const int slash = normalized.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return {};
    const QString folder = normalized.left(slash);
    return folder.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

QString assetTooltip(const AssetInfo& asset)
{
    QString tooltip = QCoreApplication::translate("VNX::ProjectExplorer", "Source: %1")
                          .arg(assetDisplayPath(asset));
    tooltip += QStringLiteral("\nFormat: ") + asset.metadata.originalFormat;
    tooltip += QStringLiteral("\nDuration: ")
        + QString::number(asset.metadata.durationSec, 'f', 2) + QStringLiteral("s");
    tooltip += QStringLiteral("\nSample Rate: ")
        + QString::number(asset.metadata.sampleRate) + QStringLiteral(" Hz");
    tooltip += QStringLiteral("\nChannels: ") + QString::number(asset.metadata.channels);
    if (asset.isOrphaned)
        tooltip += QStringLiteral("\nStatus: Orphaned");
    return tooltip;
}

}
