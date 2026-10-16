#include "Core/AppSettings.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeManager.h"
#include "Dialogs/Settings/PreferencesDialog.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/TableWidget.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>

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

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

bool writeTheme(const QString& themesRoot, const QString& id)
{
    const QString packageRoot = QDir(themesRoot).filePath(id);
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":1,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"Studio Test\",\"version\":\"1.0.0\","
          "\"compatibility\":{\"harkenVersion\":\">=0.1.0 <1.0.0\","
          "\"capabilities\":[\"curve-v1\",\"meter-v1\",\"waveform-v1\"]},"
          "\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\",\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\"},\"assetMap\":\"assets.json\"}}";
    return writeFile(QDir(packageRoot).filePath(QStringLiteral("manifest.json")), manifest)
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("tokens.json")),
                     R"json({"colors":{"accent.primary":"#ff8844cc"}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/curve.json")),
                     R"json({"colors":{"curve.line":"#ff123456"}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/meter.json")),
                     R"json({"colors":{"meter.green":"#ff22aa44"}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/waveform.json")),
                     R"json({"colors":{"waveform.line":"#ff334455"}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("assets.json")),
                     R"json({"assets":{}})json");
}

VNX::PreferencesDialog::SecretStoreAccess inertSecretStore()
{
    VNX::PreferencesDialog::SecretStoreAccess access;
    access.load = [](const QString&, const QString&) { return QString(); };
    access.store = [](const QString&, const QString&, const QString&) { return true; };
    access.erase = [](const QString&, const QString&) { return true; };
    return access;
}

}

namespace VNX {

class ThemeSettingsDialogTest : public ::testing::Test
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

TEST_F(ThemeSettingsDialogTest, ListsDiscoveredThemesAndSelectsCurrentTheme)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writeTheme(root.path(), QStringLiteral("theme.studio-test")));
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());

    VNX::PreferencesDialog dialog(inertSecretStore(), manager);
    auto* combo = dialog.findChild<QComboBox*>(
        QStringLiteral("appearanceThemeCombo"));
    ASSERT_NE(combo, nullptr);

    EXPECT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->currentData().toString(), VNX::builtinThemeId());
    EXPECT_GE(combo->findData(QStringLiteral("theme.studio-test")), 0);
}

TEST_F(ThemeSettingsDialogTest, SavePublishesSelectedThemeExactlyOnce)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writeTheme(root.path(), QStringLiteral("theme.studio-test")));
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    int notifications = 0;
    QObject::connect(&manager, &VNX::ThemeManager::themeChanged,
                     [&notifications]() { ++notifications; });
    VNX::PreferencesDialog dialog(inertSecretStore(), manager);
    dialog.show();
    auto* combo = dialog.findChild<QComboBox*>(
        QStringLiteral("appearanceThemeCombo"));
    auto* apply = dialog.findChild<QPushButton*>(
        QStringLiteral("settingsApplyButton"));
    ASSERT_NE(combo, nullptr);
    ASSERT_NE(apply, nullptr);
    const int index = combo->findData(QStringLiteral("theme.studio-test"));
    ASSERT_GE(index, 0);

    combo->setCurrentIndex(index);
    EXPECT_TRUE(apply->isEnabled());
    apply->click();

    EXPECT_EQ(manager.currentThemeId(), QStringLiteral("theme.studio-test"));
    EXPECT_EQ(notifications, 1);
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
}

TEST_F(ThemeSettingsDialogTest, UsesCentralApplicationSkinRoles)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    VNX::PreferencesDialog dialog(inertSecretStore(), manager);

    EXPECT_TRUE(dialog.styleSheet().isEmpty());
    const auto widgets = dialog.findChildren<QWidget*>();
    for (const QWidget* widget : widgets)
        EXPECT_TRUE(widget->styleSheet().isEmpty()) << widget->metaObject()->className();

    auto* apply = dialog.findChild<QPushButton*>(QStringLiteral("settingsApplyButton"));
    auto* combo = dialog.findChild<QComboBox*>(QStringLiteral("appearanceThemeCombo"));
    auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("layoutTree"));
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(combo, nullptr);
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->property("tableRole").toString(), QStringLiteral("data"));
    auto* table = qobject_cast<VNX::TableWidget*>(tree->parentWidget());
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->options().kind, VNX::TableKind::Display);
}

TEST_F(ThemeSettingsDialogTest, SavingCurrentSelectionDoesNotRepublish)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    int notifications = 0;
    QObject::connect(&manager, &VNX::ThemeManager::themeChanged,
                     [&notifications]() { ++notifications; });
    VNX::PreferencesDialog dialog(inertSecretStore(), manager);
    auto* apply = dialog.findChild<QPushButton*>(
        QStringLiteral("settingsApplyButton"));
    ASSERT_NE(apply, nullptr);

    EXPECT_FALSE(apply->isEnabled());
    apply->click();

    EXPECT_EQ(notifications, 0);
    EXPECT_FALSE(apply->isEnabled());
}

}
