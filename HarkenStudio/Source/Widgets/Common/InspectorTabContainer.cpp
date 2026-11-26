
#include "InspectorTabContainer.h"

#include "Core/ThemeManager.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace harken::studio {

InspectorTabContainer::InspectorTabContainer(QWidget* parent, FramePlacement frame)
    : QFrame(parent), frame_(frame)
{
    if (frame_ == FramePlacement::Group) {
        setFrameShape(QFrame::StyledPanel);
        setProperty("surfaceRole", QStringLiteral("panel"));
    }
    bar_ = new QWidget(this);
    bar_->setObjectName(QStringLiteral("inspectorTabBar"));
    bar_->setAttribute(Qt::WA_StyledBackground, true);
    barLayout_ = new QHBoxLayout(bar_);
    barLayout_->setSpacing(0);

    buttons_ = new QButtonGroup(this);
    buttons_->setExclusive(true);
    connect(buttons_, &QButtonGroup::idClicked, this, [this](int id) { stack_->setCurrentIndex(id); });

    stack_ = new QStackedWidget(this);
    connect(stack_, &QStackedWidget::currentChanged, this, &InspectorTabContainer::currentChanged);
    connect(stack_, &QStackedWidget::currentChanged, this, &QWidget::updateGeometry);

    content_ = new QWidget(this);
    content_->setObjectName(QStringLiteral("inspectorTabContent"));
    content_->setAttribute(Qt::WA_StyledBackground, true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(bar_);
    auto* stackMargin = new QVBoxLayout(content_);
    stackMargin->addWidget(stack_);
    layout->addWidget(content_, 1);
    ThemeManager::instance().bindMetrics(this, [this, stackMargin](const ThemeMetrics& metrics) {
        const int margin = metrics.inspector.pageMargin;
        stackMargin->setContentsMargins(margin, margin, margin, margin);
        barLayout_->setContentsMargins(0, 0, 0, metrics.borderWidth);
        bar_->setFixedHeight(metrics.layout.sectionHeaderHeight);
    });
}

void InspectorTabContainer::addTab(QWidget* contentWidget,
                                    const QString& tabTitle,
                                    const QString& objectName)
{
    if (!contentWidget) return;
    if (!objectName.isEmpty())
        contentWidget->setObjectName(objectName);

    auto* scroll = new QScrollArea(stack_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setProperty("inspectorTabPage", frame_ == FramePlacement::Page);
    scroll->setWidget(contentWidget);

    const int index = stack_->addWidget(scroll);

    auto* button = new QToolButton(bar_);
    button->setText(QString(tabTitle).replace(QLatin1Char('&'), QStringLiteral("&&")));
    button->setAccessibleName(tabTitle);
    button->setToolTip(tabTitle);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    if (!objectName.isEmpty())
        button->setObjectName(objectName + QStringLiteral("Button"));
    buttons_->addButton(button, index);
    barLayout_->addWidget(button, 1);
    button->show();

    if (index == 0 || content_->isHidden())
        setCurrentTab(index);
}

void InspectorTabContainer::clearTabs()
{
    const auto btns = buttons_->buttons();
    for (auto* b : btns)
    {
        buttons_->removeButton(b);
        b->deleteLater();
    }
    while (stack_->count() > 0)
    {
        QWidget* page = stack_->widget(0);
        stack_->removeWidget(page);
        page->deleteLater();
    }
}

void InspectorTabContainer::setCurrentTab(int index)
{
    auto* button = buttons_->button(index);
    if (!button || button->isHidden()) return;
    const bool restoringCurrentPage = content_->isHidden() && stack_->currentIndex() == index;
    bar_->show();
    content_->show();
    stack_->setCurrentIndex(index);
    button->setChecked(true);
    if (restoringCurrentPage) emit currentChanged(index);
}

void InspectorTabContainer::setTabVisible(int index, bool visible)
{
    auto* button = buttons_->button(index);
    if (!button || button->isHidden() == !visible) return;
    button->setVisible(visible);
    auto* current = buttons_->button(stack_->currentIndex());
    if (current && !current->isHidden()) {
        setCurrentTab(stack_->currentIndex());
        return;
    }
    for (int i = 0; i < tabCount(); ++i) {
        if (!buttons_->button(i)->isHidden()) {
            setCurrentTab(i);
            return;
        }
    }
    bar_->hide();
    content_->hide();
    emit currentChanged(-1);
}

int InspectorTabContainer::currentTabIndex() const
{
    return content_->isHidden() ? -1 : stack_->currentIndex();
}

int InspectorTabContainer::tabCount() const
{
    return stack_->count();
}

QWidget* InspectorTabContainer::tabWidget(int index) const
{
    auto* scroll = qobject_cast<QScrollArea*>(stack_->widget(index));
    return scroll ? scroll->widget() : nullptr;
}

QSize InspectorTabContainer::sizeHint() const
{
    QSize hint = QFrame::sizeHint();
    if (frame_ != FramePlacement::Group || currentTabIndex() < 0) return hint;
    const auto* scroll = static_cast<QScrollArea*>(stack_->currentWidget());
    const auto* page = scroll->widget();
    const auto padding = content_->layout()->contentsMargins();
    const int width = contentsRect().width() - padding.left() - padding.right();
    const int minimumWidth = page->minimumSizeHint().width();
    const int wrapWidth = qMax(minimumWidth, width - scroll->verticalScrollBar()->sizeHint().width());
    const int height = page->hasHeightForWidth() ? page->heightForWidth(wrapWidth) : page->sizeHint().height();
    const int horizontalBar = width < minimumWidth ? scroll->horizontalScrollBar()->sizeHint().height() : 0;
    hint.setHeight(bar_->height() + padding.top() + height + horizontalBar + padding.bottom() + 2 * frameWidth());
    return hint;
}

void InspectorTabContainer::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    if (frame_ == FramePlacement::Group && event->size().width() != event->oldSize().width())
        updateGeometry();
}

}
