#include "Core/ShortcutManager.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <gtest/gtest.h>

#include <utility>

namespace {

class ShortcutRestore
{
public:
    ShortcutRestore(VNX::ShortcutManager& manager, QString id)
        : manager_(manager), id_(std::move(id)), shortcut_(manager.shortcut(id_))
    {
    }

    ~ShortcutRestore()
    {
        manager_.setShortcut(id_, shortcut_);
    }

private:
    VNX::ShortcutManager& manager_;
    QString id_;
    QKeySequence shortcut_;
};

}

TEST(ShortcutManager, RegistersSupportedCommandsOnly)
{
    VNX::ShortcutManager& manager = VNX::ShortcutManager::instance();
    QSet<QString> ids;
    for (const VNX::ShortcutInfo& info : manager.allShortcuts())
        ids.insert(info.id);

    EXPECT_EQ(ids, QSet<QString>({
        VNX::Shortcuts::SaveProject,
        VNX::Shortcuts::Undo,
        VNX::Shortcuts::Redo,
        VNX::Shortcuts::Cut,
        VNX::Shortcuts::Copy,
        VNX::Shortcuts::Paste,
        VNX::Shortcuts::Delete,
        VNX::Shortcuts::SelectAll,
        VNX::Shortcuts::Duplicate,
        VNX::Shortcuts::PlayStop,
        VNX::Shortcuts::PlaybackReset,
        VNX::Shortcuts::AudioSettings,
        VNX::Shortcuts::Preferences
    }));
    EXPECT_FALSE(ids.contains(QStringLiteral("file.new")));
    EXPECT_FALSE(ids.contains(QStringLiteral("file.open")));
    EXPECT_FALSE(ids.contains(QStringLiteral("file.saveAs")));
    EXPECT_FALSE(ids.contains(QStringLiteral("file.quit")));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Preferences).defaultShortcut,
              QKeySequence(QStringLiteral("Ctrl+,")));
    EXPECT_EQ(manager.info(VNX::Shortcuts::SaveProject).defaultShortcut,
              QKeySequence(QKeySequence::Save));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Undo).defaultShortcut,
              QKeySequence(QKeySequence::Undo));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Redo).defaultShortcut,
              QKeySequence(QKeySequence::Redo));
    EXPECT_EQ(manager.info(VNX::Shortcuts::PlayStop).defaultShortcut,
              QKeySequence(QStringLiteral("Space")));
    EXPECT_EQ(manager.info(VNX::Shortcuts::PlayStop).category,
              VNX::ShortcutCategory::Playback);
    EXPECT_EQ(manager.info(VNX::Shortcuts::PlaybackReset).category,
              VNX::ShortcutCategory::Playback);
    EXPECT_EQ(manager.info(VNX::Shortcuts::Cut).defaultShortcut,
              QKeySequence(QKeySequence::Cut));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Copy).defaultShortcut,
              QKeySequence(QKeySequence::Copy));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Paste).defaultShortcut,
              QKeySequence(QKeySequence::Paste));
    EXPECT_EQ(manager.info(VNX::Shortcuts::Delete).defaultShortcut,
              QKeySequence(QKeySequence::Delete));
}

TEST(ShortcutManager, ImportMapsLegacyAuditionIds)
{
    VNX::ShortcutManager& manager = VNX::ShortcutManager::instance();
    ShortcutRestore playStop(manager, VNX::Shortcuts::PlayStop);
    ShortcutRestore reset(manager, VNX::Shortcuts::PlaybackReset);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("shortcuts.json"));

    QJsonObject root;
    root.insert(QStringLiteral("audition.playStop"),
                QJsonObject{{QStringLiteral("current"), QStringLiteral("F5")}});
    root.insert(QStringLiteral("audition.reset"),
                QJsonObject{{QStringLiteral("current"), QStringLiteral("F6")}});
    root.insert(QStringLiteral("removed.command"),
                QJsonObject{{QStringLiteral("current"), QStringLiteral("F7")}});

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write(QJsonDocument(root).toJson()), 0);
    file.close();

    ASSERT_TRUE(manager.importShortcuts(path));
    EXPECT_EQ(manager.shortcut(VNX::Shortcuts::PlayStop), QKeySequence(QStringLiteral("F5")));
    EXPECT_TRUE(manager.info(VNX::Shortcuts::PlayStop).isCustom);
    EXPECT_EQ(manager.shortcut(VNX::Shortcuts::PlaybackReset), QKeySequence(QStringLiteral("F6")));
    EXPECT_TRUE(manager.info(VNX::Shortcuts::PlaybackReset).isCustom);
}

