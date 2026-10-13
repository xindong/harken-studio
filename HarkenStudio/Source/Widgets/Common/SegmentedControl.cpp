#include "SegmentedControl.h"

#include <QButtonGroup>
#include <QEvent>
#include <QHBoxLayout>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>

namespace VNX {
namespace {

class SegmentButton final : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        const int textWidth = option.fontMetrics.size(Qt::TextShowMnemonic, option.text).width();
        const int chromeWidth = sizeHint().width() - textWidth;
        option.text = option.fontMetrics.elidedText(option.text, Qt::ElideRight,
            qMax(0, width() - chromeWidth), Qt::TextShowMnemonic);
        QStylePainter painter(this);
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
    }
};

}

SegmentedControl::SegmentedControl(QWidget* parent)
    : QWidget(parent)
    , group_(new QButtonGroup(this))
{
    setProperty("segmentedControl", true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
}

void SegmentedControl::setOptions(const QList<QPair<QString, QVariant>>& options,
                                  bool exclusive)
{
    group_->setExclusive(exclusive);
    auto* layout = static_cast<QHBoxLayout*>(this->layout());
    const auto oldButtons = group_->buttons();
    for (auto* button : oldButtons) {
        group_->removeButton(button);
        delete button;
    }
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item;
    }
    const int count = options.size();
    for (int i = 0; i < count; ++i) {
        auto* button = new SegmentButton(this);
        button->setText(options.at(i).first);
        button->setToolTip(options.at(i).first);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(adaptiveSizing_ ? QSizePolicy::Ignored : QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setProperty("segmentedControl", true);
        button->setProperty("segmentValue", options.at(i).second);
        const QString position = count == 1
            ? QStringLiteral("only")
            : (i == 0 ? QStringLiteral("first")
                      : (i == count - 1 ? QStringLiteral("last")
                                         : QStringLiteral("joined")));
        button->setProperty("segmentPosition", position);
        group_->addButton(button, i);
        layout->addWidget(button, 1);
    }
    scheduleLayoutUpdate();
    updateGeometry();
}

void SegmentedControl::setAdaptiveSizing(bool enabled)
{
    adaptiveSizing_ = enabled;
    setSizePolicy(enabled ? QSizePolicy::Maximum : QSizePolicy::Expanding, QSizePolicy::Preferred);
    for (auto* button : group_->buttons())
        button->setSizePolicy(enabled ? QSizePolicy::Ignored : QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout()->setSizeConstraint(enabled ? QLayout::SetNoConstraint : QLayout::SetDefaultConstraint);
    scheduleLayoutUpdate();
    updateGeometry();
}

QSize SegmentedControl::sizeHint() const
{
    if (!adaptiveSizing_) return QWidget::sizeHint();
    const auto buttons = group_->buttons();
    QSize size(qMax(0, int(buttons.size()) - 1) * layout()->spacing(), 0);
    for (auto* button : buttons) {
        size.rwidth() += button->sizeHint().width();
        size.setHeight(qMax(size.height(), button->sizeHint().height()));
    }
    return size;
}

QSize SegmentedControl::minimumSizeHint() const
{
    return adaptiveSizing_ ? QSize(0, sizeHint().height()) : QWidget::minimumSizeHint();
}

bool SegmentedControl::event(QEvent* event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest) scheduleLayoutUpdate();
    return handled;
}

void SegmentedControl::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        layout()->invalidate();
        scheduleLayoutUpdate();
        updateGeometry();
    }
}

void SegmentedControl::scheduleLayoutUpdate()
{
    if (layoutUpdatePending_) return;
    layoutUpdatePending_ = true;
    QMetaObject::invokeMethod(this, [this]() {
        layoutUpdatePending_ = false;
        auto* layout = static_cast<QHBoxLayout*>(this->layout());
        bool changed = false;
        for (int i = 0; auto* option = button(i); ++i) {
            const int weight = adaptiveSizing_ ? qMax(1, option->sizeHint().width()) : 1;
            if (layout->stretch(i) == weight) continue;
            layout->setStretch(i, weight);
            changed = true;
        }
        if (changed) updateGeometry();
    }, Qt::QueuedConnection);
}

QToolButton* SegmentedControl::button(int index) const
{
    return qobject_cast<QToolButton*>(group_->button(index));
}

}
