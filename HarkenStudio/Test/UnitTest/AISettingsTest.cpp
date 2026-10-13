#include "Dialogs/Settings/PreferencesDialog.h"
#include "AI/AIAgentConfig.h"
#include "Core/AppSettings.h"
#include "MemorySettingsStorage.h"
#include "Support/AITestSupport.h"
#include "Support/SyntheticHttpServer.h"

#include <gtest/gtest.h>

#include <memory>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

using namespace VNX;

namespace {

struct FakeSecretStore
{
    QString storedKey;
    int loadCalls = 0;
    int storeCalls = 0;
    int eraseCalls = 0;
    QString lastService;
    QString lastAccount;
    QString lastStoredSecret;
    bool storeSucceeds = true;
    bool eraseSucceeds = true;

    PreferencesDialog::SecretStoreAccess access()
    {
        PreferencesDialog::SecretStoreAccess result;
        result.load = [this](const QString& service, const QString& account) {
            ++loadCalls;
            lastService = service;
            lastAccount = account;
            return storedKey;
        };
        result.store = [this](const QString& service, const QString& account,
                              const QString& secret) {
            ++storeCalls;
            lastService = service;
            lastAccount = account;
            lastStoredSecret = secret;
            if (!storeSucceeds)
                return false;
            storedKey = secret;
            return true;
        };
        result.erase = [this](const QString& service, const QString& account) {
            ++eraseCalls;
            lastService = service;
            lastAccount = account;
            if (!eraseSucceeds)
                return false;
            storedKey.clear();
            return true;
        };
        return result;
    }
};

QByteArray connectionOkBody()
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("choices"), QJsonArray{
            QJsonObject{{QStringLiteral("message"),
                         QJsonObject{{QStringLiteral("content"), QStringLiteral("OK")}}}}
        }}
    }).toJson(QJsonDocument::Compact);
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

QPushButton* applyButton(PreferencesDialog& dialog)
{
    return dialog.findChild<QPushButton*>(QStringLiteral("settingsApplyButton"));
}

}

namespace VNX {

class AISettingsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
        storage_ = std::make_shared<MemorySettingsStorage>();
        AIAgentConfigStore::setStorageOverrideForTesting(storage_);
        AppSettings::instance().setStorage(std::make_unique<MemorySettingsStorage>());
    }

    void TearDown() override
    {
        AppSettings::instance().setStorage(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"),
            QStringLiteral("HarkenStudio")));
        AIAgentConfigStore::setStorageOverrideForTesting(nullptr);
    }

    std::shared_ptr<MemorySettingsStorage> storage_;
};

TEST_F(AISettingsTest, ApiKeyFieldIsWriteOnlyAndLoadNeverEchoesStoredKey)
{
    FakeSecretStore fake;
    fake.storedKey = QStringLiteral("vault-secret");
    PreferencesDialog dialog(fake.access());
    EXPECT_EQ(fake.loadCalls, 1);
    EXPECT_EQ(fake.lastService, QStringLiteral("HarkenStudio.ai"));
    EXPECT_EQ(fake.lastAccount, QStringLiteral("api_key"));

    auto* keyEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    ASSERT_NE(keyEdit, nullptr);
    EXPECT_EQ(keyEdit->echoMode(), QLineEdit::Password);
    EXPECT_TRUE(keyEdit->text().isEmpty());
    EXPECT_EQ(fake.storeCalls, 0);
}

TEST_F(AISettingsTest, BlankKeyPreservesStoredKeyNewKeyStoresClearErases)
{
    FakeSecretStore fake;
    fake.storedKey = QStringLiteral("old-key");
    {
        PreferencesDialog dialog(fake.access());
        dialog.show();

        auto* keyEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
        auto* apply = applyButton(dialog);
        ASSERT_NE(keyEdit, nullptr);
        ASSERT_NE(apply, nullptr);

        int applied = 0;
        QObject::connect(&dialog, &PreferencesDialog::aiAgentConfigApplied, &dialog,
                         [&applied]() { ++applied; });

        EXPECT_FALSE(apply->isEnabled());
        apply->click();
        EXPECT_EQ(fake.storeCalls, 0);
        EXPECT_EQ(fake.eraseCalls, 0);
        EXPECT_EQ(fake.storedKey, QStringLiteral("old-key"));
        EXPECT_EQ(applied, 0);
        EXPECT_TRUE(dialog.isVisible());

        keyEdit->setText(QStringLiteral("new-key"));
        EXPECT_TRUE(apply->isEnabled());
        apply->click();
        EXPECT_EQ(fake.storeCalls, 1);
        EXPECT_EQ(fake.lastService, QStringLiteral("HarkenStudio.ai"));
        EXPECT_EQ(fake.lastAccount, QStringLiteral("api_key"));
        EXPECT_EQ(fake.lastStoredSecret, QStringLiteral("new-key"));
        EXPECT_EQ(fake.storedKey, QStringLiteral("new-key"));
        EXPECT_TRUE(keyEdit->text().isEmpty());
        EXPECT_EQ(applied, 1);
        EXPECT_FALSE(dialog.isVisible());
        EXPECT_EQ(dialog.result(), QDialog::Accepted);
    }

    {
        PreferencesDialog dialog(fake.access());
        dialog.show();

        auto* keyEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
        auto* apply = applyButton(dialog);
        auto* clearButton = dialog.findChild<QPushButton*>(QStringLiteral("aiApiKeyClearButton"));
        ASSERT_NE(keyEdit, nullptr);
        ASSERT_NE(apply, nullptr);
        ASSERT_NE(clearButton, nullptr);

        int applied = 0;
        QObject::connect(&dialog, &PreferencesDialog::aiAgentConfigApplied, &dialog,
                         [&applied]() { ++applied; });

        EXPECT_TRUE(keyEdit->text().isEmpty());
        EXPECT_FALSE(apply->isEnabled());

        clearButton->click();
        EXPECT_EQ(fake.eraseCalls, 0);
        EXPECT_EQ(fake.storedKey, QStringLiteral("new-key"));
        EXPECT_TRUE(keyEdit->text().isEmpty());
        EXPECT_TRUE(apply->isEnabled());
        EXPECT_EQ(applied, 0);

        apply->click();
        EXPECT_EQ(fake.eraseCalls, 1);
        EXPECT_TRUE(fake.storedKey.isEmpty());
        EXPECT_TRUE(keyEdit->text().isEmpty());
        EXPECT_EQ(applied, 1);
        EXPECT_FALSE(dialog.isVisible());
        EXPECT_EQ(dialog.result(), QDialog::Accepted);
    }
}

