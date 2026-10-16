#include "Core/AppSettings.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/QtControlSkin.h"
#include "Core/ShortcutManager.h"
#include "Core/ThemeManager.h"
#include "Dialogs/Settings/Pages/AIAgentPage.h"
#include "Dialogs/Settings/Pages/ShortcutsPage.h"
#include "Dialogs/Settings/PreferencesDialog.h"
#include "Dialogs/Settings/SettingsShellDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QKeySequenceEdit>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <gtest/gtest.h>

#include <memory>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void acceptNextModalDialog()
{
    QTimer::singleShot(0, [] {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal)
        {
            acceptNextModalDialog();
            return;
        }
        if (auto* dialog = qobject_cast<QDialog*>(modal))
            dialog->accept();
    });
}

VNX::PreferencesDialog::SecretStoreAccess inertSecretStore()
{
    VNX::PreferencesDialog::SecretStoreAccess access;
    access.load = [](const QString&, const QString&) { return QString(); };
    access.store = [](const QString&, const QString&, const QString&) { return true; };
    access.erase = [](const QString&, const QString&) { return true; };
    return access;
}

class ProbePreferencesDialog : public VNX::PreferencesDialog
{
public:
    ProbePreferencesDialog(VNX::ThemeManager& themeManager)
        : VNX::PreferencesDialog(inertSecretStore(), themeManager, nullptr)
    {
    }
};

QPushButton* applyButton(VNX::PreferencesDialog& dialog)
{
    return dialog.findChild<QPushButton*>(QStringLiteral("settingsApplyButton"));
}

QCheckBox* developerCheck(VNX::PreferencesDialog& dialog)
{
    return dialog.findChild<QCheckBox*>(QStringLiteral("developerModeCheck"));
}

}

namespace VNX {

class SettingsApplyContractTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        AppSettings::instance().setStorage(std::make_unique<MemorySettingsStorage>());
    }

    void TearDown() override
    {
        AppSettings::instance().setStorage(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"),
            QStringLiteral("HarkenStudio")));
    }
};

TEST_F(SettingsApplyContractTest, ApplyDisabledWhenClean)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    ProbePreferencesDialog dialog(manager);
    auto* apply = applyButton(dialog);
    ASSERT_NE(apply, nullptr);
    EXPECT_FALSE(apply->isEnabled());
}

TEST_F(SettingsApplyContractTest, ShortcutsCaptureSurvivesResizeAndSearchCancelsWithoutChanges)
{
    const QString previousStyle = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(previousStyle); });
    const auto skin = QtControlSkin::current();
    qApp->setStyleSheet(skin.applicationStyleSheet());
    ShortcutsPage page;
    page.load();
    page.resize(720, 480);
    page.show();
    QApplication::processEvents();
    auto* tree = page.findChild<QTreeWidget*>(QStringLiteral("shortcutTree"));
    ASSERT_NE(tree, nullptr);
    QTreeWidgetItem* item = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toString() == Shortcuts::SaveProject) {
            item = *it;
            break;
        }
    ASSERT_NE(item, nullptr);
    tree->scrollToItem(item);
    QApplication::processEvents();
    const auto openCapture = [&] {
        const QPoint point = tree->visualRect(tree->indexFromItem(item, 1)).center();
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                                QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, point, tree->viewport()->mapToGlobal(point),
                Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &event);
        }
        QApplication::processEvents();
        QApplication::processEvents();
    };
    openCapture();
    auto* editor = qobject_cast<QKeySequenceEdit*>(tree->itemWidget(item, 1));
    ASSERT_NE(editor, nullptr);
    auto* line = editor->findChild<QLineEdit*>();
    ASSERT_NE(line, nullptr);
    for (int width : {720, 960}) {
        page.resize(width, 480);
        QApplication::processEvents();
        QApplication::processEvents();
        const QRect cellRect = tree->visualRect(tree->indexFromItem(item, 1));
        const QRect editorRect(editor->mapTo(tree->viewport(), QPoint()), editor->size());
        EXPECT_TRUE(cellRect.contains(editorRect));
        EXPECT_LT(editorRect.width(), cellRect.width());
        EXPECT_TRUE(editor->rect().contains(line->geometry()));
        EXPECT_EQ(line->height(), skin.fieldHeight());
        EXPECT_EQ(editor->keySequence(), ShortcutManager::instance().shortcut(Shortcuts::SaveProject));
    }
    editor->editingFinished();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    EXPECT_EQ(tree->itemWidget(item, 1), nullptr);
    EXPECT_FALSE(page.isDirty());
    openCapture();
    ASSERT_NE(tree->itemWidget(item, 1), nullptr);
    auto* search = page.findChild<QLineEdit*>(QStringLiteral("shortcutSearchField"));
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral("Save"));
    EXPECT_EQ(tree->itemWidget(item, 1), nullptr);
    EXPECT_FALSE(page.isDirty());
}

TEST_F(SettingsApplyContractTest, DirtyFieldEnablesApply)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    ProbePreferencesDialog dialog(manager);
    auto* apply = applyButton(dialog);
    auto* check = developerCheck(dialog);
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(check, nullptr);

    check->setChecked(!check->isChecked());
    EXPECT_TRUE(apply->isEnabled());
}

