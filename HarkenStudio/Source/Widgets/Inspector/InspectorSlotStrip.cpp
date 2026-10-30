
#include "InspectorSlotStrip.h"

#include "Core/ThemeManager.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QSizePolicy>

#include <algorithm>

namespace VNX {

InspectorSlotStrip::InspectorSlotStrip(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("inspectorSlotStrip"));
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);

    const auto& inspector = ThemeManager::instance().metrics().inspector;
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedHeight(inspector.slotHeight);
    for (auto& slot : slots_) {
        if (slot.kind == InspectorSlotKind::Attenuation)
            layout_->addSpacing(inspector.slotSpacing);
        slot.cell = new QWidget(this);
        slot.cell->setFixedSize(inspector.bindingSlotWidth, inspector.slotHeight);
        auto* cellLayout = new QHBoxLayout(slot.cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(0);
        layout_->addWidget(slot.cell);
    }
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
            this, [this] { update(); });
}

void InspectorSlotStrip::addSlot(InspectorSlotKind kind, QWidget* widget)
{
    if (!widget) return;
    for (auto& slot : slots_) {
        if (slot.kind != kind) continue;
        Q_ASSERT(!slot.widget);
        slot.widget = widget;
        slot.decorator = dynamic_cast<IFieldDecorator*>(widget);
        slot.cell->layout()->addWidget(widget);
        widget->show();
        return;
    }
}

int InspectorSlotStrip::occupiedSlotCount() const
{
    return static_cast<int>(std::count_if(slots_.begin(), slots_.end(),
        [](const Slot& slot) { return !slot.widget.isNull(); }));
}

QWidget* InspectorSlotStrip::slotWidget(InspectorSlotKind kind) const
{
    for (const Slot& slot : slots_)
        if (slot.kind == kind) return slot.widget;
    return nullptr;
}

void InspectorSlotStrip::bind(ProjectItem* item)
{
    for (const Slot& slot : slots_)
        if (slot.decorator) slot.decorator->bind(item);
}

QWidget* InspectorSlotStrip::detailWidget() const
{
    const auto details = detailWidgets();
    return details.empty() ? nullptr : details.front();
}

std::vector<QWidget*> InspectorSlotStrip::detailWidgets() const
{
    std::vector<QWidget*> details;
    for (const Slot& slot : slots_) {
        if (!slot.decorator) continue;
        if (QWidget* detail = slot.decorator->detailWidget())
            details.push_back(detail);
    }
    return details;
}

QString InspectorSlotStrip::hoverSummary() const
{
    QStringList parts;
    for (const Slot& slot : slots_) {
        if (!slot.decorator) continue;
        const QString summary = slot.decorator->hoverSummary();
        if (!summary.isEmpty()) parts.push_back(summary);
    }
    return parts.join(QStringLiteral("\n"));
}

bool InspectorSlotStrip::isReadOnly() const
{
    for (const Slot& slot : slots_)
        if (slot.decorator && !slot.decorator->isReadOnly())
            return false;
    return true;
}

QSize InspectorSlotStrip::sizeHint() const
{
    return layout_->sizeHint();
}

QSize InspectorSlotStrip::minimumSizeHint() const
{
    return layout_->minimumSize();
}

void InspectorSlotStrip::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    const QRect left = slots_.front().cell->geometry();
    const QRect right = slots_.back().cell->geometry();
    const int x = (left.x() + left.width() + right.x()) / 2;
    const int separatorHeight = 14;
    const int inset = (height() - separatorHeight) / 2;
    QPainter painter(this);
    painter.fillRect(x, inset, 1, separatorHeight,
                     ThemeManager::instance().colors().borderPrimary);
}

}
