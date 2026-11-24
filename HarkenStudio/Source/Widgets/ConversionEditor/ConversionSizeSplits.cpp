#include "ConversionSizeSplits.h"

#include "Core/FontManager.h"
#include "Core/NumericFormat.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeLayout.h"
#include "Widgets/Common/AxisCrosshair.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpressionValidator>
#include <QScrollBar>
#include <QSignalBlocker>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace harken::studio {
namespace {

constexpr quint64 kMaxBytes = std::numeric_limits<qint64>::max();
const QStringList kSizeUnits = {QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"),
    QStringLiteral("GB"), QStringLiteral("TB"), QStringLiteral("PB"), QStringLiteral("EB")};
const QRegularExpression kSizeInput(QStringLiteral("^([0-9]+)(?:\\.([0-9]{1,2}))?$"));

quint64 unitScale(int unit)
{
    return quint64{1} << (10 * unit);
}

int sizeUnit(quint64 bytes)
{
    int unit = 0;
    while (unit + 1 < kSizeUnits.size() && bytes >= unitScale(unit + 1)) ++unit;
    return unit;
}

QString sizeValue(quint64 bytes, int unit)
{
    const auto scale = unitScale(unit);
    const auto whole = bytes / scale;
    auto remainder = bytes % scale;
    if (!remainder) return QString::number(whole);
    int hundredths = 0;
    for (int digit = 0; digit < 2; ++digit) {
        remainder *= 10;
        hundredths = hundredths * 10 + static_cast<int>(remainder / scale);
        remainder %= scale;
    }
    if (remainder * 2 >= scale) ++hundredths;
    if (hundredths == 100) {
        if (whole != 1023) return QString::number(whole + 1);
        hundredths = 99;
    }
    if (!hundredths) return QString::number(whole);
    QString text = QStringLiteral("%1.%2").arg(whole).arg(hundredths, 2, 10, QLatin1Char('0'));
    if (hundredths % 10 == 0) text.chop(1);
    return text;
}

std::optional<quint64> parseSize(const QString& text, int unit)
{
    const auto match = kSizeInput.match(text);
    if (!match.hasMatch()) return std::nullopt;
    bool valid = false;
    const auto whole = match.capturedView(1).toULongLong(&valid);
    if (!valid) return std::nullopt;
    const auto scale = unitScale(unit);
    const auto digits = match.capturedView(2);
    const auto hundredths = digits.toUInt() * (digits.size() == 1 ? 10 : 1);
    if (unit == 0 && hundredths != 0) return std::nullopt;
    const auto fractionBytes = (scale / 100) * hundredths + ((scale % 100) * hundredths + 50) / 100;
    if (whole > (kMaxBytes - fractionBytes) / scale) return std::nullopt;
    return whole * scale + fractionBytes;
}

int editableSizeUnit(quint64 bytes)
{
    int unit = sizeUnit(bytes);
    while (unit > 0 && parseSize(sizeValue(bytes, unit), unit) != bytes) --unit;
    return unit;
}

double sizePosition(quint64 bytes)
{
    const double root = std::sqrt(static_cast<double>(bytes) / unitScale(2));
    return root / (1.0 + root);
}

quint64 sizeAt(double position)
{
    if (position <= 0) return 0;
    if (position >= 1) return kMaxBytes;
    const double root = position / (1.0 - position);
    const double bytes = root * root * unitScale(2);
    if (bytes >= static_cast<double>(kMaxBytes)) return kMaxBytes;
    const auto rounded = static_cast<quint64>(bytes + 0.5);
    const auto step = quint64{1} << std::max(0, static_cast<int>(std::bit_width(rounded)) - 8);
    return std::min(kMaxBytes, (rounded + step / 2) / step * step);
}

}

QString conversionSizeText(quint64 bytes)
{
    const int unit = sizeUnit(bytes);
    return sizeValue(bytes, unit) + QLatin1Char(' ') + kSizeUnits[unit];
}

