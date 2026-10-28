
#include "Dialogs/Settings/SettingsPanel.h"

#include "Core/ContextInfo.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace VNX {

SettingsPanel::SettingsPanel(QWidget* parent)
    : QWidget(parent)
{
    ContextInfo::tag(this, ContextTopic::forPanel(QStringLiteral("settings")));
    setMinimumSize(420, 280);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    categoryList_ = new QListWidget();
    categoryList_->setObjectName(QStringLiteral("settingsCategoryList"));
    categoryList_->setFixedWidth(150);
    connect(categoryList_, &QListWidget::currentRowChanged,
            this, &SettingsPanel::onCategoryChanged);
    contentLayout->addWidget(categoryList_);

    contentStack_ = new QStackedWidget();
    contentStack_->setObjectName(QStringLiteral("settingsContentStack"));
    contentLayout->addWidget(contentStack_, 1);

    mainLayout->addLayout(contentLayout, 1);
}

void SettingsPanel::addPage(SettingsPage* page)
{
    page->setObjectName(QStringLiteral("settingsPage_%1").arg(page->pageId()));
    pages_.append(page);
    contentStack_->addWidget(page);
    categoryList_->addItem(page->categoryLabel());
}

void SettingsPanel::loadPages()
{
    for (SettingsPage* page : pages_)
        page->load();
    if (categoryList_->count() > 0)
        categoryList_->setCurrentRow(0);
}

bool SettingsPanel::showPage(const QString& pageId)
{
    for (int i = 0; i < pages_.size(); ++i)
    {
        if (pages_.at(i)->pageId() == pageId)
        {
            categoryList_->setCurrentRow(i);
            return true;
        }
    }
    return false;
}

void SettingsPanel::onCategoryChanged(int index)
{
    contentStack_->setCurrentIndex(index);
    if (index >= 0 && index < pages_.size())
        ContextInfo::tag(categoryList_, ContextTopic::forPanel(
            QStringLiteral("settings.") + pages_.at(index)->pageId()));
}

QString SettingsPanel::currentPageId() const
{
    const int index = contentStack_->currentIndex();
    return index >= 0 && index < pages_.size() ? pages_.at(index)->pageId() : QString();
}

QSize SettingsPanel::sizeHint() const
{
    return QSize(800, 550);
}

}
