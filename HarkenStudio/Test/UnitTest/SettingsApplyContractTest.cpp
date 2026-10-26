#include "Core/AppSettings.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeManager.h"
#include "Dialogs/Settings/PreferencesDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

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
    auto* apply = dialog.findChild<QPushButton*>(QStringLiteral("settingsApplyButton"));
    auto* check = dialog.findChild<QCheckBox*>(QStringLiteral("developerModeCheck"));
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

    acceptNextModalDialog();
    apply->click();

    EXPECT_EQ(VNX::AppSettings::instance().developerMode(), storedDeveloper);
    EXPECT_EQ(VNX::AppSettings::instance().language(), storedLanguage);
}

}