QString exactConversionSizeText(quint64 bytes)
{
    const int unit = editableSizeUnit(bytes);
    return sizeValue(bytes, unit) + QLatin1Char(' ') + kSizeUnits[unit];
}

ConversionSizeField::ConversionSizeField(quint64 bytes, QWidget* parent)
    : QWidget(parent), value_(new QLineEdit(this)), unit_(new QComboBox(this)), bytes_(bytes)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    ThemeLayout::spacing(layout, ThemeLayout::Space::Control);
    layout->addWidget(value_, 7);
    layout->addWidget(unit_, 3);
    const int unit = editableSizeUnit(bytes);
    unit_->addItems(kSizeUnits);
    unit_->setCurrentIndex(unit);
    value_->setMaxLength(80);
    value_->setValidator(new QRegularExpressionValidator(kSizeInput, value_));
    value_->setText(sizeValue(bytes, unit));
    value_->selectAll();
    setFocusProxy(value_);
    connect(value_, &QLineEdit::textChanged, this, [this](const QString& text) {
        bytes_ = parseSize(text, unit_->currentIndex());
    });
    connect(unit_, &QComboBox::currentIndexChanged, this, [this](int next) {
        if (bytes_) {
            const QSignalBlocker blocker(value_);
            value_->setText(sizeValue(*bytes_, next));
        } else {
            bytes_ = parseSize(value_->text(), next);
        }
    });
}

std::optional<quint64> ConversionSizeField::bytes() const
{
    return bytes_;
}

ConversionSizeSplits::ConversionSizeSplits(QWidget* parent)
    : QAbstractScrollArea(parent), addMarker_(new AxisIndicatorButton(QString::fromLatin1(ThemeAssetIds::TableAdd), viewport()))
{
    setObjectName(QStringLiteral("conversionSizeSplits"));
    setAccessibleName(tr("File size splits"));
    setAccessibleDescription(tr("Hover to read the file size. Click the plus marker or use the context menu to add a split. Click a block to select its rule. Drag a divider to adjust it or double-click it to enter an exact size."));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setAutoFillBackground(false);
    viewport()->setMouseTracking(true);
    addMarker_->setObjectName(QStringLiteral("conversionSplitAddMarker"));
    addMarker_->setAccessibleName(tr("Add split at the previewed size"));
    addMarker_->hide();
    connect(addMarker_, &QAbstractButton::clicked, this, [this] {
        if (hover_ && hover_->splitBytes) emit splitRequested(hover_->index, *hover_->splitBytes);
    });
    auto& theme = ThemeManager::instance();
    theme.bindMetrics(this, [this](const ThemeMetrics&) { updateGeometryForLimits(); });
    connect(&theme, &ThemeManager::appearanceChanged, viewport(), qOverload<>(&QWidget::update));
}

int ConversionSizeSplits::barHeight() const
{
    const auto& metrics = ThemeManager::instance().metrics();
    return std::max(metrics.control.compactHeight, fontMetrics().height() + 2 * metrics.layout.tightGap);
}

