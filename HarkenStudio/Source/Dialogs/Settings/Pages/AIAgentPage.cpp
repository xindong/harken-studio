
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
#include <QPushButton>
#include <QSignalBlocker>
#include <QShowEvent>
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
    aiContextWindowSpin_->setKeyboardTracking(false);
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
    aiConnectionTestLabel_->hide();
    connectionTestRow->addStretch();

    aiConnectionSaveButton_ = new QPushButton(tr("Save Connection"));
    aiConnectionSaveButton_->setObjectName(QStringLiteral("aiConnectionSaveButton"));
    aiConnectionSaveButton_->setAutoDefault(false);
    connectionTestRow->addWidget(aiConnectionSaveButton_);
    credForm->addRow(QString(), connectionTestWidget);

    connect(aiConnectionTestButton_, &QPushButton::clicked,
            this, &AIAgentPage::onAIConnectionTestClicked);
    connect(aiConnectionSaveButton_, &QPushButton::clicked, this, [this] {
        saveConnection();
    });

    static_cast<QVBoxLayout*>(credGroup->layout())->addLayout(credForm);
    static_cast<QVBoxLayout*>(credGroup->layout())->addWidget(aiConnectionTestLabel_);

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

    connect(aiBaseUrlEdit_, &QLineEdit::textChanged, this, &AIAgentPage::connectionEdited);
    connect(aiModelEdit_, &QLineEdit::textChanged, this, &AIAgentPage::connectionEdited);
    connect(aiContextWindowSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AIAgentPage::connectionEdited);
    connect(aiPermissionModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIAgentPage::savePermission);
    connect(aiApiKeyEdit_, &QLineEdit::textChanged, this, &AIAgentPage::connectionEdited);

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

void AIAgentPage::connectionEdited()
{
    invalidateConnectionTest();
    refreshConnectionValidation();
}

void AIAgentPage::rememberBaseline()
{
    baselineBaseUrl_ = aiBaseUrlEdit_->text();
    baselineApiKey_ = aiApiKeyEdit_->text();
    baselineModel_ = aiModelEdit_->text();
    baselineContextWindow_ = aiContextWindowSpin_->value();
}

void AIAgentPage::load()
{
    refreshPermission();
    discardConnectionChanges();
}

void AIAgentPage::showEvent(QShowEvent* event)
{
    SettingsPage::showEvent(event);
    refreshPermission();
}

void AIAgentPage::refreshPermission()
{
    const AIAgentConfig config = AIAgentConfigStore().effectiveSnapshot();
    const QSignalBlocker blocker(aiPermissionModeCombo_);
    aiPermissionModeCombo_->setCurrentIndex(
        aiPermissionModeCombo_->findData(toId(config.permissionProfile)));
}

void AIAgentPage::discardConnectionChanges()
{
    const AIAgentConfig config = AIAgentConfigStore().effectiveSnapshot();

    const QSignalBlocker b1(aiBaseUrlEdit_);
    const QSignalBlocker b2(aiApiKeyEdit_);
    const QSignalBlocker b3(aiModelEdit_);
    const QSignalBlocker b4(aiContextWindowSpin_);

    aiBaseUrlEdit_->setText(config.baseUrl);
    aiApiKeyEdit_->setText(secretStore_.load(QStringLiteral("HarkenStudio.ai"),
                                            QStringLiteral("api_key")));
    aiModelEdit_->setText(config.model);
    aiContextWindowSpin_->setValue(config.contextWindowTokens);

    invalidateConnectionTest();
    rememberBaseline();
    refreshConnectionValidation();
}