TEST(ShortcutManager, RebindUpdatesAllWidgetConsumersAndDropsDestroyedActions)
{
    VNX::ShortcutManager& manager = VNX::ShortcutManager::instance();
    ShortcutRestore restore(manager, VNX::Shortcuts::SaveProject);
    const qsizetype initialActionCount =
        manager.info(VNX::Shortcuts::SaveProject).actions.size();
    const QKeySequence replacement(QStringLiteral("Ctrl+Alt+Shift+F12"));

    {
        QWidget firstWidget;
        QWidget secondWidget;
        int firstTriggerCount = 0;
        int secondTriggerCount = 0;
        QAction* firstAction = manager.bindWidgetAction(
            VNX::Shortcuts::SaveProject, &firstWidget, [&firstTriggerCount]() {
                ++firstTriggerCount;
            });
        QAction* secondAction = manager.bindWidgetAction(
            VNX::Shortcuts::SaveProject, &secondWidget, [&secondTriggerCount]() {
                ++secondTriggerCount;
            });

        ASSERT_NE(firstAction, nullptr);
        ASSERT_NE(secondAction, nullptr);
        EXPECT_EQ(manager.info(VNX::Shortcuts::SaveProject).actions.size(),
                  initialActionCount + 2);

        manager.setShortcut(VNX::Shortcuts::SaveProject, replacement);
        EXPECT_EQ(firstAction->shortcut(), replacement);
        EXPECT_EQ(secondAction->shortcut(), replacement);

        firstAction->trigger();
        secondAction->trigger();
        EXPECT_EQ(firstTriggerCount, 1);
        EXPECT_EQ(secondTriggerCount, 1);
    }

    EXPECT_EQ(manager.info(VNX::Shortcuts::SaveProject).actions.size(),
              initialActionCount);
}

