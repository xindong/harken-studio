#include "Core/AI/AIAgentConfig.h"
#include "Core/AppSettings.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeManager.h"
#include "Dialogs/Settings/PreferencesPanel.h"
#include "Dialogs/Settings/Pages/AppearancePage.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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


QString alternateBuiltinId(const VNX::ThemeManager& manager)
{
    for (const auto& theme : manager.availableThemes())
        if (theme.builtin && theme.id != manager.currentThemeId())
            return theme.id;
    return {};
}

bool selectPreset(QListWidget& presets, const QString& id)
{
    for (int i = 0; i < presets.count(); ++i) {
        auto* item = presets.item(i);
        if (item->data(Qt::UserRole).toString() == id) {
            presets.setCurrentItem(item);
            return true;
        }
    }
    return false;
}

bool selectColor(QTreeWidget& colors, const QString& key)
{
    for (int i = 0; i < colors.topLevelItemCount(); ++i) {
        auto* item = colors.topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == key) {
            colors.setCurrentItem(item);
            return true;
        }
    }
    return false;
}

bool finishColorEdit(QLineEdit& edit, const QColor& color)
{
    edit.setText(color.name(QColor::HexArgb));
    edit.setModified(true);
    return QMetaObject::invokeMethod(&edit, "editingFinished", Qt::DirectConnection);
}

QVariantMap storedValues(const VNX::ISettingsStorage& storage)
{
    QVariantMap values;
    for (const QString& key : storage.allKeys())
        values.insert(key, storage.value(key));
    return values;
}

class WritableSettingsStorage : public VNX::MemorySettingsStorage
{
public:
    bool isWritable() const override { return writable; }
    bool writable = true;
};

}

namespace VNX {

class SettingsApplyContractTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(root_.isValid());
        AppSettings::instance().setStorage(std::make_unique<MemorySettingsStorage>());
        AIAgentConfigStore::setStorageOverrideForTesting(std::make_shared<MemorySettingsStorage>());
        originalConfig_.baseUrl = QStringLiteral("https://original.example/v1");
        originalConfig_.model = QStringLiteral("original-model");
        AIAgentConfigStore().save(originalConfig_);
        auto storage = std::make_unique<MemorySettingsStorage>();
        themeStorage_ = storage.get();
        theme_ = std::make_unique<ThemeManager>(root_.path(), std::move(storage));
    }

    void TearDown() override
    {
        theme_.reset();
        AIAgentConfigStore::setStorageOverrideForTesting(nullptr);
        AppSettings::instance().setStorage(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"),
            QStringLiteral("HarkenStudio")));
    }

    PreferencesPanel::SecretStoreAccess secretStore()
    {
        return {
            [this](const QString&, const QString&) { return secret_; },
            [this](const QString&, const QString&, const QString& value) {
                if (failSecretWrite_) return false;
                secret_ = value;
                return true;
            },
            [this](const QString&, const QString&) {
                if (failSecretWrite_) return false;
                secret_.clear();
                return true;
            }
        };
    }

    QTemporaryDir root_;
    std::unique_ptr<ThemeManager> theme_;
    MemorySettingsStorage* themeStorage_ = nullptr;
    AIAgentConfig originalConfig_;
    QString secret_ = QStringLiteral("original-key");
    bool failSecretWrite_ = false;
};

TEST_F(SettingsApplyContractTest, FailedConnectionCommitKeepsIndependentPreferences)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* developer = panel.findChild<QCheckBox*>(QStringLiteral("developerModeCheck"));
    auto* language = panel.findChild<QComboBox*>(QStringLiteral("appearanceLanguageCombo"));
    auto* url = panel.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* key = panel.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* save = panel.findChild<QPushButton*>(QStringLiteral("aiConnectionSaveButton"));
    ASSERT_NE(developer, nullptr);
    ASSERT_NE(language, nullptr);
    ASSERT_NE(url, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(save, nullptr);

    developer->setChecked(true);
    language->setCurrentIndex(language->findData(QStringLiteral("zh-CN")));
    url->setText(QStringLiteral("https://replacement.example/v1"));
    key->setText(QStringLiteral("replacement-key"));
    failSecretWrite_ = true;
    ASSERT_TRUE(save->isEnabled());
    save->click();

    EXPECT_TRUE(AppSettings::instance().developerMode());
    EXPECT_EQ(AppSettings::instance().language(), QStringLiteral("zh-CN"));
    EXPECT_TRUE(AIAgentConfigStore().effectiveSnapshot() == originalConfig_);
    EXPECT_EQ(secret_, QStringLiteral("original-key"));
    EXPECT_TRUE(panel.findChild<AIAgentPage*>()->hasPendingChanges());
}

