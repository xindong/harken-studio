
#include "Dialogs/Settings/Pages/ShortcutsPage.h"

#include "Core/ShortcutManager.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableItemDelegate.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeySequenceEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace VNX {

ShortcutsPage::ShortcutsPage(QWidget* parent)
    : SettingsPage(parent)
{
    auto* body = makePageBody();

    shortcutSearchEdit_ = new SearchField();
    shortcutSearchEdit_->setObjectName(QStringLiteral("shortcutSearchField"));
    shortcutSearchEdit_->setPlaceholderText(tr("Search shortcuts..."));
    connect(shortcutSearchEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        cancelShortcutCapture();
        filterShortcutTree(text);
    });
    body->addWidget(shortcutSearchEdit_);

    auto* table = new TableWidget(TableOptions::display(), this);
    shortcutTree_ = table->createItemView(QStringLiteral("shortcutTree"),
        {tr("Command"), tr("Shortcut"), tr("Default")}, {380, 170, 150});
    shortcutTree_->setAllColumnsShowFocus(true);
    shortcutTree_->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(shortcutTree_, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem* item, int) {
        beginShortcutCapture(item);
    });

    connect(shortcutTree_, &QTreeWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = shortcutTree_->itemAt(pos);
        if (!item)
            return;
        const QString id = item->data(0, Qt::UserRole).toString();
        if (id.isEmpty())
            return;

        QMenu menu(this);
        QAction* rebindAction = menu.addAction(tr("Rebind..."));
        QAction* clearAction = menu.addAction(tr("Clear Shortcut"));
        QAction* resetAction = menu.addAction(tr("Reset to Default"));
        QAction* chosen = menu.exec(shortcutTree_->viewport()->mapToGlobal(pos));

        if (chosen == rebindAction)
            beginShortcutCapture(item);
        else if (chosen == clearAction)
            commitShortcut(id, QKeySequence());
        else if (chosen == resetAction)
            commitShortcut(id, ShortcutManager::instance().info(id).defaultShortcut);
    });

    body->addWidget(table, 1);

    auto* btnRow = new QHBoxLayout();
    auto* resetBtn = new QPushButton(tr("Reset to Defaults"));
    resetBtn->setObjectName(QStringLiteral("shortcutResetButton"));
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        resetShortcutsToDefaults();
    });
    btnRow->addWidget(resetBtn);
    btnRow->addStretch();
    body->addLayout(btnRow);

    auto& manager = ShortcutManager::instance();
    connect(&manager, &ShortcutManager::shortcutChanged, this, &ShortcutsPage::refreshShortcutRows);
    connect(&manager, &ShortcutManager::shortcutsReset, this, &ShortcutsPage::refreshShortcutRows);
}

QString ShortcutsPage::pageId() const
{
    return QString::fromLatin1(kSettingsPageShortcuts);
}

QString ShortcutsPage::categoryLabel() const
{
    return tr("Shortcuts");
}

void ShortcutsPage::load()
{
    populateShortcutTree();
}

void ShortcutsPage::populateShortcutTree()
{
    if (!shortcutTree_)
        return;

    cancelShortcutCapture();
    shortcutTree_->clear();

    ShortcutManager& manager = ShortcutManager::instance();

    for (int i = 0; i <= static_cast<int>(ShortcutCategory::Custom); ++i)
    {
        ShortcutCategory cat = static_cast<ShortcutCategory>(i);
        const QList<ShortcutInfo> entries = manager.shortcutsInCategory(cat);
        if (entries.isEmpty())
            continue;

        auto* catItem = shortcutTree_->createGroup(ShortcutManager::categoryName(cat));

        for (const ShortcutInfo& info : entries)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem();
            item->setText(0, info.displayName);
            item->setToolTip(0, info.description);
            item->setText(2, info.defaultShortcut.toString(QKeySequence::NativeText));
            item->setData(0, Qt::UserRole, info.id);
            catItem->addChild(item);
            refreshShortcutRow(item);
        }
    }

    shortcutTree_->expandAll();
    filterShortcutTree(shortcutSearchEdit_->text());
}

void ShortcutsPage::filterShortcutTree(const QString& filter)
{
    for (int i = 0; i < shortcutTree_->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* category = shortcutTree_->topLevelItem(i);
        bool anyVisible = false;

        for (int j = 0; j < category->childCount(); ++j)
        {
            QTreeWidgetItem* item = category->child(j);
            const bool matches = filter.isEmpty()
                || item->text(0).contains(filter, Qt::CaseInsensitive)
                || item->text(1).contains(filter, Qt::CaseInsensitive);
            item->setHidden(!matches);
            anyVisible = anyVisible || matches;
        }

        category->setHidden(!anyVisible);
    }
}