void ConversionSizeSplits::updateGeometryForLimits()
{
    const auto& theme = ThemeManager::instance();
    const auto& metrics = theme.metrics();
    const auto font = fontMetrics();
    const QFontMetrics caption(FontManager::instance().getFont(FontRole::Caption));
    addMarker_->setFixedSize(metrics.control.compactHeight, metrics.control.compactHeight);
    const int count = static_cast<int>(limits_.size()) + 1;
    const int numberWidth = font.horizontalAdvance(QString::number(count)) + 2 * metrics.layout.tightGap;
    const int minimumBlockWidth = std::max(metrics.table.compactColumnWidth, numberWidth) + 2 * QtControlSkin::current().tableResizeGripWidth(this);
    minimumSpan_ = minimumBlockWidth + metrics.accentBorderWidth;
    const int minimumWidth = (count - 1) * minimumSpan_ + minimumBlockWidth;
    axisWidth_ = std::max(width() - minimumWidth, metrics.control.fieldMinimumWidth);
    canvasWidth_ = minimumWidth + axisWidth_;
    const int scaleHeight = std::max({font.height(), caption.height(), qCeil(theme.snapshot()->domains.curve.metrics.readoutHeight)});
    scaleBand_ = QRect(0, 0, canvasWidth_, scaleHeight);
    barTop_ = scaleHeight + metrics.layout.tightGap;
    const int markerTop = barTop_ + barHeight() + metrics.layout.tightGap;
    addMarker_->move(addMarker_->x(), markerTop);
    horizontalScrollBar()->setRange(0, std::max(0, canvasWidth_ - width()));
    horizontalScrollBar()->setPageStep(width());
    horizontalScrollBar()->setSingleStep(minimumSpan_);
    const int scrollHeight = canvasWidth_ > width() ? horizontalScrollBar()->sizeHint().height() : 0;
    setFixedHeight(markerTop + addMarker_->height() + scrollHeight);
    updateHover(hoverPosition_);
}

void ConversionSizeSplits::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateGeometryForLimits();
}

void ConversionSizeSplits::scrollContentsBy(int, int)
{
    updateHover(hoverPosition_);
}

void ConversionSizeSplits::changeEvent(QEvent* event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) updateGeometryForLimits();
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        cancelDrag();
        updateHover({-1, -1});
    }
}

void ConversionSizeSplits::setLimits(QVector<quint64> limits)
{
    dragging_ = -1;
    if (limits_ != limits) hover_.reset();
    limits_ = std::move(limits);
    updateGeometryForLimits();
}

int ConversionSizeSplits::edge(int index) const
{
    if (index == 0) return 0;
    if (index > limits_.size()) return canvasWidth_;
    return index * minimumSpan_ + qRound(axisWidth_ * sizePosition(limits_[index - 1]));
}

QRect ConversionSizeSplits::boundaryRect(int index) const
{
    const int width = ThemeManager::instance().metrics().accentBorderWidth;
    return {edge(index + 1) - width, barTop_, width, barHeight()};
}

QRect ConversionSizeSplits::rangeRect(int index) const
{
    const int left = edge(index);
    const int right = index < limits_.size() ? boundaryRect(index).left() : canvasWidth_;
    return {left, barTop_, right - left, barHeight()};
}

int ConversionSizeSplits::rangeAt(int x) const
{
    int first = 0;
    int last = static_cast<int>(limits_.size());
    while (first < last) {
        const int middle = first + (last - first) / 2;
        if (x < edge(middle + 1)) last = middle;
        else first = middle + 1;
    }
    return first;
}

QVector<ConversionSizeSplits::TickLabel> ConversionSizeSplits::visibleTicks() const
{
    const auto font = fontMetrics();
    const auto& layout = ThemeManager::instance().metrics().layout;
    const auto labelRect = [this](int left, int width) { return QRect(left, scaleBand_.top(), width, scaleBand_.height()); };
    const int count = static_cast<int>(limits_.size());
    const auto zero = conversionSizeText(0);
    const int zeroWidth = font.horizontalAdvance(zero);
    const int infWidth = font.horizontalAdvance(kPositiveInfinityText);
    QVector<TickLabel> ticks{{labelRect(0, zeroWidth), zero},
        {labelRect(canvasWidth_ - infWidth, infWidth), kPositiveInfinityText}};
    const int offset = horizontalScrollBar()->value();
    const int right = offset + viewport()->width();
    const int leftBound = std::max(offset, zeroWidth + layout.controlGap);
    const int rightBound = std::min(right, canvasWidth_ - infWidth - layout.controlGap);
    if (leftBound >= rightBound) return ticks;
    const auto labelAt = [&](int index) {
        const QString text = font.elidedText(conversionSizeText(limits_[index]), Qt::ElideMiddle, rightBound - leftBound);
        const int width = std::min(font.horizontalAdvance(text) + 2, rightBound - leftBound);
        const int left = std::clamp(boundaryRect(index).center().x() - width / 2, leftBound, rightBound - width);
        return TickLabel{labelRect(left, width), text};
    };
    const int first = rangeAt(offset);
    int last = rangeAt(right - 1);
    if (last == count || boundaryRect(last).left() >= right) --last;
    const int active = activeBoundary();
    const int preferred = active >= 0 ? active : std::min(selected_, count - 1);
    QRect reserved;
    if (preferred >= first && preferred <= last) {
        ticks.append(labelAt(preferred));
        reserved = ticks.back().rect.adjusted(-layout.controlGap, 0, layout.controlGap, 0);
    }
    int nextLeft = leftBound;
    for (int index = first; index <= last; ++index) {
        if (index == preferred) continue;
        auto tick = labelAt(index);
        if (tick.rect.left() < nextLeft || tick.rect.intersects(reserved)) continue;
        nextLeft = tick.rect.right() + 1 + layout.controlGap;
        ticks.append(std::move(tick));
    }
    return ticks;
}

