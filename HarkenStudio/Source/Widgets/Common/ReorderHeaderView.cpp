
#include "ReorderHeaderView.h"
#include "TableItemDelegate.h"

#include <QApplication>
#include <QEvent>
#include <QEasingCurve>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScopedValueRollback>
#include <QStyle>
#include <QStyleOptionHeaderV2>

namespace VNX {

namespace {

constexpr int   kSlideDurationMs = 140;
constexpr int   kDropDurationMs  = 120;
constexpr qreal kGhostOpacity    = 0.9;

}

ReorderHeaderView::ReorderHeaderView(Qt::Orientation orientation, QWidget* parent)
    : QHeaderView(orientation, parent)
{
    setSectionsMovable(true);

    slideAnim_ = new QPropertyAnimation(this, "slideProgress", this);
    slideAnim_->setDuration(kSlideDurationMs);
    slideAnim_->setEasingCurve(QEasingCurve::OutCubic);
    slideAnim_->setStartValue(0.0);
    slideAnim_->setEndValue(1.0);

    dropAnim_ = new QPropertyAnimation(this, "ghostX", this);
    dropAnim_->setDuration(kDropDurationMs);
    dropAnim_->setEasingCurve(QEasingCurve::OutCubic);
    connect(dropAnim_, &QPropertyAnimation::finished,
            this, &ReorderHeaderView::finishDrag);
    connect(this, &QHeaderView::sectionMoved,
            this, &ReorderHeaderView::restorePinnedTrailingSections);
}

void ReorderHeaderView::setPinnedTrailingSections(const QList<int>& sections)
{
    for (int section : sections) Q_ASSERT(section >= 0 && section < count());
    cancelDrag();
    pinnedTrailingSections_ = sections;
    restorePinnedTrailingSections();
}

void ReorderHeaderView::restorePinnedTrailingSections()
{
    if (restoringPinnedSections_) return;
    const QScopedValueRollback guard(restoringPinnedSections_, true);
    for (int i = pinnedTrailingSections_.size() - 1; i >= 0; --i) {
        const int from = visualIndex(pinnedTrailingSections_[i]);
        const int to = count() - pinnedTrailingSections_.size() + i;
        if (from >= 0 && from != to) moveSection(from, to);
    }
}

void ReorderHeaderView::initStyleOptionForIndex(QStyleOptionHeader* option, int logicalIndex) const
{
    QHeaderView::initStyleOptionForIndex(option, logicalIndex);
    if (property("columnAccents").toBool())
        option->state.setFlag(QStyle::State_On,
            model()->headerData(logicalIndex, orientation(), TableColumnAccentRole).toBool());
}

void ReorderHeaderView::setSlideProgress(qreal progress)
{
    slideProgress_ = progress;
    emit sectionPositionsChanged();
    viewport()->update();
}

void ReorderHeaderView::setGhostX(qreal x)
{
    ghostX_ = x;
    emit sectionPositionsChanged();
    viewport()->update();
}

int ReorderHeaderView::handleAt(int x) const
{
    int visual = visualIndexAt(x);
    if (visual < 0)
        return -1;
    const int logical = logicalIndex(visual);
    const int pos = sectionViewportPosition(logical);
    const int grip = style()->pixelMetric(QStyle::PM_HeaderGripMargin, nullptr, this);
    if (x < pos + grip) {
        while (visual > 0) {
            const int previous = logicalIndex(--visual);
            if (!isSectionHidden(previous))
                return previous;
        }
        return -1;
    }
    if (x > pos + sectionSize(logical) - grip)
        return logical;
    return -1;
}

bool ReorderHeaderView::isMovableSection(int logical) const
{
    if (orientation() != Qt::Horizontal || isRightToLeft() || !sectionsMovable())
        return false;
    if (logical < 0 || isSectionHidden(logical) || pinnedTrailingSections_.contains(logical))
        return false;
    return logical != 0 || isFirstSectionMovable();
}

bool ReorderHeaderView::event(QEvent* event)
{
    if ((event->type() == QEvent::UngrabMouse
         || event->type() == QEvent::WindowDeactivate
         || event->type() == QEvent::Hide)
        && (pressLogical_ >= 0 || dragging_))
        cancelDrag();
    return QHeaderView::event(event);
}

bool ReorderHeaderView::eventFilter(QObject* watched, QEvent* event)
{
    if (dragging_ && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        cancelDrag();
        return true;
    }
    return QHeaderView::eventFilter(watched, event);
}

void ReorderHeaderView::mousePressEvent(QMouseEvent* event)
{
    if (dragging_)
        return;
    if (event->button() == Qt::LeftButton) {
        const int x = event->position().toPoint().x();
        const int logical = logicalIndexAt(x);
        if (handleAt(x) < 0 && logical >= 0 && sectionsMovable()
            && orientation() == Qt::Horizontal && !isRightToLeft()) {
            pressLogical_ = logical;
            pressX_ = x;
            return;
        }
    }
    QHeaderView::mousePressEvent(event);
}

void ReorderHeaderView::mouseMoveEvent(QMouseEvent* event)
{
    if (dropping_)
        return;
    if (pressLogical_ >= 0 && !isMovableSection(pressLogical_))
        return;
    const int x = event->position().toPoint().x();
    if (!dragging_ && pressLogical_ >= 0
        && qAbs(x - pressX_) >= QApplication::startDragDistance())
        beginDrag(x);

    if (dragging_) {
        setGhostX(x - grabDx_);
        updateTarget();
        return;
    }
    QHeaderView::mouseMoveEvent(event);
}

void ReorderHeaderView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QHeaderView::mouseReleaseEvent(event);
        return;
    }
    if (dragging_) {
        if (dropping_)
            return;
        dropping_ = true;
        dropAnim_->setStartValue(ghostX_);
        dropAnim_->setEndValue(qreal(slotLeft()));
        dropAnim_->start();
        return;
    }
    if (pressLogical_ >= 0) {
        const int logical = pressLogical_;
        pressLogical_ = -1;
        if (sectionsClickable())
            emit sectionClicked(logical);
        return;
    }
    QHeaderView::mouseReleaseEvent(event);
}