TEST_F(SettingsApplyContractTest, OpenPreferencesFollowExternalCommittedValues)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* developer = panel.findChild<QCheckBox*>(QStringLiteral("developerModeCheck"));
    auto* language = panel.findChild<QComboBox*>(QStringLiteral("appearanceLanguageCombo"));
    ASSERT_NE(developer, nullptr);
    ASSERT_NE(language, nullptr);

    AppSettings::instance().setDeveloperMode(true);
    AppSettings::instance().setLanguage(QStringLiteral("zh-CN"));
    EXPECT_TRUE(developer->isChecked());
    EXPECT_EQ(language->currentData().toString(), QStringLiteral("zh-CN"));

    AppSettings::instance().setDeveloperMode(false);
    AppSettings::instance().setLanguage(QStringLiteral("en"));
    EXPECT_FALSE(developer->isChecked());
    EXPECT_EQ(language->currentData().toString(), QStringLiteral("en"));
}

TEST_F(SettingsApplyContractTest, SearchNavigationPreservesDraftsAndCommittedSettings)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("settingsSearchField"));
    auto* empty = panel.findChild<QLabel*>(QStringLiteral("settingsSearchEmptyState"));
    auto* developer = panel.findChild<QCheckBox*>(QStringLiteral("developerModeCheck"));
    auto* url = panel.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* key = panel.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* status = panel.findChild<QLabel*>(QStringLiteral("aiConnectionTestLabel"));
    auto* page = panel.findChild<AIAgentPage*>();
    auto* appearance = panel.findChild<AppearancePage*>();
    ASSERT_NE(search, nullptr);
    ASSERT_NE(empty, nullptr);
    ASSERT_NE(developer, nullptr);
    ASSERT_NE(url, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(appearance, nullptr);

    panel.show();
    ASSERT_TRUE(panel.showPage(page->pageId()));
    QCoreApplication::processEvents();
    const QString draftUrl = QStringLiteral("https://draft-search-value.example/v1");
    const QString draftKey = QStringLiteral("draft-search-secret-token");
    developer->setChecked(true);
    url->setText(draftUrl);
    key->setText(draftKey);
    ASSERT_TRUE(page->hasPendingChanges());

    search->setText(appearance->categoryLabel());
    EXPECT_EQ(panel.currentPageId(), appearance->pageId());
    EXPECT_TRUE(appearance->isVisible());
    for (const QString& query : {draftUrl, draftKey, secret_}) {
        search->setText(query);
        EXPECT_TRUE(empty->isVisible());
    }
    search->clear();
    EXPECT_FALSE(empty->isVisible());
    search->setText(appearance->categoryLabel());
    ASSERT_TRUE(panel.showPage(page->pageId()));

    EXPECT_EQ(panel.currentPageId(), page->pageId());
    EXPECT_TRUE(page->isVisible());
    EXPECT_TRUE(url->isVisible());
    EXPECT_TRUE(key->isVisible());
    EXPECT_TRUE(status->isHidden());
    EXPECT_EQ(url->text(), draftUrl);
    EXPECT_EQ(key->text(), draftKey);
    EXPECT_TRUE(page->hasPendingChanges());
    EXPECT_TRUE(AIAgentConfigStore().effectiveSnapshot() == originalConfig_);
    EXPECT_EQ(secret_, QStringLiteral("original-key"));
    EXPECT_TRUE(AppSettings::instance().developerMode());
    EXPECT_TRUE(developer->isChecked());
}

