#include "ListFooter.h"
#include "Core/ContextInfo.h"

#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QToolButton>

#include <algorithm>

namespace VNX {

ListFooter::ListFooter(QWidget* parent, bool standardButtons, ThemeManager* themeManager)
    : QWidget(parent), themeManager_(themeManager ? themeManager : &ThemeManager::instance())
{
    setObjectName(QStringLiteral("listFoot"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QHBoxLayout(this);
    layout->addStretch(1);
    if (standardButtons) {
        auto* add = new QAction(tr("Add"), this);
        add->setToolTip(tr("Add"));
        addButton_ = addActionButton(add, true);
        ContextInfo::tag(addButton_, ContextTopic::forPanel(QStringLiteral("list.add")));
        auto* remove = new QAction(tr("Remove"), this);
        remove->setToolTip(tr("Remove"));
        removeButton_ = addActionButton(remove, true);
        ContextInfo::tag(removeButton_, ContextTopic::forPanel(QStringLiteral("list.remove")));
        removeButton_->setEnabled(false);
    }
    connect(themeManager_, &ThemeManager::themeChanged, this, [this] { applyTheme(); });
    connect(themeManager_, &ThemeManager::appearanceChanged, this, &ListFooter::refreshIcons);
    applyTheme();
}

QToolButton* ListFooter::addActionButton(QAction* action, bool compact, Position position,
                                        const QString& iconAsset)
{
    auto* button = new QToolButton(this);
    button->setProperty("listFootBtn", true);
    button->setProperty("tableCompactAction", compact);
    button->setProperty("flatIconButton", compact);
    button->setDefaultAction(action);
    button->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextOnly);
    button->setProperty("tableIconAsset", iconAsset);
    button->setAccessibleName(action->toolTip());
    button->setCursor(Qt::PointingHandCursor);
    auto* row = static_cast<QHBoxLayout*>(layout());
    if (position == Position::Left) row->insertWidget(leftCount_++, button);
    else row->addWidget(button);
    applyTheme();
    return button;
}

QToolButton* ListFooter::setRefreshAction(QAction* action)
{
    if (!refreshButton_)
        refreshButton_ = addActionButton(action, true, Position::Left,
            QString::fromLatin1(ThemeAssetIds::AuditionReset));
    else
        refreshButton_->setDefaultAction(action);
    return refreshButton_;
}

void ListFooter::addStatusWidget(QWidget* widget)
{
    static_cast<QHBoxLayout*>(layout())->insertWidget(leftCount_++, widget, 1);
}

void ListFooter::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) applyTheme();
}

void ListFooter::applyTheme()
{
    if (!layout()) return;
    refreshIcons();
    const auto& metrics = themeManager_->metrics().table;
    layout()->setContentsMargins(metrics.cellPaddingX, metrics.cellPaddingY,
                                 metrics.cellPaddingX, metrics.cellPaddingY);
    layout()->setSpacing(metrics.buttonSpacing);
    int height = metrics.footerHeight;
    for (auto* button : findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
        button->ensurePolished();
        const bool compact = button->property("tableCompactAction").toBool();
        if (compact) button->setIconSize(QSize(themeManager_->metrics().objectIconExtent,
                                               themeManager_->metrics().objectIconExtent));
        const int buttonHeight = std::max(metrics.buttonHeight,
            compact ? button->iconSize().height()
                    : button->fontMetrics().height() + metrics.cellPaddingY * 2);
        button->setFixedHeight(buttonHeight);
        if (compact)
            button->setFixedWidth(std::max(metrics.buttonWidth, button->iconSize().width()));
        height = std::max(height, buttonHeight + metrics.cellPaddingY * 2);
    }
    setFixedHeight(height);
}

void ListFooter::refreshIcons()
{
    if (addButton_)
        addButton_->setIcon(themeManager_->resolveIcon(
            QString::fromLatin1(ThemeAssetIds::TableAdd)).icon);
    if (removeButton_)
        removeButton_->setIcon(themeManager_->resolveIcon(
            QString::fromLatin1(ThemeAssetIds::TableRemove)).icon);
    for (auto* button : findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
        const auto asset = button->property("tableIconAsset").toString();
        if (!asset.isEmpty()) button->setIcon(themeManager_->resolveIcon(asset).icon);
    }
}

}