void ShortcutsPage::refreshShortcutRow(QTreeWidgetItem* item)
{
    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty() || item == shortcutCaptureItem_)
        return;

    const ShortcutInfo info = ShortcutManager::instance().info(id);
    item->setText(1, info.currentShortcut.toString(QKeySequence::NativeText));
    item->setData(1, TableTextRole, static_cast<int>(info.isCustom
        ? TableTextState::Custom : TableTextState::Normal));
}

void ShortcutsPage::refreshShortcutRows()
{
    for (QTreeWidgetItemIterator it(shortcutTree_); *it; ++it)
        refreshShortcutRow(*it);
}

void ShortcutsPage::beginShortcutCapture(QTreeWidgetItem* item)
{
    cancelShortcutCapture();

    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty())
        return;

    shortcutCaptureItem_ = item;
    shortcutCaptureEdit_ = new QKeySequenceEdit();
    shortcutCaptureEdit_->setKeySequence(ShortcutManager::instance().shortcut(id));
    connect(shortcutCaptureEdit_, &QKeySequenceEdit::editingFinished,
            this, &ShortcutsPage::commitShortcutCapture);
    shortcutCaptureEdit_->installEventFilter(this);
    if (QWidget* proxy = shortcutCaptureEdit_->focusProxy())
        proxy->installEventFilter(this);

    shortcutTree_->setCurrentItem(item);
    shortcutTree_->setItemWidget(item, 1, shortcutCaptureEdit_);
    shortcutCaptureEdit_->setFocus();
}

void ShortcutsPage::cancelShortcutCapture()
{
    if (!shortcutCaptureItem_)
        return;

    QTreeWidgetItem* item = shortcutCaptureItem_;
    shortcutCaptureEdit_->disconnect(this);
    shortcutCaptureEdit_->removeEventFilter(this);
    if (QWidget* proxy = shortcutCaptureEdit_->focusProxy())
        proxy->removeEventFilter(this);
    shortcutCaptureItem_ = nullptr;
    shortcutCaptureEdit_ = nullptr;
    shortcutTree_->removeItemWidget(item, 1);
    refreshShortcutRow(item);
}

void ShortcutsPage::commitShortcutCapture()
{
    if (!shortcutCaptureItem_ || !shortcutCaptureEdit_)
        return;

    QTreeWidgetItem* item = shortcutCaptureItem_;
    const QString id = item->data(0, Qt::UserRole).toString();
    const QKeySequence sequence = shortcutCaptureEdit_->keySequence();
    cancelShortcutCapture();
    commitShortcut(id, sequence);
}

void ShortcutsPage::commitShortcut(const QString& id, const QKeySequence& sequence)
{
    ShortcutManager& manager = ShortcutManager::instance();
    if (sequence == manager.shortcut(id))
        return;

    QString conflictId = manager.conflictingShortcut(id, sequence);
    while (!conflictId.isEmpty())
    {
        const ShortcutInfo conflictInfo = manager.info(conflictId);
        const QMessageBox::StandardButton reply = QMessageBox::question(
            this, tr("Shortcut Conflict"),
            tr("\"%1\" is already used by \"%2\".\n\n"
               "Reassign it? \"%2\" will lose its shortcut.")
                .arg(sequence.toString(QKeySequence::NativeText), conflictInfo.displayName),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;
        if (manager.conflictingShortcut(id, sequence) == conflictId)
            break;
        conflictId = manager.conflictingShortcut(id, sequence);
    }

    if (!conflictId.isEmpty())
        manager.setShortcut(conflictId, QKeySequence());
    manager.setShortcut(id, sequence);
    manager.saveShortcuts();
}

void ShortcutsPage::resetShortcutsToDefaults()
{
    cancelShortcutCapture();
    const QMessageBox::StandardButton reply = QMessageBox::question(
        this, tr("Reset Shortcuts"),
        tr("Reset all shortcuts to their defaults? Custom shortcuts will be replaced."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (reply != QMessageBox::Yes)
        return;

    ShortcutManager& manager = ShortcutManager::instance();
    manager.resetAllToDefaults();
    manager.saveShortcuts();
}

bool ShortcutsPage::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusOut && shortcutCaptureEdit_
        && (watched == shortcutCaptureEdit_ || watched == shortcutCaptureEdit_->focusProxy()))
    {
        commitShortcutCapture();
    }
    return SettingsPage::eventFilter(watched, event);
}

void ShortcutsPage::hideEvent(QHideEvent* event)
{
    commitShortcutCapture();
    SettingsPage::hideEvent(event);
}

}