TEST_F(AISettingsTest, ConnectionTestUsesUnsavedValuesTypedKeyThenSavedKeyFallback)
{
    TestSupport::SyntheticHttpServer server;
    ASSERT_TRUE(server.listen(true));

    FakeSecretStore fake;
    fake.storedKey = QStringLiteral("saved-key");
    PreferencesDialog dialog(fake.access());

    auto* baseUrlEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* modelEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiModelEdit"));
    auto* keyEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* testButton = dialog.findChild<QPushButton*>(QStringLiteral("aiConnectionTestButton"));
    ASSERT_NE(baseUrlEdit, nullptr);
    ASSERT_NE(modelEdit, nullptr);
    ASSERT_NE(keyEdit, nullptr);
    ASSERT_NE(testButton, nullptr);

    baseUrlEdit->setText(server.baseUrl().toString());
    modelEdit->setText(QStringLiteral("test-model"));
    keyEdit->setText(QStringLiteral("typed-key"));

    server.enqueueResponse({200, connectionOkBody(), {}, 0, false});
    testButton->click();
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return server.requestCount() == 1 && testButton->isEnabled();
    }, 5000));
    EXPECT_EQ(server.requestCount(), 1);
    EXPECT_EQ(server.lastPath(), QByteArrayLiteral("/v1/chat/completions"));
    EXPECT_EQ(server.lastHeader("Authorization"), QByteArrayLiteral("Bearer typed-key"));
    const QJsonObject body = QJsonDocument::fromJson(server.lastBody()).object();
    EXPECT_EQ(body.value(QStringLiteral("model")).toString(), QStringLiteral("test-model"));
    EXPECT_EQ(keyEdit->text(), QStringLiteral("typed-key"));
    EXPECT_TRUE(testButton->isEnabled());

    EXPECT_EQ(fake.storeCalls, 0);
    EXPECT_EQ(fake.eraseCalls, 0);
    storage_->beginGroup(QStringLiteral("ai"));
    EXPECT_FALSE(storage_->contains(QStringLiteral("base_url")));
    EXPECT_FALSE(storage_->contains(QStringLiteral("model")));
    storage_->endGroup();

    keyEdit->clear();
    server.enqueueResponse({200, connectionOkBody(), {}, 0, false});
    testButton->click();
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return server.requestCount() == 2 && testButton->isEnabled();
    }, 5000));
    EXPECT_EQ(server.lastHeader("Authorization"), QByteArrayLiteral("Bearer saved-key"));
    EXPECT_TRUE(testButton->isEnabled());
}

TEST_F(AISettingsTest, ConnectionTestValidationFailsBeforeAnyNetworkOrSecretUse)
{
    FakeSecretStore fake;
    PreferencesDialog dialog(fake.access());

    auto* baseUrlEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
    auto* modelEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiModelEdit"));
    auto* testButton = dialog.findChild<QPushButton*>(QStringLiteral("aiConnectionTestButton"));
    auto* testLabel = dialog.findChild<QLabel*>(QStringLiteral("aiConnectionTestLabel"));
    ASSERT_NE(baseUrlEdit, nullptr);
    ASSERT_NE(modelEdit, nullptr);
    ASSERT_NE(testButton, nullptr);
    ASSERT_NE(testLabel, nullptr);

    const int loadsAfterConstruction = fake.loadCalls;
    baseUrlEdit->setText(QStringLiteral("not-a-url"));
    modelEdit->setText(QStringLiteral("test-model"));
    testButton->click();
    EXPECT_TRUE(testButton->isEnabled());
    EXPECT_EQ(fake.loadCalls, loadsAfterConstruction);

    for (const auto& url : {QStringLiteral("http://example.test/v1"),
                            QStringLiteral("http://127.0.0.1:1234/v1")}) {
        baseUrlEdit->setText(url);
        testButton->click();
        EXPECT_EQ(fake.loadCalls, loadsAfterConstruction);
    }

    baseUrlEdit->setText(QStringLiteral("https://example.invalid/v1"));
    testButton->click();
    EXPECT_TRUE(testButton->isEnabled());
    EXPECT_EQ(fake.loadCalls, loadsAfterConstruction + 1);
}