TEST(ShortcutManager, MenuUsesFocusedScopeWithoutRegisteringAnotherShortcut)
{
    auto& manager = VNX::ShortcutManager::instance();
    const QString id = VNX::Shortcuts::Copy;
    ShortcutRestore restore(manager, id);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* first = new QWidget(&window);
    auto* child = new QWidget(first);
    auto* second = new QWidget(&window);
    child->setFocusPolicy(Qt::StrongFocus);
    second->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(first);
    layout->addWidget(second);
    first->setMinimumSize(100, 50);
    second->setMinimumSize(100, 50);
    child->resize(50, 20);
    int firstCount = 0;
    int secondCount = 0;
    QAction* firstAction = manager.bindWidgetAction(id, first, [&]() { ++firstCount; },
                                                  Qt::WidgetWithChildrenShortcut);
    manager.bindWidgetAction(id, second, [&]() { ++secondCount; });
    QMenu menu(&window);
    QAction* menuAction = manager.createMenuAction(id, &menu);
    window.show();
    window.activateWindow();
    child->setFocus();
    QApplication::processEvents();
    ASSERT_EQ(QApplication::focusWidget(), child);
    menu.popup(window.mapToGlobal(QPoint(0, 0)));
    ASSERT_TRUE(menuAction->isEnabled());
    menu.setActiveAction(menuAction);
    QKeyEvent activate(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(&menu, &activate);
    EXPECT_EQ(firstCount, 1);
    EXPECT_EQ(secondCount, 0);

    firstAction->setShortcutContext(Qt::WidgetShortcut);
    menu.aboutToShow();
    EXPECT_FALSE(menuAction->isEnabled());
    firstAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    firstAction->setEnabled(false);
    menu.aboutToShow();
    EXPECT_FALSE(menuAction->isEnabled());
    firstAction->setEnabled(true);

    second->setFocus();
    menu.aboutToShow();
    ASSERT_TRUE(menuAction->isEnabled());
    child->setFocus();
    menuAction->trigger();
    EXPECT_EQ(firstCount, 1);
    EXPECT_EQ(secondCount, 1);
    second->setFocus();

    const QKeySequence replacement(QStringLiteral("Ctrl+Alt+Shift+F12"));
    manager.setShortcut(id, replacement);
    EXPECT_TRUE(menuAction->text().endsWith(replacement.toString(QKeySequence::NativeText)));
    QKeyEvent press(QEvent::KeyPress, Qt::Key_F12,
                    Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    QApplication::sendEvent(second, &press);
    EXPECT_EQ(firstCount, 1);
    EXPECT_EQ(secondCount, 2);

    manager.setShortcut(id, {});
    EXPECT_FALSE(menuAction->text().contains(QLatin1Char('\t')));
    menu.aboutToShow();
    menuAction->trigger();
    EXPECT_EQ(secondCount, 3);
    manager.resetToDefault(id);
    EXPECT_TRUE(menuAction->text().endsWith(manager.shortcut(id).toString(QKeySequence::NativeText)));

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QFile imported(directory.filePath(QStringLiteral("shortcuts.json")));
    ASSERT_TRUE(imported.open(QIODevice::WriteOnly));
    imported.write(QJsonDocument(QJsonObject{
        {id, QJsonObject{{QStringLiteral("current"), replacement.toString()}}}
    }).toJson());
    imported.close();
    ASSERT_TRUE(manager.importShortcuts(imported.fileName()));
    EXPECT_TRUE(menuAction->text().endsWith(replacement.toString(QKeySequence::NativeText)));

    menu.aboutToShow();
    delete second;
    menuAction->trigger();
    EXPECT_EQ(secondCount, 3);
}

TEST(ShortcutManager, MenuTextEditingDoesNotEscapeToTheEnclosingWidget)
{
    auto& manager = VNX::ShortcutManager::instance();
    ShortcutRestore restore(manager, VNX::Shortcuts::Copy);
    manager.setShortcut(VNX::Shortcuts::Copy, QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F12")));
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* line = new QLineEdit(&window);
    layout->addWidget(line);
    int outerCount = 0;
    QMenu menu(&window);
    QMap<QString, QAction*> actions;
    for (const QString& id : {VNX::Shortcuts::Copy, VNX::Shortcuts::Cut,
                              VNX::Shortcuts::Paste, VNX::Shortcuts::Delete,
                              VNX::Shortcuts::Duplicate, VNX::Shortcuts::SelectAll}) {
        manager.bindWidgetAction(id, &window, [&]() { ++outerCount; },
                                 Qt::WidgetWithChildrenShortcut);
        actions[id] = manager.createMenuAction(id, &menu);
    }
    const QString previousClipboard = QApplication::clipboard()->text();
    auto restoreClipboard = qScopeGuard([&]() { QApplication::clipboard()->setText(previousClipboard); });
    window.show();
    window.activateWindow();
    line->setFocus();
    QApplication::processEvents();
    line->setText(QStringLiteral("editable text"));
    line->selectAll();
    menu.aboutToShow();
    EXPECT_FALSE(actions[VNX::Shortcuts::Duplicate]->isEnabled());
    ASSERT_TRUE(actions[VNX::Shortcuts::Copy]->isEnabled());
    EXPECT_TRUE(actions[VNX::Shortcuts::Copy]->text().endsWith(
        QKeySequence(QKeySequence::Copy).toString(QKeySequence::NativeText)));
    actions[VNX::Shortcuts::Copy]->trigger();
    EXPECT_EQ(QApplication::clipboard()->text(), line->text());
    actions[VNX::Shortcuts::Cut]->trigger();
    EXPECT_TRUE(line->text().isEmpty());
    menu.aboutToShow();
    ASSERT_TRUE(actions[VNX::Shortcuts::Paste]->isEnabled());
    actions[VNX::Shortcuts::Paste]->trigger();
    EXPECT_EQ(line->text(), QStringLiteral("editable text"));
    menu.aboutToShow();
    actions[VNX::Shortcuts::SelectAll]->trigger();
    line->setReadOnly(true);
    menu.aboutToShow();
    EXPECT_TRUE(actions[VNX::Shortcuts::Copy]->isEnabled());
    for (const QString& id : {VNX::Shortcuts::Cut, VNX::Shortcuts::Paste, VNX::Shortcuts::Delete})
        EXPECT_FALSE(actions[id]->isEnabled());
    line->setReadOnly(false);
    menu.aboutToShow();
    actions[VNX::Shortcuts::Delete]->trigger();
    EXPECT_TRUE(line->text().isEmpty());
    EXPECT_EQ(outerCount, 0);

    line->setText(QStringLiteral("secret"));
    line->setEchoMode(QLineEdit::Password);
    line->selectAll();
    menu.aboutToShow();
    EXPECT_FALSE(actions[VNX::Shortcuts::Copy]->isEnabled());
    EXPECT_FALSE(actions[VNX::Shortcuts::Cut]->isEnabled());
}

TEST(ShortcutManager, MenuSupportsNativeTextAndItemSelection)
{
    auto& manager = VNX::ShortcutManager::instance();
    ShortcutRestore restore(manager, VNX::Shortcuts::SelectAll);
    manager.setShortcut(VNX::Shortcuts::SelectAll, QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F12")));
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* plainText = new QPlainTextEdit(&window);
    auto* richText = new QTextEdit(&window);
    auto* tree = new QTreeWidget(&window);
    layout->addWidget(plainText);
    layout->addWidget(richText);
    layout->addWidget(tree);
    QMenu menu(&window);
    QAction* selectAll = manager.createMenuAction(VNX::Shortcuts::SelectAll, &menu);
    QAction* remove = manager.createMenuAction(VNX::Shortcuts::Delete, &menu);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
    const auto checkText = [&](auto* edit) {
        edit->setPlainText(QStringLiteral("first\nsecond"));
        edit->setFocus();
        menu.aboutToShow();
        ASSERT_TRUE(selectAll->isEnabled());
        EXPECT_TRUE(selectAll->text().endsWith(
            QKeySequence(QKeySequence::SelectAll).toString(QKeySequence::NativeText)));
        EXPECT_FALSE(remove->isEnabled());
        selectAll->trigger();
        menu.aboutToShow();
        ASSERT_TRUE(remove->isEnabled());
        remove->trigger();
        EXPECT_TRUE(edit->toPlainText().isEmpty());
        edit->undo();
        EXPECT_EQ(edit->toPlainText(), QStringLiteral("first\nsecond"));
    };
    checkText(plainText);
    checkText(richText);

    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->addTopLevelItem(new QTreeWidgetItem(QStringList{QStringLiteral("First")}));
    tree->addTopLevelItem(new QTreeWidgetItem(QStringList{QStringLiteral("Second")}));
    tree->setFocus();
    menu.aboutToShow();
    ASSERT_TRUE(selectAll->isEnabled());
    EXPECT_TRUE(selectAll->text().endsWith(
        QKeySequence(QKeySequence::SelectAll).toString(QKeySequence::NativeText)));
    EXPECT_FALSE(remove->isEnabled());
    selectAll->trigger();
    EXPECT_EQ(tree->selectedItems().size(), 2);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    menu.aboutToShow();
    EXPECT_FALSE(selectAll->isEnabled());
}
