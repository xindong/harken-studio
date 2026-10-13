
#include "Dialogs/Settings/SettingsShellDialog.h"

#include "Core/QtControlSkin.h"
#include "Core/ContextInfo.h"

#include <QAbstractButton>
#include <QFrame>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>

namespace VNX {

SettingsShellDialog::SettingsShellDialog(QWidget* parent)
    : BaseDialog(parent)
{
    ContextInfo::tag(this, ContextTopic::forPanel(QStringLiteral("settings")));
    setMinimumSize(700, 500);
    resize(800, 550);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 12);
    mainLayout->setSpacing(0);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    categoryList_ = new QListWidget();
    categoryList_->setObjectName(QStringLiteral("settingsCategoryList"));
    categoryList_->setFixedWidth(150);
    connect(categoryList_, &QListWidget::currentRowChanged,
            this, &SettingsShellDialog::onCategoryChanged);
    contentLayout->addWidget(categoryList_);

    contentStack_ = new QStackedWidget();
    contentStack_->setObjectName(QStringLiteral("settingsContentStack"));
    contentLayout->addWidget(contentStack_, 1);

    mainLayout->addLayout(contentLayout, 1);

    auto* separator = new QFrame();
    separator->setObjectName(QStringLiteral("settingsSeparator"));
    separator->setFrameShape(QFrame::HLine);
    separator->setFixedHeight(1);
    mainLayout->addWidget(separator);

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(16, 12, 16, 0);
    buttonLayout->setSpacing(12);
    buttonLayout->addStretch();

    applyButton_ = new QPushButton(tr("Apply"));
    applyButton_->setObjectName(QStringLiteral("settingsApplyButton"));
    ContextInfo::tag(applyButton_, ContextTopic::forPanel(QStringLiteral("settings.apply")));
    QtControlSkin::setSemanticProperty(applyButton_, "settingsRole",
                                       QStringLiteral("primary"));
    connect(applyButton_, &QPushButton::clicked,
            this, &SettingsShellDialog::onApplyClicked);
    buttonLayout->addWidget(applyButton_);

    cancelButton_ = new QPushButton(tr("Cancel"));
    cancelButton_->setObjectName(QStringLiteral("settingsCancelButton"));
    ContextInfo::tag(cancelButton_, ContextTopic::forPanel(QStringLiteral("settings.cancel")));
    cancelButton_->setAutoDefault(false);
    QtControlSkin::setSemanticProperty(cancelButton_, "settingsRole",
                                       QStringLiteral("secondary"));
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelButton_);

    mainLayout->addLayout(buttonLayout);

    applyButton_->ensurePolished();
    cancelButton_->ensurePolished();
    const int width = qMax(applyButton_->sizeHint().width(),
                           cancelButton_->sizeHint().width());
    applyButton_->setFixedWidth(width);
    cancelButton_->setFixedWidth(width);
}

void SettingsShellDialog::addPage(SettingsPage* page)
{
    page->setObjectName(QStringLiteral("settingsPage_%1").arg(page->pageId()));
    pages_.append(page);
    contentStack_->addWidget(page);
    categoryList_->addItem(page->categoryLabel());
    connect(page, &SettingsPage::dirtyChanged,
            this, &SettingsShellDialog::updateApplyEnabled);
}

void SettingsShellDialog::loadPages()
{
    for (SettingsPage* page : pages_)
        page->load();
    if (categoryList_->count() > 0)
        categoryList_->setCurrentRow(0);
    updateApplyEnabled();
}

bool SettingsShellDialog::showPage(const QString& pageId)
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

void SettingsShellDialog::onCategoryChanged(int index)
{
    contentStack_->setCurrentIndex(index);
    if (index >= 0 && index < pages_.size())
        ContextInfo::tag(categoryList_, ContextTopic::forPanel(
            QStringLiteral("settings.") + pages_.at(index)->pageId()));
}

bool SettingsShellDialog::anyDirty() const
{
    for (const SettingsPage* page : pages_)
    {
        if (page->isDirty())
            return true;
    }
    return false;
}

void SettingsShellDialog::updateApplyEnabled()
{
    const bool dirty = anyDirty();
    applyButton_->setEnabled(dirty);
    applyButton_->setDefault(dirty);
}

void SettingsShellDialog::onApplyClicked()
{
    QList<SettingsPage*> committed;
    for (SettingsPage* page : pages_)
    {
        if (!page->apply())
        {
            for (int i = committed.size() - 1; i >= 0; --i)
                committed.at(i)->revert();
            showPage(page->pageId());
            updateApplyEnabled();
            return;
        }
        committed.append(page);
    }
    accept();
}

bool SettingsShellDialog::confirmDiscard()
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Unsaved Changes"));
    box.setText(tr("Discard the changes you have not applied?"));
    QPushButton* discard = box.addButton(tr("Discard Changes"),
                                         QMessageBox::AcceptRole);
    QPushButton* keep = box.addButton(tr("Keep Editing"),
                                      QMessageBox::RejectRole);
    box.setDefaultButton(keep);
    box.exec();
    return box.clickedButton() == discard;
}

void SettingsShellDialog::reject()
{
    if (anyDirty() && !confirmDiscard())
        return;
    BaseDialog::reject();
}

}