void ReorderHeaderView::beginDrag(int x)
{
    dragging_ = true;
    qApp->installEventFilter(this);
    dragLogical_ = pressLogical_;
    dragFrom_ = visualIndex(dragLogical_);
    dragTarget_ = dragFrom_;
    grabDx_ = pressX_ - sectionViewportPosition(dragLogical_);
    slideProgress_ = 1.0;
    shiftFrom_.clear();
    shiftTo_.clear();
    setGhostX(x - grabDx_);
}

// Drop slots are hit-tested against the unshifted layout so the thresholds stay
// put for the whole drag; testing against the animated positions would move the
// threshold with the section it just displaced and make the target flip-flop.
void ReorderHeaderView::updateTarget()
{
    const int center = qRound(ghostX_) + sectionSize(dragLogical_) / 2;
    int target = dragFrom_;
    for (int visual = 0; visual < count(); ++visual) {
        const int logical = logicalIndex(visual);
        if (logical == dragLogical_ || !isMovableSection(logical))
            continue;
        const int middle = sectionViewportPosition(logical) + sectionSize(logical) / 2;
        if (visual < dragFrom_) {
            if (center < middle) {
                target = visual;
                break;
            }
        } else if (visual > dragFrom_ && center >= middle) {
            target = visual;
        }
    }
    if (!isFirstSectionMovable() && logicalIndex(0) == 0)
        target = qMax(target, 1);

    if (target != dragTarget_)
        retarget(target);
}