TEST_F(SettingsApplyContractTest, SuccessfulApplyPersistsAndClosesDialog)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    ProbePreferencesDialog dialog(manager);
    dialog.show();
    auto* apply = applyButton(dialog);
    auto* check = developerCheck(dialog);
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(check, nullptr);

    const bool nextValue = !check->isChecked();
    check->setChecked(nextValue);
    apply->click();
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_EQ(VNX::AppSettings::instance().developerMode(), nextValue);
}

TEST_F(SettingsApplyContractTest, DismissalDiscardsDraftWithoutConfirmation)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    const bool stored = VNX::AppSettings::instance().developerMode();
    for (int dismissal = 0; dismissal < 3; ++dismissal) {
        SCOPED_TRACE(dismissal);
        ProbePreferencesDialog dialog(manager);
        dialog.show();
        auto* check = developerCheck(dialog);
        auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("settingsCancelButton"));
        ASSERT_NE(check, nullptr);
        ASSERT_NE(cancel, nullptr);
        EXPECT_EQ(check->isChecked(), stored);
        check->setChecked(!stored);

        bool prompted = false;
        QTimer::singleShot(0, &dialog, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                prompted = true;
                box->reject();
            }
        });
        if (dismissal == 0)
            cancel->click();
        else if (dismissal == 1) {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&dialog, &escape);
        } else
            dialog.close();
        QApplication::processEvents();
        EXPECT_FALSE(prompted);
        EXPECT_FALSE(dialog.isVisible());
        EXPECT_EQ(dialog.result(), QDialog::Rejected);
        EXPECT_EQ(VNX::AppSettings::instance().developerMode(), stored);
    }
}

TEST_F(SettingsApplyContractTest, CleanRejectClosesDialog)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    ProbePreferencesDialog dialog(manager);
    dialog.show();
    dialog.reject();
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(dialog.result(), QDialog::Rejected);
}

TEST_F(SettingsApplyContractTest, FailedApplyJumpsToFailingPage)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PreferencesDialog::SecretStoreAccess failingStore;
    failingStore.load = [](const QString&, const QString&) { return QString(); };
    failingStore.store = [](const QString&, const QString&, const QString&) {
        return false;
    };
    failingStore.erase = [](const QString&, const QString&) { return true; };

    VNX::PreferencesDialog dialog(failingStore, manager);
    dialog.show();
    auto* apply = applyButton(dialog);
    auto* keyEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* stack = dialog.findChild<QStackedWidget*>(QStringLiteral("settingsContentStack"));
    auto* aiPage = dialog.findChild<QWidget*>(
        QStringLiteral("settingsPage_%1").arg(QLatin1String(VNX::kSettingsPageAIAgent)));
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(keyEdit, nullptr);
    ASSERT_NE(stack, nullptr);
    ASSERT_NE(aiPage, nullptr);

    EXPECT_EQ(stack->currentIndex(), 0);
    keyEdit->setText(QStringLiteral("will-fail"));
    EXPECT_TRUE(apply->isEnabled());

    acceptNextModalDialog();
    apply->click();
    EXPECT_EQ(stack->currentWidget(), aiPage);
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_TRUE(apply->isEnabled());
}

TEST_F(SettingsApplyContractTest, FailedApplyRollsBackEarlierPages)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PreferencesDialog::SecretStoreAccess failingStore;
    failingStore.load = [](const QString&, const QString&) { return QString(); };
    failingStore.store = [](const QString&, const QString&, const QString&) {
        return false;
    };
    failingStore.erase = [](const QString&, const QString&) { return true; };

    VNX::PreferencesDialog dialog(failingStore, manager);
    dialog.show();
    auto* apply = applyButton(dialog);
    auto* check = developerCheck(dialog);
    auto* language = dialog.findChild<QComboBox*>(QStringLiteral("appearanceLanguageCombo"));
    auto* keyEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(check, nullptr);
    ASSERT_NE(language, nullptr);
    ASSERT_NE(keyEdit, nullptr);

    const bool storedDeveloper = VNX::AppSettings::instance().developerMode();
    const QString storedLanguage = VNX::AppSettings::instance().language();
    const bool nextDeveloper = !check->isChecked();
    check->setChecked(nextDeveloper);

    const int nextLanguage = language->currentIndex() == 0 ? 1 : 0;
    language->setCurrentIndex(nextLanguage);
    const QString pendingLanguage = language->currentData().toString();
    ASSERT_NE(pendingLanguage, storedLanguage);

    keyEdit->setText(QStringLiteral("will-fail"));
    EXPECT_TRUE(apply->isEnabled());

    acceptNextModalDialog();
    apply->click();

    EXPECT_TRUE(dialog.isVisible());
    EXPECT_NE(dialog.result(), QDialog::Accepted);
    EXPECT_EQ(VNX::AppSettings::instance().developerMode(), storedDeveloper);
    EXPECT_EQ(VNX::AppSettings::instance().language(), storedLanguage);
    EXPECT_TRUE(apply->isEnabled());
    EXPECT_EQ(check->isChecked(), nextDeveloper);
    EXPECT_EQ(language->currentData().toString(), pendingLanguage);
    EXPECT_EQ(keyEdit->text(), QStringLiteral("will-fail"));
}

}