TEST_F(SettingsApplyContractTest, DiscardPendingConnectionPreservesImmediatePermission)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* url = panel.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* key = panel.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* permission = panel.findChild<QComboBox*>(QStringLiteral("aiPermissionModeCombo"));
    auto* page = panel.findChild<AIAgentPage*>();
    ASSERT_NE(url, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(permission, nullptr);
    ASSERT_NE(page, nullptr);

    url->setText(QStringLiteral("https://unsaved.example/v1"));
    key->setText(QStringLiteral("unsaved-key"));
    permission->setCurrentIndex(permission->findData(QStringLiteral("read_only")));
    ASSERT_TRUE(page->hasPendingChanges());
    panel.discardConnectionChanges();

    auto expected = originalConfig_;
    expected.permissionProfile = AIPermissionMode::ReadOnly;
    EXPECT_TRUE(AIAgentConfigStore().effectiveSnapshot() == expected);
    EXPECT_EQ(secret_, QStringLiteral("original-key"));
    EXPECT_EQ(url->text(), originalConfig_.baseUrl);
    EXPECT_EQ(key->text(), secret_);
    EXPECT_FALSE(page->hasPendingChanges());
}

TEST_F(SettingsApplyContractTest, ConnectionCommitPreservesLatestIndependentSettings)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* url = panel.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* model = panel.findChild<QLineEdit*>(QStringLiteral("aiModelEdit"));
    auto* key = panel.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* save = panel.findChild<QPushButton*>(QStringLiteral("aiConnectionSaveButton"));
    auto* permission = panel.findChild<QComboBox*>(QStringLiteral("aiPermissionModeCombo"));
    ASSERT_NE(url, nullptr);
    ASSERT_NE(model, nullptr);
    ASSERT_NE(key, nullptr);
    ASSERT_NE(save, nullptr);
    ASSERT_NE(permission, nullptr);

    url->setText(QStringLiteral("https://replacement.example/v1"));
    model->setText(QStringLiteral("replacement-model"));
    key->setText(QStringLiteral("replacement-key"));
    auto expected = originalConfig_;
    expected.permissionProfile = AIPermissionMode::ReadOnly;
    expected.maxTokens = 2048;
    AIAgentConfigStore().save(expected);
    ASSERT_TRUE(save->isEnabled());
    save->click();

    expected.baseUrl = url->text();
    expected.model = model->text();
    EXPECT_TRUE(AIAgentConfigStore().effectiveSnapshot() == expected);
    EXPECT_EQ(secret_, QStringLiteral("replacement-key"));
    EXPECT_EQ(permission->currentData().toString(), QStringLiteral("read_only"));
    EXPECT_FALSE(panel.findChild<AIAgentPage*>()->hasPendingChanges());
}

