
#include "TreeRowPing.h"
#include "LocationPingAnimation.h"

#include "Core/ThemeManager.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVariantAnimation>
#include <QWidget>

namespace VNX::TreeRowPing {

namespace {

constexpr qreal kPopScale = 1.6;

class PingOverlay : public QWidget
{
public:
    PingOverlay(QTreeWidget* tree, const QRect& capsule)
        : QWidget(tree->viewport()), capsule_(capsule)
    {
        setObjectName(QStringLiteral("treeRowPing"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(tree->viewport()->rect());

        auto* anim = new QVariantAnimation(this);
        anim->setStartValue(0.0);
        anim->setEndValue(1.0);
        anim->setDuration(LocationPingAnimation::kDurationMs);
        connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            t_ = v.toReal();
            update();
        });
        connect(anim, &QVariantAnimation::finished, this, &QWidget::deleteLater);
        anim->start();

        connect(tree->verticalScrollBar(), &QScrollBar::valueChanged,
                this, &QWidget::deleteLater);
        connect(tree->horizontalScrollBar(), &QScrollBar::valueChanged,
                this, &QWidget::deleteLater);

        show();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const qreal pop = LocationPingAnimation::popProgress(t_);
        const qreal scale = kPopScale - (kPopScale - 1.0) * pop;
        const qreal fade = LocationPingAnimation::fadeOpacity(t_);

        QRectF r(capsule_);
        r.adjust(-r.width() * (scale - 1.0) / 2.0, -r.height() * (scale - 1.0) / 2.0,
                 r.width() * (scale - 1.0) / 2.0, r.height() * (scale - 1.0) / 2.0);

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor accent = ThemeManager::instance().colors().accent;
        QColor fill = accent;
        fill.setAlphaF(fill.alphaF() * 0.22 * fade * pop);
        QColor edge = accent;
        edge.setAlphaF(edge.alphaF() * fade);
        const auto& metrics = ThemeManager::instance().metrics();
        p.setPen(QPen(edge, metrics.accentBorderWidth));
        p.setBrush(fill);
        const qreal radius = r.height() / 2.0;
        const qreal inset = metrics.accentBorderWidth / 2.0;
        p.drawRoundedRect(r.adjusted(inset, inset, -inset, -inset), radius, radius);
    }

private:
    QRect capsule_;
    qreal t_ = 0.0;
};

}

void ping(QTreeWidget* tree, QTreeWidgetItem* item)
{
    if (!tree || !item) return;

    delete tree->viewport()->findChild<QWidget*>(
        QStringLiteral("treeRowPing"), Qt::FindDirectChildrenOnly);

    for (QTreeWidgetItem* p = item->parent(); p; p = p->parent())
        p->setExpanded(true);
    tree->resizeColumnToContents(0);
    tree->doItemsLayout();
    tree->scrollToItem(item, QAbstractItemView::PositionAtCenter);

    QRect r = tree->visualItemRect(item);
    if (r.isEmpty()) return;

    const auto& metrics = ThemeManager::instance().metrics();
    const int iconW = tree->iconSize().width() > 0 ? tree->iconSize().width() : metrics.iconExtent;
    const int textW = QFontMetrics(tree->font()).horizontalAdvance(item->text(0));
    const int padding = metrics.spacingSmall * 2 + metrics.spacingXSmall;
    const int visibleWidth = qMin(iconW + textW + padding, tree->viewport()->width());
    auto* scrollBar = tree->horizontalScrollBar();
    if (r.left() < 0)
        scrollBar->setValue(scrollBar->value() + r.left());
    else if (r.left() + visibleWidth > tree->viewport()->width())
        scrollBar->setValue(scrollBar->value() + r.left() + visibleWidth - tree->viewport()->width());
    r = tree->visualItemRect(item);
    const int w = qMin(iconW + textW + padding, tree->viewport()->width() - r.left());
    if (w <= 0) return;

    new PingOverlay(tree, QRect(r.left(), r.top(), w, r.height()));
}

}
