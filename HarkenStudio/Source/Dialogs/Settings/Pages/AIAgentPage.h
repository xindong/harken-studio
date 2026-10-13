
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace VNX {

class OpenAICompatibleLLMProvider;

inline constexpr const char* kSettingsPageAIAgent = "ai_agent";

class AIAgentPage : public SettingsPage
{
    Q_OBJECT

public:
    struct SecretStoreAccess
    {
        std::function<QString(const QString& service, const QString& account)> load;
        std::function<bool(const QString& service, const QString& account,
                           const QString& secret)> store;
        std::function<bool(const QString& service, const QString& account)> erase;
    };

    explicit AIAgentPage(SecretStoreAccess secretStore, QWidget* parent = nullptr);

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;
    bool apply() override;
    bool isDirty() const override;

signals:
    void aiAgentConfigApplied();

private:
    void resetAIAgentSettings();
    void refreshAIApiKeyStatus();
    void refreshAIToolVisibility();
    void onAIApiKeyClearClicked();
    void onAIConnectionTestClicked();
    void finishAIConnectionTest(const QString& message, bool success);
    void rememberBaseline();
    bool vaultHasKey() const;
    void notifyDirty();

    QLineEdit* aiBaseUrlEdit_ = nullptr;
    QLineEdit* aiApiKeyEdit_ = nullptr;
    QLabel* aiApiKeyStatusLabel_ = nullptr;
    QPushButton* aiApiKeyClearButton_ = nullptr;
    QPushButton* aiConnectionTestButton_ = nullptr;
    QLabel* aiConnectionTestLabel_ = nullptr;
    QLineEdit* aiModelEdit_ = nullptr;
    QSpinBox* aiContextWindowSpin_ = nullptr;
    QComboBox* aiPermissionModeCombo_ = nullptr;
    QComboBox* aiToolProfileCombo_ = nullptr;
    QLabel* aiToolVisibilityLabel_ = nullptr;
    OpenAICompatibleLLMProvider* aiConnectionTestProvider_ = nullptr;
    QString aiConnectionTestRequestId_;
    SecretStoreAccess secretStore_;
    bool apiKeyClearStaged_ = false;

    QString baselineBaseUrl_;
    QString baselineModel_;
    int baselineContextWindow_ = 0;
    QString baselinePermissionMode_;
    QString baselineToolProfile_;
};

}
