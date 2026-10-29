#include "Core/AI/AIAgentConfig.h"
#include "Core/AppSettings.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeManager.h"
#include "Dialogs/Settings/PreferencesPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>

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
        theme_ = std::make_unique<ThemeManager>(root_.path(), std::make_unique<MemorySettingsStorage>());
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

}