TEST_F(AISettingsTest, ApplyWritesOnlyWhenChangedAndBypassNormalizesToAsk)
{
    FakeSecretStore fake;
    {
        PreferencesDialog dialog(fake.access());
        dialog.show();

        auto* baseUrlEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiBaseUrlEdit"));
        auto* modelEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiModelEdit"));
        auto* contextWindowSpin =
            dialog.findChild<QSpinBox*>(QStringLiteral("aiContextWindowSpin"));
        auto* apply = applyButton(dialog);
        ASSERT_NE(baseUrlEdit, nullptr);
        ASSERT_NE(modelEdit, nullptr);
        ASSERT_NE(contextWindowSpin, nullptr);
        ASSERT_NE(apply, nullptr);

        int applied = 0;
        QObject::connect(&dialog, &PreferencesDialog::aiAgentConfigApplied, &dialog,
                         [&applied]() { ++applied; });

        EXPECT_FALSE(apply->isEnabled());

        baseUrlEdit->setText(QStringLiteral("https://example.test/v1"));
        modelEdit->setText(QStringLiteral("model-one"));
        contextWindowSpin->setValue(65536);
        EXPECT_TRUE(apply->isEnabled());
        apply->click();
        EXPECT_EQ(applied, 1);
        EXPECT_FALSE(dialog.isVisible());
        EXPECT_EQ(dialog.result(), QDialog::Accepted);
        {
            const AIAgentConfig saved = AIAgentConfigStore().load();
            EXPECT_EQ(saved.baseUrl, QStringLiteral("https://example.test/v1"));
            EXPECT_EQ(saved.model, QStringLiteral("model-one"));
            EXPECT_EQ(saved.contextWindowTokens, 65536);
            EXPECT_EQ(toId(saved.permissionProfile), QStringLiteral("ask"));
            storage_->beginGroup(QStringLiteral("ai"));
            EXPECT_FALSE(storage_->contains(QStringLiteral("api_key")));
            storage_->endGroup();
        }
    }

    {
        PreferencesDialog dialog(fake.access());
        dialog.show();

        auto* modelEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiModelEdit"));
        auto* modeCombo = dialog.findChild<QComboBox*>(QStringLiteral("aiPermissionModeCombo"));
        auto* apply = applyButton(dialog);
        ASSERT_NE(modelEdit, nullptr);
        ASSERT_NE(modeCombo, nullptr);
        ASSERT_NE(apply, nullptr);

        int applied = 0;
        QObject::connect(&dialog, &PreferencesDialog::aiAgentConfigApplied, &dialog,
                         [&applied]() { ++applied; });

        EXPECT_FALSE(apply->isEnabled());
        apply->click();
        EXPECT_EQ(applied, 0);
        EXPECT_TRUE(dialog.isVisible());

        const int bypassIndex = modeCombo->findData(QStringLiteral("bypass"));
        ASSERT_GE(bypassIndex, 0);
        modeCombo->setCurrentIndex(bypassIndex);
        modelEdit->setText(QStringLiteral("model-two"));
        EXPECT_TRUE(apply->isEnabled());
        apply->click();
        EXPECT_EQ(applied, 1);
        EXPECT_FALSE(dialog.isVisible());
        {
            const AIAgentConfig saved = AIAgentConfigStore().load();
            EXPECT_EQ(toId(saved.permissionProfile), QStringLiteral("ask"));
            EXPECT_EQ(saved.model, QStringLiteral("model-two"));
        }
    }
}

TEST_F(AISettingsTest, VaultWriteFailureKeepsDialogOpen)
{
    FakeSecretStore fake;
    fake.storeSucceeds = false;
    PreferencesDialog dialog(fake.access());
    dialog.show();

    auto* keyEdit = dialog.findChild<QLineEdit*>(QStringLiteral("aiApiKeyEdit"));
    auto* apply = applyButton(dialog);
    ASSERT_NE(keyEdit, nullptr);
    ASSERT_NE(apply, nullptr);

    keyEdit->setText(QStringLiteral("will-fail"));
    EXPECT_TRUE(apply->isEnabled());
    acceptNextModalDialog();
    apply->click();

    EXPECT_EQ(fake.storeCalls, 1);
    EXPECT_TRUE(fake.storedKey.isEmpty());
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_TRUE(apply->isEnabled());
    EXPECT_NE(dialog.result(), QDialog::Accepted);
}

}
