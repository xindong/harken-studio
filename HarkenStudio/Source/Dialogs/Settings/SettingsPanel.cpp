
#include "Dialogs/Settings/SettingsPanel.h"

#include "Core/ContextInfo.h"
#include "Widgets/Common/SearchField.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
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

    auto* searchContainer = new QWidget(this);
    searchContainer->setObjectName(QStringLiteral("settingsSearchBar"));
    searchContainer->setAttribute(Qt::WA_StyledBackground, true);
    auto* searchLayout = new QHBoxLayout(searchContainer);
    searchLayout->setContentsMargins(8, 8, 8, 9);
    searchField_ = new SearchField(searchContainer);
    searchField_->setObjectName(QStringLiteral("settingsSearchField"));
    searchField_->setPlaceholderText(tr("Search settings..."));
    searchField_->setAccessibleName(tr("Search settings"));
    connect(searchField_, &QLineEdit::textChanged, this, &SettingsPanel::applySearchFilter);
    searchLayout->addWidget(searchField_);
    mainLayout->addWidget(searchContainer);

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

    emptyState_ = new QLabel(tr("No matching settings."), this);
    emptyState_->setObjectName(QStringLiteral("settingsSearchEmptyState"));
    emptyState_->setProperty("textRole", QStringLiteral("muted"));
    emptyState_->setAlignment(Qt::AlignCenter);
    emptyState_->setWordWrap(true);
    emptyState_->hide();
    contentLayout->addWidget(emptyState_, 1);

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
            searchField_->clear();
            categoryList_->setCurrentRow(i);
            return true;
        }
    }
    return false;
}

void SettingsPanel::onCategoryChanged(int index)
{
    if (index >= 0 && index < pages_.size())
    {
        contentStack_->setCurrentIndex(index);
        ContextInfo::tag(categoryList_, ContextTopic::forPanel(
            QStringLiteral("settings.") + pages_.at(index)->pageId()));
    }
    else
        ContextInfo::tag(categoryList_, ContextTopic::forPanel(QStringLiteral("settings")));
}

void SettingsPanel::applySearchFilter(const QString& text)
{
    int current = contentStack_->currentIndex();
    int firstMatch = -1;
    {
        const QSignalBlocker blocker(categoryList_);
        for (int i = 0; i < pages_.size(); ++i)
        {
            const bool matches = pages_.at(i)->applySearchFilter(text);
            categoryList_->item(i)->setHidden(!matches);
            if (matches && firstMatch < 0)
                firstMatch = i;
        }
        if (current < 0 || categoryList_->item(current)->isHidden())
            current = firstMatch;
        categoryList_->setCurrentRow(current);
    }
    onCategoryChanged(current);
    contentStack_->setVisible(current >= 0);
    emptyState_->setVisible(current < 0);
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