int ConversionSizeSplits::activeBoundary() const
{
    if (dragging_ >= 0) return dragging_;
    return hover_ && hover_->part == SplitTarget::Divider ? hover_->index : -1;
}

std::optional<ConversionSizeSplits::SplitTarget> ConversionSizeSplits::targetAt(const QPoint& point) const
{
    if (!isEnabled() || !viewport()->rect().contains(point)) return std::nullopt;
    const QPoint canvasPoint = point + QPoint(horizontalScrollBar()->value(), 0);
    const int canvasX = canvasPoint.x();
    const int range = rangeAt(canvasX);
    const auto block = rangeRect(range);
    if (dragging_ >= 0 || (range < limits_.size() && canvasX > block.right())) {
        const int index = dragging_ >= 0 ? dragging_ : range;
        const auto divider = boundaryRect(index);
        const auto part = divider.contains(canvasPoint) ? SplitTarget::Divider : SplitTarget::Scale;
        return SplitTarget{part, index, canvasX, std::nullopt};
    }
    const auto part = block.contains(canvasPoint) ? SplitTarget::Block : SplitTarget::Scale;
    if (!canSplit(range)) return SplitTarget{part, range, canvasX, std::nullopt};
    const double fraction = (canvasX - block.left()) / static_cast<double>(block.width());
    return SplitTarget{part, range, canvasX, valueInRange(range, fraction)};
}

void ConversionSizeSplits::updateHover(QPoint position)
{
    hoverPosition_ = position;
    const auto marker = addMarker_->geometry();
    if (dragging_ < 0 && position.y() >= marker.top()) {
        if (hover_ && hover_->splitBytes && marker.contains(position))
            position.setX(hover_->canvasX - horizontalScrollBar()->value());
        else
            position = {-1, -1};
    }
    hover_ = targetAt(position);
    const bool showMarker = hover_ && hover_->splitBytes;
    if (showMarker) {
        const int left = hover_->canvasX - horizontalScrollBar()->value() - addMarker_->width() / 2;
        addMarker_->move(std::clamp(left, 0, std::max(0, viewport()->width() - addMarker_->width())), marker.top());
    }
    addMarker_->setVisible(showMarker);
    auto cursor = Qt::ArrowCursor;
    if (activeBoundary() >= 0) cursor = Qt::SplitHCursor;
    else if (hover_ && hover_->part == SplitTarget::Block) cursor = Qt::PointingHandCursor;
    if (viewport()->cursor().shape() != cursor) viewport()->setCursor(cursor);
    viewport()->update();
}

bool ConversionSizeSplits::canSplit(int index) const
{
    if (index == -1) index = static_cast<int>(limits_.size());
    if (index < 0 || index > limits_.size()) return false;
    const quint64 first = index ? limits_[index - 1] + 1 : 0;
    return index < limits_.size() ? first < limits_[index] : first <= kMaxBytes;
}