bool AIAgentPage::saveConnection()
{
    const QString newBaseUrl = aiBaseUrlEdit_->text().trimmed();
    const QString newApiKey = aiApiKeyEdit_->text().trimmed();
    const QString newModel = aiModelEdit_->text().trimmed();
    const int newContextWindowTokens = aiContextWindowSpin_->value();
    if ((!newBaseUrl.isEmpty() || !newModel.isEmpty())
        && !isValidAIEndpoint(newBaseUrl, newModel))
        return false;

    AIAgentConfigStore configStore;
    AIAgentConfig config = configStore.load();
    const QString storedKey = secretStore_.load(QStringLiteral("HarkenStudio.ai"),
                                               QStringLiteral("api_key"));
    const auto current = config.effectiveSnapshot();
    if (current.baseUrl != baselineBaseUrl_ || current.model != baselineModel_
        || current.contextWindowTokens != baselineContextWindow_
        || storedKey != baselineApiKey_) {
        discardConnectionChanges();
        showError(tr("AI connection settings changed. The saved configuration was reloaded."));
        return false;
    }
    config.baseUrl = newBaseUrl;
    config.model = newModel;
    config.contextWindowTokens = newContextWindowTokens;

    const bool keyChanged = newApiKey != baselineApiKey_;
    if (keyChanged && newApiKey.isEmpty())
    {
        if (!secretStore_.erase(QStringLiteral("HarkenStudio.ai"),
                                QStringLiteral("api_key")))
        {
            showError(tr("The saved API key could not be cleared."));
            return false;
        }
    }
    else if (keyChanged)
    {
        if (!secretStore_.store(QStringLiteral("HarkenStudio.ai"),
                                QStringLiteral("api_key"), newApiKey))
        {
            showError(tr("The API key could not be saved."));
            return false;
        }
    }

    const bool settingsChanged = configStore.save(config);

    const QSignalBlocker b1(aiBaseUrlEdit_);
    const QSignalBlocker b2(aiApiKeyEdit_);
    const QSignalBlocker b3(aiModelEdit_);
    aiBaseUrlEdit_->setText(newBaseUrl);
    aiApiKeyEdit_->setText(newApiKey);
    aiModelEdit_->setText(newModel);
    rememberBaseline();
    refreshConnectionValidation();
    refreshPermission();
    if (settingsChanged || keyChanged)
        emit aiAgentConfigApplied();
    return true;
}

void AIAgentPage::savePermission()
{
    AIAgentConfigStore store;
    auto config = store.load();
    config.permissionProfile = parsePermissionMode(aiPermissionModeCombo_->currentData().toString());
    const bool changed = store.save(config);
    const QSignalBlocker blocker(aiPermissionModeCombo_);
    aiPermissionModeCombo_->setCurrentIndex(
        aiPermissionModeCombo_->findData(toId(config.permissionProfile)));
    if (changed)
        emit aiAgentConfigApplied();
}

bool AIAgentPage::hasPendingChanges() const
{
    return aiBaseUrlEdit_->text() != baselineBaseUrl_
        || aiModelEdit_->text() != baselineModel_
        || aiContextWindowSpin_->value() != baselineContextWindow_
        || aiApiKeyEdit_->text() != baselineApiKey_;
}

void AIAgentPage::refreshConnectionValidation()
{
    const QString baseUrl = aiBaseUrlEdit_->text().trimmed();
    const QString model = aiModelEdit_->text().trimmed();
    const bool unconfigured = baseUrl.isEmpty() && model.isEmpty();
    const bool validUrl = unconfigured
        || isValidAIEndpoint(baseUrl, model.isEmpty() ? QStringLiteral("model") : model);
    const bool validModel = unconfigured || !model.isEmpty();
    QtControlSkin::setSemanticProperty(aiBaseUrlEdit_, "inputInvalid", !validUrl);
    QtControlSkin::setSemanticProperty(aiModelEdit_, "inputInvalid", !validModel);
    aiBaseUrlEdit_->setToolTip(validUrl ? QString() : tr("Enter a valid HTTPS Base URL."));
    aiModelEdit_->setToolTip(validModel ? QString() : tr("Enter a model name."));
    aiConnectionSaveButton_->setEnabled(validUrl && validModel && hasPendingChanges());
}

void AIAgentPage::invalidateConnectionTest()
{
    const QString requestId = aiConnectionTestRequestId_;
    aiConnectionTestRequestId_.clear();
    if (aiConnectionTestProvider_ && !requestId.isEmpty())
        aiConnectionTestProvider_->cancel(requestId);
    aiConnectionTestButton_->setEnabled(true);
    aiConnectionTestLabel_->clear();
    aiConnectionTestLabel_->hide();
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

    aiConnectionTestRequestId_ = QStringLiteral("settings-connection-test-%1")
                                   .arg(++nextConnectionTestId_);
    aiConnectionTestButton_->setEnabled(false);
    aiConnectionTestLabel_->setText(tr("Testing..."));
    aiConnectionTestLabel_->show();

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
    aiConnectionTestLabel_->show();
}

}