TEST_F(SettingsApplyContractTest, AppearanceEditsPersistAcrossPresetSwitchesAndReset)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* presets = panel.findChild<QListWidget*>(QStringLiteral("appearanceThemePresets"));
    auto* colors = panel.findChild<QTreeWidget*>(QStringLiteral("appearanceColorTree"));
    auto* hex = panel.findChild<QLineEdit*>(QStringLiteral("appearanceColorHex"));
    auto* reset = panel.findChild<QPushButton*>(QStringLiteral("appearanceColorReset"));
    auto* resetAll = panel.findChild<QPushButton*>(QStringLiteral("appearanceColorsReset"));
    ASSERT_NE(presets, nullptr);
    ASSERT_NE(colors, nullptr);
    ASSERT_NE(hex, nullptr);
    ASSERT_NE(reset, nullptr);
    ASSERT_NE(resetAll, nullptr);

    const QString firstId = theme_->currentThemeId();
    const QString secondId = alternateBuiltinId(*theme_);
    ASSERT_FALSE(secondId.isEmpty());
    const QString primary = QStringLiteral("text.primary");
    const QString secondary = QStringLiteral("text.secondary");
    const QColor firstColor(18, 52, 86, 128);
    const QColor secondColor(135, 114, 89, 192);
    const auto defaults = theme_->snapshot();
    const auto before = storedValues(*themeStorage_);

    ASSERT_TRUE(selectColor(*colors, primary));
    ASSERT_TRUE(finishColorEdit(*hex, firstColor));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, firstColor);
    EXPECT_EQ(theme_->appearance().colors.value(firstId).value(primary), firstColor);
    EXPECT_NE(storedValues(*themeStorage_), before);
    ASSERT_TRUE(selectColor(*colors, secondary));
    ASSERT_TRUE(finishColorEdit(*hex, secondColor));
    ASSERT_TRUE(selectPreset(*presets, secondId));
    EXPECT_EQ(theme_->appearance().themeId, secondId);
    ASSERT_TRUE(selectColor(*colors, primary));
    ASSERT_TRUE(finishColorEdit(*hex, secondColor));

    ASSERT_TRUE(selectPreset(*presets, firstId));
    EXPECT_EQ(QColor(hex->text()), firstColor);
    ASSERT_TRUE(reset->isEnabled());
    reset->click();
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, defaults->ui.colors.textPrimary);
    EXPECT_FALSE(theme_->appearance().colors.value(firstId).contains(primary));
    EXPECT_EQ(theme_->appearance().colors.value(firstId).value(secondary), secondColor);
    ASSERT_TRUE(resetAll->isEnabled());
    resetAll->click();
    EXPECT_FALSE(theme_->appearance().colors.contains(firstId));
    EXPECT_EQ(theme_->snapshot()->ui.colors.textSecondary, defaults->ui.colors.textSecondary);
    EXPECT_EQ(theme_->appearance().colors.value(secondId).value(primary), secondColor);

    ASSERT_TRUE(selectPreset(*presets, secondId));
    EXPECT_EQ(QColor(hex->text()), secondColor);
    ThemeManager restored(root_.path(), std::make_unique<MemorySettingsStorage>(*themeStorage_));
    EXPECT_EQ(restored.appearance(), theme_->appearance());
    EXPECT_EQ(restored.snapshot()->ui.colors.textPrimary, secondColor);
}

TEST_F(SettingsApplyContractTest, AppearanceFollowsExternalCommittedThemeAndColors)
{
    PreferencesPanel panel(secretStore(), *theme_);
    auto* presets = panel.findChild<QListWidget*>(QStringLiteral("appearanceThemePresets"));
    auto* colors = panel.findChild<QTreeWidget*>(QStringLiteral("appearanceColorTree"));
    auto* hex = panel.findChild<QLineEdit*>(QStringLiteral("appearanceColorHex"));
    ASSERT_NE(presets, nullptr);
    ASSERT_NE(colors, nullptr);
    ASSERT_NE(hex, nullptr);
    ASSERT_TRUE(selectColor(*colors, QStringLiteral("text.primary")));

    auto appearance = theme_->appearance();
    appearance.themeId = alternateBuiltinId(*theme_);
    const QColor externalColor(63, 95, 127, 160);
    appearance.colors[appearance.themeId].insert(QStringLiteral("text.primary"), externalColor);
    ASSERT_TRUE(theme_->applyAppearance(appearance).ok());

    ASSERT_NE(presets->currentItem(), nullptr);
    EXPECT_EQ(presets->currentItem()->data(Qt::UserRole).toString(), appearance.themeId);
    EXPECT_EQ(QColor(hex->text()), externalColor);
    EXPECT_FALSE(hex->isModified());
}

