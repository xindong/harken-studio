
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
    shortcutTree_->setUniformRowHeights(true);
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
            stageShortcut(id, QKeySequence());
        else if (chosen == resetAction)
            stageShortcut(id, ShortcutManager::instance().info(id).defaultShortcut);
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
    pendingShortcuts_.clear();
    populateShortcutTree();
}

bool ShortcutsPage::apply()
{
    cancelShortcutCapture();
    revertShortcuts_.clear();
    appliedShortcuts_.clear();
    if (pendingShortcuts_.isEmpty())
        return true;

    ShortcutManager& manager = ShortcutManager::instance();
    for (auto it = pendingShortcuts_.cbegin(); it != pendingShortcuts_.cend(); ++it)
    {
        revertShortcuts_.insert(it.key(), manager.shortcut(it.key()));
        manager.setShortcut(it.key(), it.value());
        appliedShortcuts_.insert(it.key(), it.value());
    }
    manager.saveShortcuts();
    pendingShortcuts_.clear();

    for (QTreeWidgetItemIterator it(shortcutTree_); *it; ++it)
        refreshShortcutRow(*it);

    emit dirtyChanged();
    return true;
}

void ShortcutsPage::revert()
{
    if (revertShortcuts_.isEmpty())
        return;

    ShortcutManager& manager = ShortcutManager::instance();
    pendingShortcuts_ = appliedShortcuts_;
    for (auto it = revertShortcuts_.cbegin(); it != revertShortcuts_.cend(); ++it)
        manager.setShortcut(it.key(), it.value());
    manager.saveShortcuts();
    revertShortcuts_.clear();
    appliedShortcuts_.clear();

    for (QTreeWidgetItemIterator it(shortcutTree_); *it; ++it)
        refreshShortcutRow(*it);

    emit dirtyChanged();
}

bool ShortcutsPage::isDirty() const
{
    ShortcutManager& manager = ShortcutManager::instance();
    for (auto it = pendingShortcuts_.cbegin(); it != pendingShortcuts_.cend(); ++it)
    {
        if (it.value() != manager.shortcut(it.key()))
            return true;
    }
    return false;
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

        QTreeWidgetItem* catItem = new QTreeWidgetItem({ShortcutManager::categoryName(cat)});
        catItem->setFlags(Qt::ItemIsEnabled);
        catItem->setData(0, TableGroupRole, true);
        shortcutTree_->addTopLevelItem(catItem);

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
    if (id.isEmpty())
        return;

    const ShortcutInfo info = ShortcutManager::instance().info(id);
    const QKeySequence effective = effectiveShortcut(id);

    item->setText(1, effective.toString(QKeySequence::NativeText));

    const bool staged = pendingShortcuts_.contains(id)
                     && pendingShortcuts_.value(id) != info.currentShortcut;
    const bool custom = (effective != info.defaultShortcut);

    item->setData(1, TableTextRole, static_cast<int>(staged ? TableTextState::Modified
        : custom ? TableTextState::Custom : TableTextState::Normal));
}

QKeySequence ShortcutsPage::effectiveShortcut(const QString& id) const
{
    if (pendingShortcuts_.contains(id))
        return pendingShortcuts_.value(id);
    return ShortcutManager::instance().shortcut(id);
}

QString ShortcutsPage::effectiveConflict(const QString& id, const QKeySequence& sequence) const
{
    if (sequence.isEmpty())
        return QString();

    const QList<ShortcutInfo> shortcuts = ShortcutManager::instance().allShortcuts();
    for (const ShortcutInfo& info : shortcuts)
    {
        if (info.id != id && effectiveShortcut(info.id) == sequence)
            return info.id;
    }
    return QString();
}

void ShortcutsPage::beginShortcutCapture(QTreeWidgetItem* item)
{
    cancelShortcutCapture();

    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty())
        return;

    shortcutCaptureItem_ = item;
    shortcutCaptureEdit_ = new QKeySequenceEdit();
    shortcutCaptureEdit_->setKeySequence(effectiveShortcut(id));
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
    const QKeySequence sequence = shortcutCaptureEdit_->keySequence();
    shortcutCaptureItem_ = nullptr;
    shortcutCaptureEdit_ = nullptr;
    shortcutTree_->removeItemWidget(item, 1);

    const QString id = item->data(0, Qt::UserRole).toString();

    const QString conflictId = effectiveConflict(id, sequence);
    if (!conflictId.isEmpty())
    {
        const ShortcutInfo conflictInfo = ShortcutManager::instance().info(conflictId);
        const QMessageBox::StandardButton reply = QMessageBox::question(
            this, tr("Shortcut Conflict"),
            tr("\"%1\" is already used by \"%2\".\n\n"
               "Reassign it? \"%2\" will lose its shortcut.")
                .arg(sequence.toString(QKeySequence::NativeText), conflictInfo.displayName),
            QMessageBox::Yes | QMessageBox::No);

        if (reply != QMessageBox::Yes)
        {
            refreshShortcutRow(item);
            return;
        }
        stageShortcut(conflictId, QKeySequence());
    }

    stageShortcut(id, sequence);
}

void ShortcutsPage::stageShortcut(const QString& id, const QKeySequence& sequence)
{
    if (sequence == ShortcutManager::instance().shortcut(id))
        pendingShortcuts_.remove(id);
    else
        pendingShortcuts_.insert(id, sequence);

    for (QTreeWidgetItemIterator it(shortcutTree_); *it; ++it)
    {
        if ((*it)->data(0, Qt::UserRole).toString() == id)
        {
            refreshShortcutRow(*it);
            break;
        }
    }
    emit dirtyChanged();
}

void ShortcutsPage::resetShortcutsToDefaults()
{
    cancelShortcutCapture();
    pendingShortcuts_.clear();

    const QList<ShortcutInfo> shortcuts = ShortcutManager::instance().allShortcuts();
    for (const ShortcutInfo& info : shortcuts)
    {
        if (info.currentShortcut != info.defaultShortcut)
            pendingShortcuts_.insert(info.id, info.defaultShortcut);
    }

    for (QTreeWidgetItemIterator it(shortcutTree_); *it; ++it)
        refreshShortcutRow(*it);
    emit dirtyChanged();
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

}