void ReorderHeaderView::retarget(int visual)
{
    QHash<int, qreal> snapshot;
    for (int i = 0; i < count(); ++i) {
        const int logical = logicalIndex(i);
        snapshot.insert(logical, shiftOf(logical));
    }

    dragTarget_ = visual;
    const qreal width = sectionSize(dragLogical_);
    shiftTo_.clear();
    for (int i = 0; i < count(); ++i) {
        const int logical = logicalIndex(i);
        if (logical == dragLogical_)
            continue;
        if (dragTarget_ > dragFrom_ && i > dragFrom_ && i <= dragTarget_)
            shiftTo_.insert(logical, -width);
        else if (dragTarget_ < dragFrom_ && i >= dragTarget_ && i < dragFrom_)
            shiftTo_.insert(logical, width);
    }
    shiftFrom_ = snapshot;

    slideAnim_->stop();
    setSlideProgress(0.0);
    slideAnim_->start();
}

void ReorderHeaderView::cancelDrag()
{
    qApp->removeEventFilter(this);
    slideAnim_->stop();
    dropAnim_->stop();
    pressLogical_ = -1;
    dragging_ = false;
    dropping_ = false;
    dragLogical_ = -1;
    dragFrom_ = -1;
    dragTarget_ = -1;
    slideProgress_ = 1.0;
    shiftFrom_.clear();
    shiftTo_.clear();
    emit sectionPositionsChanged();
    viewport()->update();
}

void ReorderHeaderView::finishDrag()
{
    qApp->removeEventFilter(this);
    const int from = dragFrom_;
    const int to = dragTarget_;
    slideAnim_->stop();
    dragging_ = false;
    dropping_ = false;
    pressLogical_ = -1;
    dragLogical_ = -1;
    slideProgress_ = 1.0;
    shiftFrom_.clear();
    shiftTo_.clear();
    if (from != to)
        moveSection(from, to);
    emit sectionPositionsChanged();
    viewport()->update();
}

QRect ReorderHeaderView::sectionDisplayRect(int logical) const
{
    const int left = isDraggedSection(logical) ? qRound(ghostX_)
        : sectionViewportPosition(logical) + (dragging_ ? qRound(shiftOf(logical)) : 0);
    return QRect(left, 0, sectionSize(logical), viewport()->height());
}

bool ReorderHeaderView::isDraggedSection(int logical) const
{
    return dragging_ && logical == dragLogical_;
}

qreal ReorderHeaderView::shiftOf(int logical) const
{
    const qreal from = shiftFrom_.value(logical, 0.0);
    const qreal to = shiftTo_.value(logical, 0.0);
    return from + (to - from) * slideProgress_;
}

int ReorderHeaderView::slotLeft() const
{
    const int logical = logicalIndex(dragTarget_);
    if (dragTarget_ > dragFrom_)
        return sectionViewportPosition(logical) + sectionSize(logical)
             - sectionSize(dragLogical_);
    return sectionViewportPosition(logical);
}

void ReorderHeaderView::paintSection(QPainter* painter, const QRect& rect,
                                     int logicalIndex) const
{
    if (dragging_ && logicalIndex == dragLogical_)
        return;
    const qreal shift = dragging_ ? shiftOf(logicalIndex) : 0.0;
    QHeaderView::paintSection(painter,
                              shift == 0.0 ? rect : rect.translated(qRound(shift), 0),
                              logicalIndex);
}

void ReorderHeaderView::paintEvent(QPaintEvent* event)
{
    QHeaderView::paintEvent(event);
    if (!dragging_)
        return;

    const QRect slot(slotLeft(), 0, sectionSize(dragLogical_), viewport()->height());
    QPainter painter(viewport());

    QStyleOptionHeaderV2 opt;
    initStyleOption(&opt);
    opt.section = dragLogical_;
    opt.position = QStyleOptionHeader::Middle;
    opt.rect = slot;
    style()->drawControl(QStyle::CE_Header, &opt, &painter, this);

    painter.setOpacity(kGhostOpacity);
    QHeaderView::paintSection(&painter,
                              QRect(qRound(ghostX_), 0, slot.width(), slot.height()),
                              dragLogical_);
}

}