quint64 ConversionSizeSplits::valueInRange(int index, double fraction) const
{
    const double left = index ? sizePosition(limits_[index - 1]) : 0.0;
    const double right = index < limits_.size() ? sizePosition(limits_[index]) : 1.0;
    return std::clamp(sizeAt(std::lerp(left, right, fraction)), index ? limits_[index - 1] + 1 : 0,
                      index < limits_.size() ? limits_[index] - 1 : kMaxBytes);
}

void ConversionSizeSplits::requestSplit(int index)
{
    const int range = index < 0 ? static_cast<int>(limits_.size()) : index;
    if (canSplit(range)) emit splitRequested(range, valueInRange(range, 0.5));
}

void ConversionSizeSplits::setSelectedRange(int index)
{
    selected_ = index;
    const bool underPointer = hover_ && hover_->part == SplitTarget::Block && hover_->index == index;
    if (dragging_ < 0 && !underPointer && index >= 0 && index <= limits_.size()) {
        const auto block = rangeRect(index);
        auto* scroll = horizontalScrollBar();
        if (block.left() < scroll->value() || block.right() >= scroll->value() + viewport()->width())
            scroll->setValue(block.x() + (block.width() - viewport()->width()) / 2);
    }
    viewport()->update();
}

void ConversionSizeSplits::paintEvent(QPaintEvent*)
{
    const auto& theme = ThemeManager::instance();
    const auto& colors = theme.colors();
    const auto& metrics = theme.metrics();
    QPainter painter(viewport());
    const int offset = horizontalScrollBar()->value();
    painter.translate(-offset, 0);
    const int first = rangeAt(offset);
    const int last = rangeAt(offset + viewport()->width());
    for (int index = first; index <= last; ++index) {
        const auto block = rangeRect(index);
        const auto color = colors.categoricalColor(index);
        const bool selected = index == selected_;
        painter.fillRect(block, colors.bgSecondary);
        auto fill = color;
        fill.setAlpha(selected ? 100 : ThemeSoftTintAlpha);
        painter.fillRect(block, fill);
        if (hover_ && hover_->splitBytes && hover_->index == index) {
            painter.save();
            painter.setClipRect(block);
            painter.setPen(QPen(colors.accent, metrics.borderWidth, Qt::DashLine, Qt::FlatCap));
            painter.drawLine(hover_->canvasX, block.top(), hover_->canvasX, block.bottom() + 1);
            painter.restore();
        }
        if (selected) {
            const qreal inset = metrics.accentBorderWidth / 2.0;
            painter.setPen(QPen(color, metrics.accentBorderWidth));
            painter.drawRect(QRectF(block).adjusted(inset, inset, -inset, -inset));
        }
        painter.setPen(colors.textPrimary);
        painter.drawText(block, Qt::AlignCenter, QString::number(index + 1));
    }
    const int active = activeBoundary();
    if (isEnabled() && active >= 0) painter.fillRect(boundaryRect(active), colors.accent);
    painter.setPen(colors.textSecondary);
    for (const auto& tick : visibleTicks()) {
        painter.drawText(tick.rect, Qt::AlignCenter, tick.text);
    }
    if (hover_ && hover_->splitBytes) {
        const auto& curve = theme.snapshot()->domains.curve;
        const auto text = conversionSizeText(*hover_->splitBytes);
        const QFontMetrics caption(FontManager::instance().getFont(FontRole::Caption), painter.device());
        const qreal width = std::min(qreal(viewport()->width()), caption.horizontalAdvance(text) + curve.metrics.readoutHorizontalPadding);
        const qreal left = std::clamp(hover_->canvasX - width / 2, qreal(offset), offset + viewport()->width() - width);
        painter.setRenderHint(QPainter::Antialiasing);
        drawAxisReadout(painter, curve, QRectF(left, scaleBand_.top(), width, scaleBand_.height()), text);
    }
}

