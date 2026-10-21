
#include "Dialogs/Settings/Pages/AIAgentPage.h"

#include "Core/AI/AIAgentConfig.h"
#include "Core/AI/AIEndpointValidation.h"
#include "Core/AI/AIPermissionModel.h"
#include "Core/AI/OpenAICompatibleLLMProvider.h"
#include "Core/ContextInfo.h"
#include "Core/QtControlSkin.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUrl>

namespace VNX {

AIAgentPage::AIAgentPage(SecretStoreAccess secretStore, QWidget* parent)
    : SettingsPage(parent)
{
    secretStore_ = std::move(secretStore);
    if (!secretStore_.load)
        secretStore_.load = [](const QString&, const QString&) { return QString(); };
    if (!secretStore_.store)
        secretStore_.store = [](const QString&, const QString&, const QString&) { return false; };
    if (!secretStore_.erase)
        secretStore_.erase = [](const QString&, const QString&) { return false; };

    auto* layout = makePageBody();

    auto* credGroup = makeSection(tr("Credentials"));
    ContextInfo::tag(credGroup, ContextTopic::forPanel(QStringLiteral("settings.ai_agent.credentials")));
    auto* credForm = makeForm();

    aiBaseUrlEdit_ = new QLineEdit();
    aiBaseUrlEdit_->setObjectName("aiBaseUrlEdit");
    aiBaseUrlEdit_->setPlaceholderText(QStringLiteral("https://your-gateway/v1"));
    credForm->addRow(tr("Base URL:"), aiBaseUrlEdit_);

    aiApiKeyEdit_ = new QLineEdit();
    aiApiKeyEdit_->setObjectName("aiApiKeyEdit");
    aiApiKeyEdit_->setEchoMode(QLineEdit::Password);
    aiApiKeyEdit_->setPlaceholderText(tr("API key"));
    credForm->addRow(tr("API Key:"), aiApiKeyEdit_);

    aiModelEdit_ = new QLineEdit();
    aiModelEdit_->setObjectName("aiModelEdit");
    aiModelEdit_->setPlaceholderText(QStringLiteral("claude-opus-4-6"));
    credForm->addRow(tr("Model:"), aiModelEdit_);

    aiContextWindowSpin_ = new QSpinBox();
    aiContextWindowSpin_->setObjectName(QStringLiteral("aiContextWindowSpin"));
    aiContextWindowSpin_->setRange(4096, 2000000);
    aiContextWindowSpin_->setSingleStep(4096);
    aiContextWindowSpin_->setToolTip(tr("tokens"));
    credForm->addRow(tr("Context Window:"), aiContextWindowSpin_);

    auto* connectionTestWidget = new QWidget();
    auto* connectionTestRow = new QHBoxLayout(connectionTestWidget);
    connectionTestRow->setContentsMargins(0, 0, 0, 0);
    connectionTestRow->setSpacing(8);

    aiConnectionTestButton_ = new QPushButton(tr("Test Connection"));
    aiConnectionTestButton_->setObjectName("aiConnectionTestButton");
    ContextInfo::tag(aiConnectionTestButton_,
        ContextTopic::forPanel(QStringLiteral("settings.ai_agent.connection")));
    connectionTestRow->addWidget(aiConnectionTestButton_);

    aiConnectionTestLabel_ = new QLabel();
    aiConnectionTestLabel_->setObjectName("aiConnectionTestLabel");
    aiConnectionTestLabel_->setTextFormat(Qt::PlainText);
    aiConnectionTestLabel_->setWordWrap(true);
    connectionTestRow->addWidget(aiConnectionTestLabel_, 1);
    credForm->addRow(QString(), connectionTestWidget);

    connect(aiConnectionTestButton_, &QPushButton::clicked,
            this, &AIAgentPage::onAIConnectionTestClicked);

    static_cast<QVBoxLayout*>(credGroup->layout())->addLayout(credForm);

    layout->addWidget(credGroup);

    auto* permGroup = makeSection(tr("Permission"));
    ContextInfo::tag(permGroup, ContextTopic::forPanel(QStringLiteral("settings.ai_agent.permission")));
    auto* permForm = makeForm();
    aiPermissionModeCombo_ = new QComboBox();
    aiPermissionModeCombo_->setObjectName("aiPermissionModeCombo");
    for (const auto& metadata : permissionMetadata())
        aiPermissionModeCombo_->addItem(tr(metadata.label), QString::fromLatin1(metadata.id));
    permForm->addRow(tr("Mode:"), aiPermissionModeCombo_);
    static_cast<QVBoxLayout*>(permGroup->layout())->addLayout(permForm);

    layout->addWidget(permGroup);

    connect(aiBaseUrlEdit_, &QLineEdit::textChanged, this, &AIAgentPage::notifyDirty);
    connect(aiModelEdit_, &QLineEdit::textChanged, this, &AIAgentPage::notifyDirty);
    connect(aiContextWindowSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AIAgentPage::notifyDirty);
    connect(aiPermissionModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIAgentPage::notifyDirty);
    connect(aiApiKeyEdit_, &QLineEdit::textChanged, this, &AIAgentPage::notifyDirty);

    layout->addStretch();
}

QString AIAgentPage::pageId() const
{
    return QString::fromLatin1(kSettingsPageAIAgent);
}

QString AIAgentPage::categoryLabel() const
{
    return tr("AI Agent");
}

void AIAgentPage::notifyDirty()
{
    emit dirtyChanged();
}

void AIAgentPage::rememberBaseline()
{
    baselineBaseUrl_ = aiBaseUrlEdit_->text();
    baselineApiKey_ = aiApiKeyEdit_->text();
    baselineModel_ = aiModelEdit_->text();
    baselineContextWindow_ = aiContextWindowSpin_->value();
    baselinePermissionMode_ = aiPermissionModeCombo_->currentData().toString();
}

void AIAgentPage::load()
{
    const AIAgentConfig config = AIAgentConfigStore().effectiveSnapshot();

    const QSignalBlocker b1(aiBaseUrlEdit_);
    const QSignalBlocker b2(aiApiKeyEdit_);
    const QSignalBlocker b3(aiModelEdit_);
    const QSignalBlocker b4(aiContextWindowSpin_);
    const QSignalBlocker b5(aiPermissionModeCombo_);

    aiBaseUrlEdit_->setText(config.baseUrl);
    aiApiKeyEdit_->setText(secretStore_.load(QStringLiteral("HarkenStudio.ai"),
                                            QStringLiteral("api_key")));
    aiModelEdit_->setText(config.model);
    aiContextWindowSpin_->setValue(config.contextWindowTokens);

    int idx = aiPermissionModeCombo_->findData(toId(config.permissionProfile));
    aiPermissionModeCombo_->setCurrentIndex(idx);

    if (aiConnectionTestLabel_)
        aiConnectionTestLabel_->clear();
    rememberBaseline();
}

bool AIAgentPage::apply()
{
    const QString newBaseUrl = aiBaseUrlEdit_->text().trimmed();
    const QString newApiKey = aiApiKeyEdit_->text().trimmed();
    const QString newModel = aiModelEdit_->text().trimmed();
    const int newContextWindowTokens = aiContextWindowSpin_->value();
    const QString newMode = aiPermissionModeCombo_->currentData().toString();

    AIAgentConfigStore configStore;
    AIAgentConfig config = configStore.load();
    config.baseUrl = newBaseUrl;
    config.model = newModel;
    config.contextWindowTokens = newContextWindowTokens;
    config.permissionProfile = parsePermissionMode(newMode);

    const bool keyChanged = newApiKey != baselineApiKey_;
    if (keyChanged && newApiKey.isEmpty())
    {
        if (!secretStore_.erase(QStringLiteral("HarkenStudio.ai"),
                                QStringLiteral("api_key")))
        {
            QMessageBox::warning(this, tr("API Key"),
                                 tr("The saved API key could not be cleared."));
            return false;
        }
    }
    else if (keyChanged)
    {
        if (!secretStore_.store(QStringLiteral("HarkenStudio.ai"),
                                QStringLiteral("api_key"), newApiKey))
        {
            QMessageBox::warning(this, tr("API Key"),
                                 tr("The API key could not be saved."));
            return false;
        }
    }

    const bool settingsChanged = configStore.save(config);

    aiApiKeyEdit_->setText(newApiKey);
    rememberBaseline();
    emit dirtyChanged();
    if (settingsChanged || keyChanged)
        emit aiAgentConfigApplied();
    return true;
}

bool AIAgentPage::isDirty() const
{
    return aiBaseUrlEdit_->text() != baselineBaseUrl_
        || aiModelEdit_->text() != baselineModel_
        || aiContextWindowSpin_->value() != baselineContextWindow_
        || aiPermissionModeCombo_->currentData().toString() != baselinePermissionMode_
        || aiApiKeyEdit_->text() != baselineApiKey_;
}

void AIAgentPage::onAIConnectionTestClicked()
{
    if (!aiConnectionTestButton_ || !aiConnectionTestLabel_)
        return;

    const QString baseUrl = aiBaseUrlEdit_->text().trimmed();
    const QString model = aiModelEdit_->text().trimmed();
    QUrl endpoint;
    if (!isValidAIEndpoint(baseUrl, model, &endpoint))
    {
        finishAIConnectionTest(
            tr("Enter a valid HTTPS Base URL and a Model."), false);
        return;
    }

    const QString apiKey = aiApiKeyEdit_->text().trimmed();
    if (apiKey.isEmpty())
    {
        finishAIConnectionTest(tr("Enter an API key before testing."), false);
        return;
    }

    if (!aiConnectionTestProvider_)
    {
        aiConnectionTestProvider_ = new OpenAICompatibleLLMProvider(this);
        connect(aiConnectionTestProvider_, &ILLMProvider::completed, this,
                [this](const ILLMProvider::Response& response) {
            if (response.requestId != aiConnectionTestRequestId_)
                return;
            finishAIConnectionTest(tr("Connection succeeded."), true);
        });
        connect(aiConnectionTestProvider_, &ILLMProvider::failed, this,
                [this](const QString& requestId, const QString& code,
                       const QString& message) {
            if (requestId != aiConnectionTestRequestId_)
                return;
            finishAIConnectionTest(
                QStringLiteral("%1: %2").arg(code.left(64), message.left(512)), false);
        });
    }

    aiConnectionTestRequestId_ = QStringLiteral("settings-connection-test");
    aiConnectionTestButton_->setEnabled(false);
    aiConnectionTestLabel_->setText(tr("Testing..."));

    ILLMProvider::Request request;
    request.requestId = aiConnectionTestRequestId_;
    request.model = model;
    request.messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), QStringLiteral("Reply with OK.")}
    });
    request.apiKey = apiKey;
    request.endpoint = endpoint;
    request.maxTokens = 128;
    request.timeoutMs = 10000;
    aiConnectionTestProvider_->submit(request);
}

void AIAgentPage::finishAIConnectionTest(const QString& message, bool success)
{
    aiConnectionTestRequestId_.clear();
    if (aiConnectionTestButton_)
        aiConnectionTestButton_->setEnabled(true);
    if (!aiConnectionTestLabel_)
        return;

    QtControlSkin::setSemanticProperty(
        aiConnectionTestLabel_, "settingsRole",
        success ? QStringLiteral("statusSuccess") : QStringLiteral("statusError"));
    aiConnectionTestLabel_->setText(message.left(640));
}

}