TEST_F(SettingsApplyContractTest, ColorPickerPreviewRequiresAcceptance)
{
    const auto committed = theme_->appearance();
    const auto original = theme_->snapshot();
    const auto before = storedValues(*themeStorage_);
    const QColor previewColor(27, 81, 143, 176);
    {
        AppearancePage page(*theme_);
        page.load();
        auto* colors = page.findChild<QTreeWidget*>(QStringLiteral("appearanceColorTree"));
        auto* edit = page.findChild<QPushButton*>(QStringLiteral("appearanceColorEdit"));
        ASSERT_NE(colors, nullptr);
        ASSERT_NE(edit, nullptr);
        ASSERT_TRUE(selectColor(*colors, QStringLiteral("text.primary")));
        edit->click();
        auto* picker = page.findChild<QColorDialog*>(QStringLiteral("appearanceColorDialog"));
        ASSERT_NE(picker, nullptr);
        picker->setCurrentColor(previewColor);
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, previewColor);
        EXPECT_EQ(theme_->appearance(), committed);
        EXPECT_EQ(storedValues(*themeStorage_), before);

        picker->reject();
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, original->ui.colors.textPrimary);
        EXPECT_EQ(theme_->appearance(), committed);
        EXPECT_EQ(storedValues(*themeStorage_), before);
        QCoreApplication::sendPostedEvents(picker, QEvent::DeferredDelete);

        edit->click();
        picker = page.findChild<QColorDialog*>(QStringLiteral("appearanceColorDialog"));
        ASSERT_NE(picker, nullptr);
        picker->setCurrentColor(previewColor);
        picker->accept();
        EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, previewColor);
        EXPECT_EQ(theme_->appearance().colors.value(committed.themeId)
                      .value(QStringLiteral("text.primary")), previewColor);
        EXPECT_NE(storedValues(*themeStorage_), before);
    }
    EXPECT_EQ(theme_->snapshot()->ui.colors.textPrimary, previewColor);
    ThemeManager restored(root_.path(), std::make_unique<MemorySettingsStorage>(*themeStorage_));
    EXPECT_EQ(restored.appearance(), theme_->appearance());
    EXPECT_EQ(restored.snapshot()->ui.colors.textPrimary, previewColor);
}

TEST_F(SettingsApplyContractTest, FailedPickerCommitAndPageDestructionRestoreCommittedAppearance)
{
    auto storage = std::make_unique<WritableSettingsStorage>();
    auto* stored = storage.get();
    ThemeManager manager(root_.path(), std::move(storage));
    const auto committed = manager.appearance();
    const auto original = manager.snapshot();
    const auto before = storedValues(*stored);
    const QColor previewColor(41, 97, 151, 192);
    {
        AppearancePage page(manager);
        page.load();
        auto* colors = page.findChild<QTreeWidget*>(QStringLiteral("appearanceColorTree"));
        auto* edit = page.findChild<QPushButton*>(QStringLiteral("appearanceColorEdit"));
        auto* hex = page.findChild<QLineEdit*>(QStringLiteral("appearanceColorHex"));
        auto* error = page.findChild<QLabel*>(QStringLiteral("appearanceColorError"));
        ASSERT_NE(colors, nullptr);
        ASSERT_NE(edit, nullptr);
        ASSERT_NE(hex, nullptr);
        ASSERT_NE(error, nullptr);
        ASSERT_TRUE(selectColor(*colors, QStringLiteral("text.primary")));
        edit->click();
        auto* picker = page.findChild<QColorDialog*>(QStringLiteral("appearanceColorDialog"));
        ASSERT_NE(picker, nullptr);
        picker->setCurrentColor(previewColor);
        ASSERT_EQ(manager.snapshot()->ui.colors.textPrimary, previewColor);
        stored->writable = false;
        picker->accept();

        EXPECT_EQ(manager.appearance(), committed);
        EXPECT_EQ(manager.snapshot()->ui.colors.textPrimary, original->ui.colors.textPrimary);
        EXPECT_EQ(QColor(hex->text()), original->ui.colors.textPrimary);
        EXPECT_EQ(storedValues(*stored), before);
        EXPECT_FALSE(error->text().isEmpty());
        QCoreApplication::sendPostedEvents(picker, QEvent::DeferredDelete);

        edit->click();
        picker = page.findChild<QColorDialog*>(QStringLiteral("appearanceColorDialog"));
        ASSERT_NE(picker, nullptr);
        picker->setCurrentColor(previewColor);
        ASSERT_EQ(manager.snapshot()->ui.colors.textPrimary, previewColor);
    }
    EXPECT_EQ(manager.appearance(), committed);
    EXPECT_EQ(manager.snapshot()->ui.colors.textPrimary, original->ui.colors.textPrimary);
    EXPECT_EQ(storedValues(*stored), before);
}

}
