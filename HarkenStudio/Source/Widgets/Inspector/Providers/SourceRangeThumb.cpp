#include "Widgets/Inspector/Providers/SourceRangeThumb.h"

#include "Core/SoundSourceSemantics.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/WaveformRenderer.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace VNX {

namespace {

int frameX(uint64_t frame, uint64_t total, const QRectF& r)
{
    if (total == 0)
        return static_cast<int>(std::lround(r.left()));
    const double ratio = std::clamp(
        static_cast<double>(frame) / static_cast<double>(total), 0.0, 1.0);
    return static_cast<int>(std::lround(r.left() + ratio * r.width()));
}

}

SourceRangeThumb::SourceRangeThumb(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("sourceRangeThumb"));
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QtControlSkin::setSemanticProperty(this, "fieldRole", QStringLiteral("preview"));
}

void SourceRangeThumb::setSource(const SoundSourceDef& source, const AssetInfo* asset)
{
    source_ = source;
    asset_ = asset ? std::optional<AssetInfo>{*asset} : std::nullopt;
    peaks_ = {};
    if (asset && !asset->absolutePath.isEmpty())
        WaveformCache::load(asset->absolutePath, peaks_);
    const auto range = SoundSourceSemantics::effectiveRange(source_, asset);
    const bool warning = range && !range->intact();
    QtControlSkin::setSemanticProperty(this, "rangeWarning", warning);
    update();
}

void SourceRangeThumb::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    QtControlSkin::paintFieldFrame(painter, this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& colors = ThemeManager::instance().colors();
    const auto& style = ThemeManager::instance().snapshot()->domains.waveform;
    const QRect inner = rect().adjusted(2, 2, -2, -2);
    if (peaks_.isValid()) {
        WaveformRenderer::render(painter, inner, peaks_, style);
    } else {
        painter.setPen(colors.textMuted);
        painter.drawText(inner, Qt::AlignCenter,
                         asset_ ? tr("Waveform unavailable") : tr("Missing source"));
        return;
    }

    const uint64_t total = asset_ ? asset_->metadata.totalSamples : 0;
    if (total == 0)
        return;

    const QRectF r = QRectF(inner);
    const uint64_t trimStart = source_.trimStartFrame;
    const uint64_t trimEnd = source_.trimEndFrame.value_or(total);
    QColor dim = colors.bgPrimary;
    dim.setAlpha(160);
    const int trimStartX = frameX(trimStart, total, r);
    const int trimEndX = frameX(trimEnd, total, r);
    if (trimStartX > r.left())
        painter.fillRect(QRectF(r.left(), r.top(), trimStartX - r.left(), r.height()), dim);
    if (trimEndX < r.right())
        painter.fillRect(QRectF(trimEndX, r.top(), r.right() - trimEndX, r.height()), dim);

    if (source_.loopRegion) {
        const int loopStartX = frameX(source_.loopRegion->startFrame, total, r);
        const int loopEndX = frameX(source_.loopRegion->endFrame, total, r);
        QColor loopFill = style.loopHandle;
        loopFill.setAlpha(70);
        painter.fillRect(QRect(loopStartX, inner.top(),
                               std::max(1, loopEndX - loopStartX), inner.height()),
                         loopFill);
        painter.setPen(QPen(style.loopHandle, 1));
        painter.drawLine(loopStartX, inner.top(), loopStartX, inner.bottom());
        painter.drawLine(loopEndX, inner.top(), loopEndX, inner.bottom());
    }

    painter.setPen(QPen(style.trimHandle, 1));
    painter.drawLine(trimStartX, inner.top(), trimStartX, inner.bottom());
    painter.drawLine(trimEndX, inner.top(), trimEndX, inner.bottom());
}

void SourceRangeThumb::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked();
}

}
