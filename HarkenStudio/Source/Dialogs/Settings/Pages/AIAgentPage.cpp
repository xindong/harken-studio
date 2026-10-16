
#include "Dialogs/Settings/Pages/AIAgentPage.h"

#include "Core/AI/AIAgentConfig.h"
#include "Core/AI/AICommandCatalog.h"
#include "Core/AI/AICommandValidator.h"
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

    auto* apiKeyRowWidget = new QWidget();
    auto* apiKeyRow = new QHBoxLayout(apiKeyRowWidget);
    apiKeyRow->setContentsMargins(0, 0, 0, 0);
    apiKeyRow->setSpacing(8);
    apiKeyRow->addWidget(aiApiKeyEdit_, 1);

    aiApiKeyClearButton_ = new QPushButton(tr("Clear Saved Key"));
    aiApiKeyClearButton_->setObjectName("aiApiKeyClearButton");
    apiKeyRow->addWidget(aiApiKeyClearButton_);
    credForm->addRow(tr("API Key:"), apiKeyRowWidget);

    aiApiKeyStatusLabel_ = new QLabel();
    aiApiKeyStatusLabel_->setObjectName("aiApiKeyStatusLabel");
    aiApiKeyStatusLabel_->setWordWrap(true);
    credForm->addRow(tr("Stored Key:"), aiApiKeyStatusLabel_);

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

    connect(aiApiKeyClearButton_, &QPushButton::clicked,
            this, &AIAgentPage::onAIApiKeyClearClicked);
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

    auto* scopeGroup = makeSection(tr("Tool Scope"));
    ContextInfo::tag(scopeGroup, ContextTopic::forPanel(QStringLiteral("settings.ai_agent.tools")));
    auto* scopeForm = makeForm();
    aiToolProfileCombo_ = new QComboBox();
    aiToolProfileCombo_->setObjectName("aiToolProfileCombo");
    for (const auto& metadata : toolProfileMetadata())
        aiToolProfileCombo_->addItem(tr(metadata.label), toId(metadata.profile));
    scopeForm->addRow(tr("Profile:"), aiToolProfileCombo_);

    aiToolVisibilityLabel_ = new QLabel();
    aiToolVisibilityLabel_->setObjectName(QStringLiteral("aiToolVisibilityLabel"));
    aiToolVisibilityLabel_->setWordWrap(true);
    scopeForm->addRow(tr("Effective tools:"), aiToolVisibilityLabel_);
    static_cast<QVBoxLayout*>(scopeGroup->layout())->addLayout(scopeForm);
    layout->addWidget(scopeGroup);

    auto* resetRow = new QHBoxLayout();
    auto* resetBtn = new QPushButton(tr("Reset to Defaults"));
    resetBtn->setObjectName(QStringLiteral("aiResetDefaultsButton"));
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        resetAIAgentSettings();
    });
    resetRow->addWidget(resetBtn);
    resetRow->addStretch();
    layout->addLayout(resetRow);

    connect(aiToolProfileCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIAgentPage::refreshAIToolVisibility);
    connect(aiPermissionModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIAgentPage::refreshAIToolVisibility);

    connect(aiBaseUrlEdit_, &QLineEdit::textChanged, this, &AIAgentPage::notifyDirty);
    connect(aiModelEdit_, &QLineEdit::textChanged, this, &AIAgentPage::notifyDirty);
    connect(aiContextWindowSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &AIAgentPage::notifyDirty);
    connect(aiPermissionModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIAgentPage::notifyDirty);
    connect(aiToolProfileCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AIAgentPage::notifyDirty);
    connect(aiApiKeyEdit_, &QLineEdit::textChanged, this, [this]() {
        if (!aiApiKeyEdit_->text().isEmpty())
            apiKeyClearStaged_ = false;
        refreshAIApiKeyStatus();
        notifyDirty();
    });

    refreshAIToolVisibility();
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

bool AIAgentPage::vaultHasKey() const
{
    return !secretStore_.load(QStringLiteral("HarkenStudio.ai"),
                              QStringLiteral("api_key")).isEmpty();
}

void AIAgentPage::rememberBaseline()
{
    baselineBaseUrl_ = aiBaseUrlEdit_->text();
    baselineModel_ = aiModelEdit_->text();
    baselineContextWindow_ = aiContextWindowSpin_->value();
    baselinePermissionMode_ = aiPermissionModeCombo_->currentData().toString();
    baselineToolProfile_ = aiToolProfileCombo_->currentData().toString();
    apiKeyClearStaged_ = false;
}

