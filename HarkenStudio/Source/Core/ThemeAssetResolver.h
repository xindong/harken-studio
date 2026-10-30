#pragma once

#include "ThemePackageDto.h"
#include "ThemePackageParser.h"
#include "ThemeSnapshot.h"

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QMap>
#include <QRawFont>
#include <QSize>

#include <memory>
#include <optional>

namespace VNX {

struct ThemeAssetResolverLimits
{
    qsizetype maximumEncodedBytes = 1024 * 1024;
    int maximumDimension = 4096;
    qint64 maximumPixels = 16 * 1024 * 1024;
};

struct ThemeAssetVariant
{
    QSize logicalSize;
    qreal devicePixelRatio = 1.0;
    int frameIndex = 0;
    QColor tint;
};

struct ThemeImageResource
{
    ThemeSnapshotPtr snapshot;
    QString logicalId;
    ThemeAssetVariant variant;
    QImage image;
    QByteArray svgData;
    std::optional<ThemeAssetInsetsDto> insets;
    bool builtinFallback = false;
};

using ThemeImageResourcePtr = std::shared_ptr<const ThemeImageResource>;

struct ThemeImageResolution
{
    ThemeImageResourcePtr resource;
    QList<ThemePackageDiagnostic> diagnostics;

    bool ok() const { return resource != nullptr; }
};

struct ThemeIconResolution
{
    QIcon icon;
    ThemeImageResourcePtr imageResource;
    QList<ThemePackageDiagnostic> diagnostics;

    bool ok() const { return !icon.isNull() && imageResource != nullptr; }
};

struct ThemeFontResource
{
    ThemeSnapshotPtr snapshot;
    QString logicalId;
    QByteArray data;
    QRawFont font;
};

using ThemeFontResourcePtr = std::shared_ptr<const ThemeFontResource>;

struct ThemeFontResolution
{
    ThemeFontResourcePtr resource;
    QList<ThemePackageDiagnostic> diagnostics;

    bool ok() const { return resource != nullptr; }
};

class ThemeAssetResolver
{
public:
    explicit ThemeAssetResolver(ThemeAssetResolverLimits limits = {});
    ~ThemeAssetResolver();

    ThemeAssetResolver(const ThemeAssetResolver&) = delete;
    ThemeAssetResolver& operator=(const ThemeAssetResolver&) = delete;

    void registerSnapshot(const ThemeSnapshotPtr& snapshot,
                          ThemeAssetMapDto assets,
                          QMap<QString, QString> packageRoots,
                          const ThemeSnapshotPtr& sharedAssets = {});

    quint64 assetGeneration(const ThemeSnapshotPtr& snapshot) const;

    ThemeImageResolution resolveImage(
        const ThemeSnapshotPtr& snapshot,
        const QString& logicalId,
        const ThemeAssetVariant& variant = {});

    ThemeIconResolution resolveIcon(
        const ThemeSnapshotPtr& snapshot,
        const QString& logicalId,
        const ThemeAssetVariant& variant = {});

    ThemeIconResolution resolveToggleIcon(
        const ThemeSnapshotPtr& snapshot,
        const QString& onLogicalId,
        const QString& offLogicalId,
        const ThemeAssetVariant& variant = {});

    ThemeFontResolution resolveFont(const ThemeSnapshotPtr& snapshot,
                                    const QString& logicalId,
                                    qreal pixelSize = 12.0);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
