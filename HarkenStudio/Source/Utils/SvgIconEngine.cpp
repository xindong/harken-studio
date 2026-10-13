#include "SvgIconEngine.h"

#include "Core/ThemeAssetResolver.h"

#include <QApplication>
#include <QPainter>
#include <QPaintDevice>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>

#include <mutex>

namespace VNX {
namespace {

void ensureCacheLimit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        QPixmapCache::setCacheLimit(32 * 1024);
    });
}

}

SvgIconEngine::SvgIconEngine(ThemeAssetResolver* resolver,
                             ThemeSnapshotPtr snapshot,
                             QString onLogicalId,
                             QString offLogicalId,
                             QColor overrideColor)
    : resolver_(resolver)
    , snapshot_(std::move(snapshot))
    , onLogicalId_(std::move(onLogicalId))
    , offLogicalId_(std::move(offLogicalId))
    , overrideColor_(std::move(overrideColor))
{
    ensureCacheLimit();
}

QIconEngine* SvgIconEngine::clone() const
{
    return new SvgIconEngine(resolver_, snapshot_, onLogicalId_, offLogicalId_,
                             overrideColor_);
}

QColor SvgIconEngine::resolveColor(QIcon::Mode mode) const
{
    const QPalette palette = qApp->palette();
    if (overrideColor_.isValid() && mode != QIcon::Disabled)
        return overrideColor_;
    switch (mode) {
    case QIcon::Disabled:
        return palette.color(QPalette::Disabled, QPalette::ButtonText);
    case QIcon::Active:
    case QIcon::Selected:
        return palette.color(QPalette::Active, QPalette::Highlight);
    case QIcon::Normal:
    default:
        return palette.color(QPalette::Active, QPalette::ButtonText);
    }
}

QPixmap SvgIconEngine::pixmap(const QSize& size, QIcon::Mode mode,
                              QIcon::State state)
{
    return scaledPixmap(size, mode, state, qApp->devicePixelRatio());
}

QPixmap SvgIconEngine::scaledPixmap(const QSize& size, QIcon::Mode mode,
                                    QIcon::State state, qreal scale)
{
    if (!resolver_ || !snapshot_ || size.isEmpty())
        return {};

    const QString logicalId = state == QIcon::On || offLogicalId_.isEmpty()
        ? onLogicalId_
        : offLogicalId_;
    const QColor color = resolveColor(mode);
    const QString cacheKey = QStringLiteral("theme-icon:%1:%2:%3x%4:%5:%6:%7:%8")
        .arg(snapshot_->generation)
        .arg(logicalId)
        .arg(size.width())
        .arg(size.height())
        .arg(QString::number(scale, 'f', 3))
        .arg(color.rgba(), 0, 16)
        .arg(static_cast<int>(mode))
        .arg(static_cast<int>(state));

    QPixmap cached;
    if (QPixmapCache::find(cacheKey, &cached))
        return cached;

    const auto resolution = resolver_->resolveImage(
        snapshot_, logicalId, ThemeAssetVariant{size, scale, 0, color});
    if (!resolution.resource)
        return {};

    QPixmap pixmap = QPixmap::fromImage(resolution.resource->image);
    QPixmapCache::insert(cacheKey, pixmap);
    return pixmap;
}

void SvgIconEngine::paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
                          QIcon::State state)
{
    qreal scale = 1.0;
    if (painter && painter->device())
        scale = painter->device()->devicePixelRatioF();
    const QPixmap image = scaledPixmap(rect.size(), mode, state, scale);
    if (!image.isNull())
        painter->drawPixmap(rect, image);
}

QSize SvgIconEngine::actualSize(const QSize& size, QIcon::Mode, QIcon::State)
{
    return size;
}

QString SvgIconEngine::key() const
{
    return QStringLiteral("ThemeAssetIconEngine:%1:%2:%3:%4")
        .arg(snapshot_ ? snapshot_->generation : 0)
        .arg(onLogicalId_, offLogicalId_,
             overrideColor_.isValid() ? overrideColor_.name(QColor::HexArgb)
                                      : QString());
}

}
