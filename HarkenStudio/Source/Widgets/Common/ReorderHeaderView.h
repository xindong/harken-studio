
#pragma once

#include <QHash>
#include <QHeaderView>

class QEvent;
class QPropertyAnimation;

namespace VNX {

// Horizontal header whose section reordering reflows live: the dragged section
// floats under the cursor while the sections it crosses slide out of its way,
// and it eases into the opened slot on release. QHeaderView only moves a
// translucent copy and applies the move on release, which reads as a jump.
class ReorderHeaderView : public QHeaderView
{
    Q_OBJECT
    Q_PROPERTY(qreal slideProgress READ slideProgress WRITE setSlideProgress)
    Q_PROPERTY(qreal ghostX READ ghostX WRITE setGhostX)

public:
    explicit ReorderHeaderView(Qt::Orientation orientation,
                               QWidget* parent = nullptr);
    void setPinnedTrailingSections(const QList<int>& sections);

signals:
    void sectionPositionsChanged();

protected:
    void initStyleOptionForIndex(QStyleOptionHeader* option, int logicalIndex) const override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void paintSection(QPainter* painter, const QRect& rect,
                      int logicalIndex) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    virtual bool isMovableSection(int logical) const;
    QRect sectionDisplayRect(int logical) const;
    bool isDraggedSection(int logical) const;
    void cancelDrag();

private:
    qreal slideProgress() const { return slideProgress_; }
    void setSlideProgress(qreal progress);
    qreal ghostX() const { return ghostX_; }
    void setGhostX(qreal x);

    int handleAt(int x) const;
    void restorePinnedTrailingSections();
    void beginDrag(int x);
    void updateTarget();
    void retarget(int visual);
    void finishDrag();
    qreal shiftOf(int logical) const;
    int slotLeft() const;

    QPropertyAnimation* slideAnim_ = nullptr;
    QPropertyAnimation* dropAnim_  = nullptr;
    QList<int> pinnedTrailingSections_;
    bool restoringPinnedSections_ = false;

    int  pressLogical_ = -1;
    int  pressX_       = 0;
    bool dragging_     = false;
    bool dropping_     = false;
    int  dragLogical_  = -1;
    int  dragFrom_     = -1;
    int  dragTarget_   = -1;
    int  grabDx_       = 0;

    qreal slideProgress_ = 1.0;
    qreal ghostX_        = 0.0;

    QHash<int, qreal> shiftFrom_;
    QHash<int, qreal> shiftTo_;
};

}