void ConversionSizeSplits::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return QAbstractScrollArea::mousePressEvent(event);
    const auto point = event->position().toPoint();
    updateHover(point);
    if (!hover_ || hover_->part == SplitTarget::Scale) return;
    const int x = point.x() + horizontalScrollBar()->value();
    dragging_ = activeBoundary();
    if (dragging_ >= 0) {
        original_ = limits_[dragging_];
        dragOffset_ = x - edge(dragging_ + 1);
    }
    emit rangeSelected(dragging_ >= 0 ? -1 : hover_->index);
    event->accept();
}

void ConversionSizeSplits::mouseMoveEvent(QMouseEvent* event)
{
    const auto point = event->position().toPoint();
    if (dragging_ < 0) {
        updateHover(point);
        return;
    }
    const int x = point.x() + horizontalScrollBar()->value() - dragOffset_;
    const auto first = dragging_ ? limits_[dragging_ - 1] + 1 : 0;
    const auto last = dragging_ + 1 < limits_.size() ? limits_[dragging_ + 1] - 1 : kMaxBytes;
    const double position = (x - (dragging_ + 1) * minimumSpan_) / static_cast<double>(axisWidth_);
    const int start = (dragging_ + 1) * minimumSpan_ + qRound(axisWidth_ * sizePosition(original_));
    const auto bytes = x == start ? original_ : std::clamp(sizeAt(position), first, last);
    if (limits_[dragging_] != bytes) {
        limits_[dragging_] = bytes;
        emit boundaryPreviewed(dragging_, bytes);
    }
    updateHover(point);
}

void ConversionSizeSplits::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || dragging_ < 0) return;
    const int index = std::exchange(dragging_, -1);
    updateHover(event->position().toPoint());
    if (limits_[index] != original_) emit boundaryMoved(index, limits_[index]);
}

void ConversionSizeSplits::cancelDrag()
{
    if (dragging_ < 0) return;
    const int index = std::exchange(dragging_, -1);
    limits_[index] = original_;
    emit boundaryPreviewed(index, original_);
    updateHover(hoverPosition_);
}

void ConversionSizeSplits::hideEvent(QHideEvent* event)
{
    cancelDrag();
    updateHover({-1, -1});
    QAbstractScrollArea::hideEvent(event);
}

void ConversionSizeSplits::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    cancelDrag();
    updateHover(event->position().toPoint());
    if (hover_ && hover_->part == SplitTarget::Divider) {
        const int index = hover_->index;
        const auto bytes = limits_[index];
        emit rangeSelected(-1);
        emit editRequested(index, bytes);
    }
    event->accept();
}

void ConversionSizeSplits::contextMenuEvent(QContextMenuEvent* event)
{
    updateHover(event->pos());
    const auto target = hover_;
    if (!target || !target->splitBytes) return;
    const auto limits = limits_;
    QMenu menu(this);
    auto* add = menu.addAction(tr("Add Split..."));
    if (menu.exec(event->globalPos()) == add && isEnabled() && limits_ == limits)
        emit splitRequested(target->index, *target->splitBytes);
    event->accept();
}

void ConversionSizeSplits::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Left: emit rangeSelected(std::max(0, selected_ - 1)); break;
    case Qt::Key_Right: emit rangeSelected(std::min(static_cast<int>(limits_.size()), selected_ + 1)); break;
    case Qt::Key_Insert: requestSplit(selected_); break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (selected_ >= 0 && selected_ < limits_.size()) emit editRequested(selected_, limits_[selected_]);
        else requestSplit(selected_);
        break;
    case Qt::Key_Escape:
        if (dragging_ >= 0) cancelDrag();
        else emit rangeSelected(-1);
        break;
    default: return QAbstractScrollArea::keyPressEvent(event);
    }
    event->accept();
}

bool ConversionSizeSplits::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::Enter) updateHover(static_cast<QEnterEvent*>(event)->position().toPoint());
    if (event->type() == QEvent::Leave) updateHover({-1, -1});
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate) {
        cancelDrag();
        updateHover({-1, -1});
    }
    return QAbstractScrollArea::viewportEvent(event);
}

}
