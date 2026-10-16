
#include "Dialogs/Settings/Pages/LayoutPage.h"

#include "Core/NamedLayoutStore.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableItemDelegate.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>

#include <utility>

namespace VNX {

LayoutPage::LayoutPage(QWidget* parent, SaveCurrentLayout saveCurrentLayout)
    : SettingsPage(parent)
    , saveCurrentLayout_(std::move(saveCurrentLayout))
{
    auto* body = makePageBody();

    auto* table = new TableWidget(TableOptions::display(), this);
    layoutTree_ = table->createItemView(QStringLiteral("layoutTree"),
        {tr("Name"), tr("Saved At")}, {380, 170});
    layoutTree_->setRootIsDecorated(false);
    layoutTree_->setUniformRowHeights(true);
    layoutTree_->setAllColumnsShowFocus(true);
    connect(layoutTree_, &QTreeWidget::currentItemChanged,
            this, [this]() { updateLayoutButtons(); });
    body->addWidget(table, 1);

    layoutImmediateStatusLabel_ = new QLabel();
    layoutImmediateStatusLabel_->setObjectName(QStringLiteral("layoutImmediateStatusLabel"));
    layoutImmediateStatusLabel_->setWordWrap(true);
    QtControlSkin::setSemanticProperty(layoutImmediateStatusLabel_, "settingsRole",
                                       QStringLiteral("statusSuccess"));
    body->addWidget(layoutImmediateStatusLabel_);

    auto* btnRow = new QHBoxLayout();
    auto* saveButton = new QPushButton(tr("Save Current Layout..."));
    saveButton->setObjectName(QStringLiteral("layoutSaveButton"));
    saveButton->setEnabled(bool(saveCurrentLayout_));
    connect(saveButton, &QPushButton::clicked, this, &LayoutPage::onLayoutSaveClicked);
    btnRow->addWidget(saveButton);

    layoutRenameButton_ = new QPushButton(tr("Rename..."));
    layoutRenameButton_->setObjectName(QStringLiteral("layoutRenameButton"));
    connect(layoutRenameButton_, &QPushButton::clicked,
            this, &LayoutPage::onLayoutRenameClicked);
    btnRow->addWidget(layoutRenameButton_);

    layoutDeleteButton_ = new QPushButton(tr("Delete"));
    layoutDeleteButton_->setObjectName(QStringLiteral("layoutDeleteButton"));
    connect(layoutDeleteButton_, &QPushButton::clicked,
            this, &LayoutPage::onLayoutDeleteClicked);
    btnRow->addWidget(layoutDeleteButton_);

    btnRow->addStretch();
    body->addLayout(btnRow);
}

QString LayoutPage::pageId() const
{
    return QString::fromLatin1(kSettingsPageLayout);
}

QString LayoutPage::categoryLabel() const
{
    return tr("Layout");
}

void LayoutPage::load()
{
    static_cast<TableWidget*>(layoutTree_->parentWidget())->clearSelection();
    populateLayoutTree();
}

bool LayoutPage::apply()
{
    return true;
}

bool LayoutPage::isDirty() const
{
    return false;
}

void LayoutPage::populateLayoutTree()
{
    if (!layoutTree_)
        return;
    layoutTree_->clear();

    QTreeWidgetItem* defaultItem = new QTreeWidgetItem({tr("Default"), tr("Built-in")});
    defaultItem->setData(1, TableTextRole, static_cast<int>(TableTextState::Muted));
    layoutTree_->addTopLevelItem(defaultItem);

    const QList<NamedLayoutInfo> layouts = NamedLayoutStore::list();
    for (const NamedLayoutInfo& info : layouts)
    {
        QTreeWidgetItem* item = new QTreeWidgetItem({info.name, info.savedAt});
        item->setData(0, Qt::UserRole, info.name);
        layoutTree_->addTopLevelItem(item);
    }

    updateLayoutButtons();
}

void LayoutPage::updateLayoutButtons()
{
    QTreeWidgetItem* item = layoutTree_ ? layoutTree_->currentItem() : nullptr;
    const bool userLayout = item && !item->data(0, Qt::UserRole).toString().isEmpty();
    if (layoutRenameButton_)
        layoutRenameButton_->setEnabled(userLayout);
    if (layoutDeleteButton_)
        layoutDeleteButton_->setEnabled(userLayout);
}

void LayoutPage::onLayoutSaveClicked()
{
    const QString name = saveCurrentLayout_(this);
    if (name.isEmpty())
        return;
    populateLayoutTree();
    for (int row = 0; row < layoutTree_->topLevelItemCount(); ++row) {
        auto* item = layoutTree_->topLevelItem(row);
        if (item->data(0, Qt::UserRole).toString() == name) {
            layoutTree_->setCurrentItem(item);
            layoutTree_->scrollToItem(item);
            break;
        }
    }
    layoutImmediateStatusLabel_->setText(tr("Saved."));
}

void LayoutPage::onLayoutRenameClicked()
{
    QTreeWidgetItem* item = layoutTree_ ? layoutTree_->currentItem() : nullptr;
    const QString oldName = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (oldName.isEmpty())
        return;

    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Rename Layout"),
                                                  tr("Layout name:"), QLineEdit::Normal,
                                                  oldName, &ok).trimmed();
    if (!ok || newName.isEmpty() || newName == oldName)
        return;

    if (!NamedLayoutStore::rename(oldName, newName))
    {
        QMessageBox::warning(this, tr("Rename Layout"),
                             tr("Could not rename '%1' to '%2'. The name may be "
                                "reserved or already in use.").arg(oldName, newName));
        return;
    }
    emit layoutRenamed(oldName, newName);
    populateLayoutTree();
    layoutImmediateStatusLabel_->setText(tr("Saved."));
}

void LayoutPage::onLayoutDeleteClicked()
{
    QTreeWidgetItem* item = layoutTree_ ? layoutTree_->currentItem() : nullptr;
    const QString name = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (name.isEmpty())
        return;

    const QMessageBox::StandardButton reply = QMessageBox::question(
        this, tr("Delete Layout"),
        tr("Delete layout '%1'? This cannot be undone.").arg(name),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    static_cast<TableWidget*>(layoutTree_->parentWidget())->removeSelectedRow([this, name] {
        if (!NamedLayoutStore::remove(name))
        {
            QMessageBox::warning(this, tr("Delete Layout"),
                                 tr("Layout '%1' could not be deleted.").arg(name));
            return false;
        }
        emit layoutDeleted(name);
        populateLayoutTree();
        layoutImmediateStatusLabel_->setText(tr("Saved."));
        return true;
    });
}

}
