
#include "InspectorTabContainer.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include <QBoxLayout>
#include <QButtonGroup>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>

namespace VNX {

InspectorTabContainer::InspectorTabContainer(QWidget* parent)
    : QWidget(parent)
{
    bar_ = new QWidget(this);
    bar_->setObjectName(QStringLiteral("inspectorTabBar"));
    bar_->setAttribute(Qt::WA_StyledBackground, true);
    bar_->setProperty("sideBar", false);
    barLayout_ = new QBoxLayout(QBoxLayout::LeftToRight, bar_);
    barLayout_->setContentsMargins(0, 0, 0, 1);
    barLayout_->setSpacing(0);

    buttons_ = new QButtonGroup(this);
    buttons_->setExclusive(true);
    connect(buttons_, &QButtonGroup::idClicked, this, [this](int id) { stack_->setCurrentIndex(id); });

    stack_ = new QStackedWidget(this);

    rootLayout_ = new QBoxLayout(QBoxLayout::TopToBottom, this);
    rootLayout_->setContentsMargins(0, 0, 0, 0);
    rootLayout_->setSpacing(0);
    rootLayout_->addWidget(bar_);
    const int pageMargin =
        ThemeManager::instance().metrics().inspector.pageMargin;
    auto* stackMargin = new QVBoxLayout();
    stackMargin->setContentsMargins(pageMargin, pageMargin,
                                    pageMargin, pageMargin);
    stackMargin->addWidget(stack_);
    rootLayout_->addLayout(stackMargin, 1);
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
    scroll->setProperty("inspectorTabPage", true);
    scroll->setWidget(contentWidget);

    const int index = stack_->addWidget(scroll);

    auto* button = new QToolButton(bar_);
    button->setText(tabTitle);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setProperty("sideBar", sideBar_);
    // Horizontal bar: Ignored keeps the buttons equally split and shrinkable
    // to zero, so the tab bar never imposes a minimum width on the panel
    // (QStackedWidget takes the max over ALL built pages, current or not).
    // Sidebar keeps Expanding so the bar width still tracks the label text.
    button->setSizePolicy(sideBar_ ? QSizePolicy::Expanding : QSizePolicy::Ignored,
                          QSizePolicy::Expanding);
    if (!objectName.isEmpty())
        button->setObjectName(objectName + QStringLiteral("Button"));
    buttons_->addButton(button, index);
    barLayout_->addWidget(button, 1);

    if (index == 0)
    {
        button->setChecked(true);
        stack_->setCurrentIndex(0);
    }
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
    if (index < 0 || index >= stack_->count()) return;
    stack_->setCurrentIndex(index);
    if (auto* b = buttons_->button(index))
        b->setChecked(true);
}

int InspectorTabContainer::currentTabIndex() const
{
    return stack_->currentIndex();
}

int InspectorTabContainer::tabCount() const
{
    return stack_->count();
}

void InspectorTabContainer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const int h = height();
    if (h <= 0) return;
    const double ratio = static_cast<double>(width()) / h;
    if (!sideBar_ && ratio > 1.52)
        applyOrientation(true);
    else if (sideBar_ && ratio < 1.48)
        applyOrientation(false);
}

void InspectorTabContainer::applyOrientation(bool sideBar)
{
    sideBar_ = sideBar;
    rootLayout_->setDirection(sideBar ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    barLayout_->setDirection(sideBar ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    barLayout_->setContentsMargins(0, 0, sideBar ? 1 : 0, sideBar ? 0 : 1);
    QtControlSkin::setSemanticProperty(bar_, "sideBar", sideBar);
    for (auto* b : buttons_->buttons())
    {
        QtControlSkin::setSemanticProperty(b, "sideBar", sideBar);
        b->setSizePolicy(sideBar ? QSizePolicy::Expanding : QSizePolicy::Ignored,
                         QSizePolicy::Expanding);
    }
}

}