void AIAgentPage::load()
{
    const AIAgentConfig config = AIAgentConfigStore().effectiveSnapshot();

    const QSignalBlocker b1(aiBaseUrlEdit_);
    const QSignalBlocker b2(aiApiKeyEdit_);
    const QSignalBlocker b3(aiModelEdit_);
    const QSignalBlocker b4(aiContextWindowSpin_);
    const QSignalBlocker b5(aiPermissionModeCombo_);
    const QSignalBlocker b6(aiToolProfileCombo_);

    aiBaseUrlEdit_->setText(config.baseUrl);
    aiApiKeyEdit_->clear();
    aiModelEdit_->setText(config.model);
    aiContextWindowSpin_->setValue(config.contextWindowTokens);

    int idx = aiPermissionModeCombo_->findData(toId(config.permissionProfile));
    aiPermissionModeCombo_->setCurrentIndex(idx);

    int pidx = aiToolProfileCombo_->findData(config.toolProfile);
    if (pidx < 0)
        pidx = aiToolProfileCombo_->findData(QStringLiteral("authoring"));
    aiToolProfileCombo_->setCurrentIndex(pidx < 0 ? 0 : pidx);

    apiKeyClearStaged_ = false;
    refreshAIToolVisibility();
    refreshAIApiKeyStatus();
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
    const QString newProfile = aiToolProfileCombo_->currentData().toString();

    AIAgentConfigStore configStore;
    AIAgentConfig config = configStore.load();
    config.baseUrl = newBaseUrl;
    config.model = newModel;
    config.contextWindowTokens = newContextWindowTokens;
    config.permissionProfile = parsePermissionMode(newMode);
    config.toolProfile = newProfile;

    bool keyChanged = false;
    if (apiKeyClearStaged_)
    {
        if (!secretStore_.erase(QStringLiteral("HarkenStudio.ai"),
                                QStringLiteral("api_key")))
        {
            QMessageBox::warning(this, tr("API Key"),
                                 tr("The saved API key could not be cleared."));
            refreshAIApiKeyStatus();
            return false;
        }
        keyChanged = true;
        apiKeyClearStaged_ = false;
        aiApiKeyEdit_->clear();
    }
    else if (!newApiKey.isEmpty())
    {
        if (!secretStore_.store(QStringLiteral("HarkenStudio.ai"),
                                QStringLiteral("api_key"), newApiKey))
        {
            QMessageBox::warning(this, tr("API Key"),
                                 tr("The API key could not be saved."));
            refreshAIApiKeyStatus();
            return false;
        }
        keyChanged = true;
        aiApiKeyEdit_->clear();
    }

    const bool settingsChanged = configStore.save(config);

    rememberBaseline();
    refreshAIApiKeyStatus();
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
        || aiToolProfileCombo_->currentData().toString() != baselineToolProfile_
        || !aiApiKeyEdit_->text().isEmpty()
        || apiKeyClearStaged_;
}

void AIAgentPage::refreshAIApiKeyStatus()
{
    if (!aiApiKeyStatusLabel_)
        return;

    const bool typed = aiApiKeyEdit_ && !aiApiKeyEdit_->text().trimmed().isEmpty();
    if (apiKeyClearStaged_ && !typed)
    {
        QtControlSkin::setSemanticProperty(aiApiKeyStatusLabel_, "settingsRole",
                                           QStringLiteral("statusWarning"));
        aiApiKeyStatusLabel_->setText(tr("Removal pending"));
        return;
    }
    if (typed)
    {
        QtControlSkin::setSemanticProperty(aiApiKeyStatusLabel_, "settingsRole",
                                           QStringLiteral("hint"));
        aiApiKeyStatusLabel_->setText(
            vaultHasKey() ? tr("Replacement pending")
                          : tr("Save pending"));
        return;
    }

    QtControlSkin::setSemanticProperty(aiApiKeyStatusLabel_, "settingsRole",
                                       QString());
    aiApiKeyStatusLabel_->setText(vaultHasKey() ? tr("Saved") : tr("Not saved"));
}

void AIAgentPage::onAIApiKeyClearClicked()
{
    apiKeyClearStaged_ = true;
    aiApiKeyEdit_->clear();
    refreshAIApiKeyStatus();
    notifyDirty();
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

    QString apiKey = aiApiKeyEdit_->text().trimmed();
    if (apiKey.isEmpty())
    {
        apiKey = secretStore_.load(
            QStringLiteral("HarkenStudio.ai"), QStringLiteral("api_key"));
    }
    if (apiKey.isEmpty())
    {
        finishAIConnectionTest(tr("Enter or save an API key before testing."), false);
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

void AIAgentPage::resetAIAgentSettings()
{
    const QSignalBlocker b1(aiBaseUrlEdit_);
    const QSignalBlocker b2(aiApiKeyEdit_);
    const QSignalBlocker b3(aiModelEdit_);
    const QSignalBlocker b4(aiContextWindowSpin_);
    const QSignalBlocker b5(aiPermissionModeCombo_);
    const QSignalBlocker b6(aiToolProfileCombo_);
    aiBaseUrlEdit_->clear();
    aiApiKeyEdit_->clear();
    aiModelEdit_->clear();
    aiContextWindowSpin_->setValue(AIAgentConfig::defaults().contextWindowTokens);
    aiPermissionModeCombo_->setCurrentIndex(
        aiPermissionModeCombo_->findData(QStringLiteral("ask")));
    aiToolProfileCombo_->setCurrentIndex(
        aiToolProfileCombo_->findData(QStringLiteral("authoring")));
    apiKeyClearStaged_ = false;
    refreshAIToolVisibility();
    refreshAIApiKeyStatus();
    notifyDirty();
}

void AIAgentPage::refreshAIToolVisibility()
{
    if (!aiToolVisibilityLabel_ || !aiToolProfileCombo_ || !aiPermissionModeCombo_)
        return;

    const QString profile = aiToolProfileCombo_->currentData().toString();
    const QString permission = AICommandValidator::normalizePermissionProfile(
        aiPermissionModeCombo_->currentData().toString());
    const auto selection = AICommandCatalog::selectTools(profile, permission);
    if (!selection.valid)
    {
        aiToolVisibilityLabel_->setText(tr("Unavailable"));
        return;
    }
    aiToolVisibilityLabel_->setText(QString::number(selection.tools.size()));
}

}
