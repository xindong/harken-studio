
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
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
    void discardConnectionChanges();
    bool hasPendingChanges() const;

signals:
    void aiAgentConfigApplied();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void onAIConnectionTestClicked();
    void finishAIConnectionTest(const QString& message, bool success);
    void invalidateConnectionTest();
    bool saveConnection();
    void savePermission();
    void refreshPermission();
    void refreshConnectionValidation();
    void rememberBaseline();
    void connectionEdited();

    QLineEdit* aiBaseUrlEdit_ = nullptr;
    QLineEdit* aiApiKeyEdit_ = nullptr;
    QPushButton* aiConnectionTestButton_ = nullptr;
    QPushButton* aiConnectionSaveButton_ = nullptr;
    QLabel* aiConnectionTestLabel_ = nullptr;
    QLineEdit* aiModelEdit_ = nullptr;
    QSpinBox* aiContextWindowSpin_ = nullptr;
    QComboBox* aiPermissionModeCombo_ = nullptr;
    OpenAICompatibleLLMProvider* aiConnectionTestProvider_ = nullptr;
    QString aiConnectionTestRequestId_;
    quint64 nextConnectionTestId_ = 0;
    SecretStoreAccess secretStore_;

    QString baselineBaseUrl_;
    QString baselineApiKey_;
    QString baselineModel_;
    int baselineContextWindow_ = 0;
};

}
