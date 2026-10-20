#include "AI/AIAssistantService.h"
#include "AI/AIConversationSessionStore.h"
#include "AI/AIConversationStore.h"
#include "Support/AITestSupport.h"
#include "Support/MockLLMProvider.h"
#include "Widgets/Assistant/AssistantPanel.h"

#include <gtest/gtest.h>

#include <QFocusEvent>
#include <QFrame>
#include <QJsonObject>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

using namespace VNX;

namespace {

QStringList renderedMessages(AssistantPanel& panel)
{
    QStringList messages;
    auto* list = panel.findChild<QListWidget*>();
    if (!list)
        return messages;

    for (int i = 0; i < list->count(); ++i)
    {
        QWidget* widget = list->itemWidget(list->item(i));
        if (widget)
        {
            if (auto* label = widget->findChild<QLabel*>())
                messages.append(label->text());
        }
    }
    return messages;
}

int visibleConfirmationCardCount(AssistantPanel& panel)
{
    auto* list = panel.findChild<QListWidget*>();
    if (!list)
        return 0;

    int count = 0;
    for (int i = 0; i < list->count(); ++i)
    {
        QWidget* widget = list->itemWidget(list->item(i));
        if (widget && widget->objectName() == QLatin1String("aiConfirmationCard"))
            ++count;
    }
    return count;
}

QPushButton* confirmationCardButton(AssistantPanel& panel, const QString& name)
{
    auto* list = panel.findChild<QListWidget*>();
    if (!list)
        return nullptr;

    for (int i = 0; i < list->count(); ++i)
    {
        QWidget* widget = list->itemWidget(list->item(i));
        if (widget && widget->objectName() == QLatin1String("aiConfirmationCard"))
            return widget->findChild<QPushButton*>(name);
    }
    return nullptr;
}

QJsonObject setItemVolumeArguments()
{
    return QJsonObject{
        {QStringLiteral("object_id"), 7},
        {QStringLiteral("property"), QStringLiteral("volume")},
        {QStringLiteral("value"), -6.0}
    };
}

void wirePanel(AssistantPanel& panel, AIAssistantService& service)
{
    panel.setAssistantService(&service);
}

void configureValidSettings(AIAssistantService& service)
{
    service.setUnrestrictedMode(false);
    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        return defaultValue;
    });
}

class ScopedPanelFontSizeRestore
{
public:
    ScopedPanelFontSizeRestore(AppSettings& settings, int originalSize)
        : settings_(settings), originalSize_(originalSize)
    {
    }

    ~ScopedPanelFontSizeRestore()
    {
        settings_.setPanelFontSize(PanelId::Assistant, originalSize_);
    }

private:
    AppSettings& settings_;
    int originalSize_;
};

class AssistantPanelWiringTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
    }
};

}

TEST_F(AssistantPanelWiringTest, VisualStateUsesApplicationSkinProperties)
{
    AssistantPanel panel;
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* frame = panel.findChild<QFrame*>(QStringLiteral("aiInputFrame"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    auto* list = panel.findChild<QListWidget*>(QStringLiteral("aiChatList"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(send, nullptr);
    ASSERT_NE(list, nullptr);
    EXPECT_TRUE(panel.styleSheet().isEmpty());
    EXPECT_TRUE(input->styleSheet().isEmpty());
    EXPECT_TRUE(frame->styleSheet().isEmpty());
    EXPECT_TRUE(send->styleSheet().isEmpty());
    EXPECT_TRUE(list->styleSheet().isEmpty());
    EXPECT_FALSE(frame->property("focused").toBool());
    EXPECT_FALSE(send->property("turnActive").toBool());

    QFocusEvent focusIn(QEvent::FocusIn);
    QApplication::sendEvent(input, &focusIn);
    EXPECT_TRUE(frame->property("focused").toBool());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto conversationStore =
        std::make_shared<LocalAIConversationStore>(directory.path());
    auto sessionStore =
        std::make_shared<LocalAIConversationSessionStore>(directory.path());
    AIAssistantService service(nullptr);
    service.setConversationStore(conversationStore);
    service.setConversationSessionStore(sessionStore);
    service.setConversationScope(QStringLiteral("project:visual-state"));
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("Later"));
    service.setProvider(provider);
    configureValidSettings(service);
    panel.setAssistantService(&service);
    input->setText(QStringLiteral("Hello"));
    send->click();
    EXPECT_TRUE(send->property("turnActive").toBool());
}

TEST_F(AssistantPanelWiringTest, MissingServiceShowsUnavailableMessage)
{
    AssistantPanel panel;
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();

    EXPECT_EQ(input->text(), QStringLiteral("Hello"));
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, RestoredConversationRendersWhenServiceIsAttached)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto store =
        std::make_shared<LocalAIConversationStore>(directory.path());
    const QString scope = QStringLiteral("project:test");
    const QJsonArray messages{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("content"), QStringLiteral("Stored question")}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                    {QStringLiteral("content"), QStringLiteral("Stored answer")}}
    };
    ASSERT_TRUE(store->save(scope, messages).success);

    AIAssistantService service(nullptr);
    service.setConversationStore(store);
    service.setConversationScope(scope);
    AssistantPanel panel;
    wirePanel(panel, service);

    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Stored question"),
                           QStringLiteral("Stored answer")}));
}

TEST_F(AssistantPanelWiringTest, NewConversationButtonClearsPanelAndServiceContext)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    configureValidSettings(service);
    provider->enqueueTextResponse(QStringLiteral("First answer"));
    provider->enqueueTextResponse(QStringLiteral("Fresh answer"));

    AssistantPanel panel;
    wirePanel(panel, service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    auto* newConversation =
        panel.findChild<QPushButton*>(QStringLiteral("assistantNewConversation"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);
    ASSERT_NE(newConversation, nullptr);

    input->setText(QStringLiteral("First question"));
    send->click();
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("First answer"));
    }));

    newConversation->click();
    EXPECT_TRUE(renderedMessages(panel).isEmpty());

    input->setText(QStringLiteral("Fresh question"));
    send->click();
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("Fresh answer"));
    }));

    const QList<ILLMProvider::Request> requests = provider->submittedRequests();
    ASSERT_EQ(requests.size(), 2);
    ASSERT_EQ(requests[1].messages.size(), 1);
    EXPECT_EQ(requests[1].messages[0].toObject().value(QStringLiteral("content")).toString(),
              QStringLiteral("Fresh question"));
}

TEST_F(AssistantPanelWiringTest,
       NewConversationButtonCreatesAndSelectorSwitchesSessions)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto conversationStore =
        std::make_shared<LocalAIConversationStore>(
            directory.path());
    auto sessionStore =
        std::make_shared<LocalAIConversationSessionStore>(
            directory.path());

    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->enqueueTextResponse(QStringLiteral("First answer"));
    provider->enqueueTextResponse(QStringLiteral("Second answer"));
    service.setProvider(provider);
    service.setConversationStore(conversationStore);
    service.setConversationSessionStore(sessionStore);
    service.setConversationScope(QStringLiteral("project:test"));
    configureValidSettings(service);

    AssistantPanel panel;
    wirePanel(panel, service);
    auto* input = panel.findChild<QLineEdit*>(
        QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(
        QStringLiteral("assistantSend"));
    auto* create = panel.findChild<QPushButton*>(
        QStringLiteral("assistantNewConversation"));
    auto* selector = panel.findChild<QComboBox*>(
        QStringLiteral("aiConversationSelector"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);
    ASSERT_NE(create, nullptr);
    ASSERT_NE(selector, nullptr);
    ASSERT_EQ(selector->count(), 1);

    input->setText(QStringLiteral("First question"));
    send->click();
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(
            QStringLiteral("First answer"));
    }));
    EXPECT_EQ(
        selector->currentText(),
        QStringLiteral("First question"));

    create->click();
    ASSERT_EQ(selector->count(), 2);
    EXPECT_TRUE(renderedMessages(panel).isEmpty());
    input->setText(QStringLiteral("Second question"));
    send->click();
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(
            QStringLiteral("Second answer"));
    }));
    EXPECT_EQ(
        selector->currentText(),
        QStringLiteral("Second question"));

    const int firstIndex = selector->findText(
        QStringLiteral("First question"));
    ASSERT_GE(firstIndex, 0);
    selector->setCurrentIndex(firstIndex);
    EXPECT_EQ(
        renderedMessages(panel),
        (QStringList{
            QStringLiteral("First question"),
            QStringLiteral("First answer")}));
}

TEST_F(AssistantPanelWiringTest, MissingProviderFailureResetsTurnSynchronously)
{
    AIAssistantService service(nullptr);
    AssistantPanel panel;
    wirePanel(panel, service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();

    EXPECT_EQ(renderedMessages(panel),
              (QStringList{
                  QStringLiteral("Hello"),
                  QStringLiteral("[Error] PROVIDER_NOT_FOUND: No LLM provider is configured.")
              }));
    auto* list = panel.findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    for (int i = 0; i < list->count(); ++i)
    {
        QWidget* widget = list->itemWidget(list->item(i));
        ASSERT_NE(widget, nullptr);
        auto* label = widget->findChild<QLabel*>();
        ASSERT_NE(label, nullptr);
        if (widget->objectName() == QLatin1String("UserMessageWidget"))
            EXPECT_EQ(label->textFormat(), Qt::PlainText);
        else
            EXPECT_EQ(label->textFormat(), Qt::RichText);
    }
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, InvalidConfigurationGuidesUserToAIAgentSettings)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    service.setProvider(provider);
    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("not a url"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        return defaultValue;
    });

    AssistantPanel panel;
    wirePanel(panel, service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();

    EXPECT_EQ(
        renderedMessages(panel),
        (QStringList{
            QStringLiteral("Hello"),
            QStringLiteral(
                "[Error] INVALID_ARGUMENTS: The AI Base URL must use HTTPS and the model must be set. "
                "Open File &gt; Preferences &gt; AI Agent to review the Base URL and model, then try again.")
        }));
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, AIMessageRendersMarkdownRichTextUserStaysPlain)
{
    AssistantPanel panel;
    const QString raw = QStringLiteral("**bold** & <script>alert(1)</script>");
    panel.addMessage(raw, false);
    panel.addMessage(raw, true);

    auto* list = panel.findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 2);

    QWidget* aiWidget = list->itemWidget(list->item(0));
    ASSERT_NE(aiWidget, nullptr);
    EXPECT_EQ(aiWidget->objectName(), QStringLiteral("AIMessageWidget"));
    auto* aiLabel = aiWidget->findChild<QLabel*>();
    ASSERT_NE(aiLabel, nullptr);
    EXPECT_EQ(aiLabel->textFormat(), Qt::RichText);
    EXPECT_FALSE(aiLabel->openExternalLinks());
    EXPECT_EQ(aiLabel->text(), QStringLiteral(
        "<b>bold</b> &amp; &lt;script&gt;alert(1)&lt;/script&gt;"));

    QWidget* userWidget = list->itemWidget(list->item(1));
    ASSERT_NE(userWidget, nullptr);
    EXPECT_EQ(userWidget->objectName(), QStringLiteral("UserMessageWidget"));
    auto* userLabel = userWidget->findChild<QLabel*>();
    ASSERT_NE(userLabel, nullptr);
    EXPECT_EQ(userLabel->textFormat(), Qt::PlainText);
    EXPECT_EQ(userLabel->text(), raw);
}

TEST_F(AssistantPanelWiringTest, RichMessageHeightGrowsWithContent)
{
    AssistantPanel panel;
    panel.addMessage(QStringLiteral("short"), false);
    panel.addMessage(QStringLiteral("# Heading\n- item one\n- item two\n\n"
                                    "paragraph line\n```\ncode\n```"), false);

    auto* list = panel.findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 2);
    auto* shortWidget = qobject_cast<AIMessageWidget*>(list->itemWidget(list->item(0)));
    auto* richWidget = qobject_cast<AIMessageWidget*>(list->itemWidget(list->item(1)));
    ASSERT_NE(shortWidget, nullptr);
    ASSERT_NE(richWidget, nullptr);

    const int shortHeight = shortWidget->heightForContentWidth(300);
    const int richHeight = richWidget->heightForContentWidth(300);
    EXPECT_GT(shortHeight, 0);
    EXPECT_GT(richHeight, shortHeight);
}

TEST_F(AssistantPanelWiringTest, MockTextResponseReplacesThinkingAndResetsTurn)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->enqueueTextResponse(QStringLiteral("Mock response"));
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    wirePanel(panel, service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();

    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("Mock response"));
    }));
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Hello"), QStringLiteral("Mock response")}));
    EXPECT_EQ(provider->submitCount(), 1);
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, ServiceStepEventUpdatesBoundedStatusAndCompletionStillRenders)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("Mock response"));
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    wirePanel(panel, service);

    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    auto* step = panel.findChild<QLabel*>(QStringLiteral("aiStepStatus"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);
    ASSERT_NE(step, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();
    EXPECT_FALSE(step->isHidden());
    EXPECT_EQ(step->textFormat(), Qt::PlainText);
    EXPECT_LE(step->text().size(), 128);

    ASSERT_TRUE(provider->deliverNext());
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("Mock response"));
    }));
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Hello"), QStringLiteral("Mock response")}));
    EXPECT_TRUE(step->isHidden());
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, AbortPendingTurnResetsImmediately)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("Late response"));
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    wirePanel(panel, service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Wait"));
    send->click();
    EXPECT_FALSE(input->isEnabled());

    send->click();

    EXPECT_EQ(renderedMessages(panel),
              (QStringList{
                  QStringLiteral("Wait"),
                  QStringLiteral("[Aborted] The current AI operation has stopped.")
              }));
    EXPECT_EQ(provider->cancelCallCount(), 1);
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, ConfirmationCardShowsCommandAndResolvesOnlyOnce)
{
    AIAssistantService service(nullptr);
    AssistantPanel panel;
    wirePanel(panel, service);

    const QJsonObject command{
        {QStringLiteral("cmd"), QStringLiteral("set_item_property")},
        {QStringLiteral("args"), QJsonObject{
            {QStringLiteral("object_id"), 7},
            {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("value"), -6.0}
        }},
        {QStringLiteral("request_id"), QStringLiteral("ai-0001")}
    };
    panel.onConfirmationRequired(QStringLiteral("ai-0001"), command);

    auto* list = panel.findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 1);
    QWidget* card = list->itemWidget(list->item(0));
    ASSERT_NE(card, nullptr);
    auto* label = card->findChild<QLabel*>(QStringLiteral("aiConfirmationText"));
    auto* reject = card->findChild<QPushButton*>(QStringLiteral("aiConfirmationReject"));
    ASSERT_NE(label, nullptr);
    ASSERT_NE(reject, nullptr);
    EXPECT_EQ(card->objectName(), QStringLiteral("aiConfirmationCard"));
    EXPECT_EQ(label->textFormat(), Qt::PlainText);
    EXPECT_TRUE(label->text().contains(QStringLiteral("set_item_property")));
    EXPECT_TRUE(label->text().contains(QStringLiteral("\"property\":\"volume\"")));

    QString failureCode;
    int failures = 0;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        ++failures;
        failureCode = code;
    });
    reject->click();
    EXPECT_EQ(failures, 1);
    EXPECT_EQ(failureCode, QStringLiteral("AI_CONFIRMATION_EXPIRED"));
    EXPECT_EQ(visibleConfirmationCardCount(panel), 0);
    EXPECT_EQ(list->count(), 1);
    EXPECT_TRUE(renderedMessages(panel).constFirst().contains(
        QStringLiteral("AI_CONFIRMATION_EXPIRED")));
}

TEST_F(AssistantPanelWiringTest, PendingConfirmationSurvivesFontRefreshAndStillResolves)
{
    AIAssistantService service(nullptr);
    AssistantPanel panel;
    wirePanel(panel, service);

    const QJsonObject command{
        {QStringLiteral("cmd"), QStringLiteral("set_item_property")},
        {QStringLiteral("args"), QJsonObject{
            {QStringLiteral("object_id"), 7},
            {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("value"), -6.0}
        }},
        {QStringLiteral("request_id"), QStringLiteral("ai-0001")}
    };
    panel.onConfirmationRequired(QStringLiteral("ai-0001"), command);
    AppSettings& settings = AppSettings::instance();
    const int originalFontSize = settings.panelFontSize(PanelId::Assistant);
    const ScopedPanelFontSizeRestore restoreFontSize(settings, originalFontSize);
    const int refreshedFontSize = originalFontSize == 15 ? 16 : 15;
    settings.setPanelFontSize(PanelId::Assistant, refreshedFontSize);

    auto* list = panel.findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 1);
    QWidget* card = list->itemWidget(list->item(0));
    ASSERT_NE(card, nullptr);
    EXPECT_EQ(card->objectName(), QStringLiteral("aiConfirmationCard"));
    auto* label = card->findChild<QLabel*>(QStringLiteral("aiConfirmationText"));
    auto* approve = card->findChild<QPushButton*>(
        QStringLiteral("aiConfirmationApprove"));
    ASSERT_NE(label, nullptr);
    ASSERT_NE(approve, nullptr);
    EXPECT_EQ(label->textFormat(), Qt::PlainText);
    EXPECT_TRUE(label->text().contains(QStringLiteral("set_item_property")));

    QString failureCode;
    int failures = 0;
    QObject::connect(&service, &AIAssistantService::failed, &service,
                     [&](const QString&, const QString& code, const QString&) {
        ++failures;
        failureCode = code;
    });
    approve->click();
    EXPECT_EQ(failures, 1);
    EXPECT_EQ(failureCode, QStringLiteral("AI_CONFIRMATION_EXPIRED"));
    EXPECT_EQ(visibleConfirmationCardCount(panel), 0);
    ASSERT_EQ(list->count(), 1);
    const QStringList outcome = renderedMessages(panel);
    ASSERT_EQ(outcome.size(), 1);
    EXPECT_TRUE(outcome.constFirst().contains(QStringLiteral("AI_CONFIRMATION_EXPIRED")));

    settings.setPanelFontSize(PanelId::Assistant, originalFontSize);
    EXPECT_EQ(visibleConfirmationCardCount(panel), 0);
    EXPECT_EQ(list->count(), 1);
    EXPECT_EQ(renderedMessages(panel), outcome);
}

TEST_F(AssistantPanelWiringTest, RepeatedSetAssistantServiceDeliversTerminalSignalsOnce)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->enqueueTextResponse(QStringLiteral("Mock response"));
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    panel.setAssistantService(&service);
    panel.setAssistantService(&service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("Mock response"));
    }));
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Hello"), QStringLiteral("Mock response")}));

    input->setText(QStringLiteral("Again"));
    send->click();
    const QString failure = QStringLiteral(
        "[Error] INVALID_RESPONSE: No scripted mock response is available.");
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(failure);
    }));
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Hello"), QStringLiteral("Mock response"),
                           QStringLiteral("Again"), failure}));
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, ReplacedOrClearedServiceNoLongerReachesPanel)
{
    AIAssistantService first(nullptr);
    AIAssistantService second(nullptr);
    AssistantPanel panel;
    panel.setAssistantService(&first);
    panel.setAssistantService(&second);

    first.submitUserMessage(QStringLiteral("Hello"));
    EXPECT_TRUE(renderedMessages(panel).isEmpty());

    panel.setAssistantService(nullptr);
    second.submitUserMessage(QStringLiteral("Hello"));
    EXPECT_TRUE(renderedMessages(panel).isEmpty());
}

TEST_F(AssistantPanelWiringTest, ReconnectCycleDeliversEventsExactlyOnce)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->enqueueTextResponse(QStringLiteral("Mock response"));
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    panel.setAssistantService(&service);
    panel.setAssistantService(nullptr);
    panel.setAssistantService(&service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("Mock response"));
    }));
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Hello"), QStringLiteral("Mock response")}));
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, ReplacingServiceMidTurnAbortsOldTurn)
{
    AIAssistantService first(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("Late response"));
    first.setProvider(provider);
    configureValidSettings(first);
    AIAssistantService second(nullptr);
    AssistantPanel panel;
    panel.setAssistantService(&first);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();
    EXPECT_FALSE(input->isEnabled());

    panel.setAssistantService(&second);
    EXPECT_EQ(provider->cancelCallCount(), 1);
    EXPECT_TRUE(input->isEnabled());
    EXPECT_FALSE(provider->deliverNext());
    EXPECT_EQ(renderedMessages(panel), QStringList{QStringLiteral("Hello")});
    auto* step = panel.findChild<QLabel*>(QStringLiteral("aiStepStatus"));
    ASSERT_NE(step, nullptr);
    EXPECT_TRUE(step->isHidden());
    EXPECT_TRUE(step->text().isEmpty());
}

TEST_F(AssistantPanelWiringTest, ServiceDestructionMidTurnResetsPanelSafely)
{
    auto service = std::make_unique<AIAssistantService>(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("Late response"));
    service->setProvider(provider);
    configureValidSettings(*service);
    AssistantPanel panel;
    panel.setAssistantService(service.get());
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();
    EXPECT_FALSE(input->isEnabled());

    service.reset();
    EXPECT_TRUE(input->isEnabled());
    EXPECT_EQ(renderedMessages(panel), QStringList{QStringLiteral("Hello")});
    auto* step = panel.findChild<QLabel*>(QStringLiteral("aiStepStatus"));
    ASSERT_NE(step, nullptr);
    EXPECT_TRUE(step->isHidden());
    EXPECT_TRUE(step->text().isEmpty());

    input->setText(QStringLiteral("After"));
    send->click();
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{
                  QStringLiteral("Hello"),
                  QStringLiteral("[Unavailable] AI assistant service is not available.")
              }));
    EXPECT_EQ(input->text(), QStringLiteral("After"));
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, SameIdFailureWithoutFailedStepDoesNotEndLiveTurn)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("Mock response"));
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    panel.setAssistantService(&service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();
    EXPECT_FALSE(input->isEnabled());

    panel.onTurnFailed(QStringLiteral("ai-0001"), QStringLiteral("INVALID_ARGUMENTS"),
                       QStringLiteral("An AI turn is already active."));
    EXPECT_FALSE(input->isEnabled());
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Hello"), QStringLiteral("Thinking...")}));

    ASSERT_TRUE(provider->deliverNext());
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("Mock response"));
    }));
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Hello"), QStringLiteral("Mock response")}));
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, ConfirmationApprovalRearmsStopStateUntilCompletion)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueToolCallsResponse({ILLMProvider::ToolCall{
        QStringLiteral("call-1"), QStringLiteral("set_item_property"),
        setItemVolumeArguments()}});
    provider->enqueueTextResponse(QStringLiteral("Volume updated"));
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    panel.setAssistantService(&service);
    panel.setAssistantService(&service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Lower the volume"));
    send->click();
    EXPECT_FALSE(input->isEnabled());

    ASSERT_TRUE(provider->deliverNext());
    EXPECT_EQ(visibleConfirmationCardCount(panel), 1);
    EXPECT_TRUE(input->isEnabled());

    auto* approve = confirmationCardButton(panel, QStringLiteral("aiConfirmationApprove"));
    ASSERT_NE(approve, nullptr);
    approve->click();
    EXPECT_EQ(visibleConfirmationCardCount(panel), 0);
    EXPECT_FALSE(input->isEnabled());

    ASSERT_TRUE(provider->deliverNext());
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("Volume updated"));
    }));
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, ConfirmationRejectionKeepsResumedTurnActive)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueToolCallsResponse({ILLMProvider::ToolCall{
        QStringLiteral("call-1"), QStringLiteral("set_item_property"),
        setItemVolumeArguments()}});
    provider->enqueueTextResponse(QStringLiteral("Understood"));
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    panel.setAssistantService(&service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Lower the volume"));
    send->click();
    ASSERT_TRUE(provider->deliverNext());
    EXPECT_EQ(visibleConfirmationCardCount(panel), 1);
    EXPECT_TRUE(input->isEnabled());

    auto* reject = confirmationCardButton(panel, QStringLiteral("aiConfirmationReject"));
    ASSERT_NE(reject, nullptr);
    reject->click();
    EXPECT_EQ(visibleConfirmationCardCount(panel), 0);
    EXPECT_FALSE(input->isEnabled());

    ASSERT_TRUE(provider->deliverNext());
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("Understood"));
    }));
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, StaleOrForeignFailureDoesNotResetLiveTurn)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("Mock response"));
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    panel.setAssistantService(&service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();
    EXPECT_FALSE(input->isEnabled());

    service.submitUserMessage(QStringLiteral("Intruder"));
    EXPECT_FALSE(input->isEnabled());

    panel.onTurnFailed(QStringLiteral("ai-9999"), QStringLiteral("INTERNAL_ERROR"),
                       QStringLiteral("Foreign failure"));
    EXPECT_FALSE(input->isEnabled());
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Hello"), QStringLiteral("Thinking...")}));

    ASSERT_TRUE(provider->deliverNext());
    ASSERT_TRUE(TestSupport::spinUntil([&]() {
        return renderedMessages(panel).contains(QStringLiteral("Mock response"));
    }));
    EXPECT_EQ(renderedMessages(panel),
              (QStringList{QStringLiteral("Hello"), QStringLiteral("Mock response")}));
    EXPECT_TRUE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, TerminalResponseClearsStaleConfirmationCard)
{
    AIAssistantService service(nullptr);
    AssistantPanel panel;
    panel.setAssistantService(&service);

    const QJsonObject command{
        {QStringLiteral("cmd"), QStringLiteral("set_item_property")},
        {QStringLiteral("args"), setItemVolumeArguments()},
        {QStringLiteral("request_id"), QStringLiteral("ai-0001")}
    };
    panel.onConfirmationRequired(QStringLiteral("ai-0001"), command);
    EXPECT_EQ(visibleConfirmationCardCount(panel), 1);

    panel.onResponseReady(QStringLiteral("ai-0001"), QStringLiteral("All done."));
    EXPECT_EQ(visibleConfirmationCardCount(panel), 0);
    EXPECT_EQ(renderedMessages(panel), QStringList{QStringLiteral("All done.")});

    AppSettings& settings = AppSettings::instance();
    const int originalFontSize = settings.panelFontSize(PanelId::Assistant);
    const ScopedPanelFontSizeRestore restoreFontSize(settings, originalFontSize);
    settings.setPanelFontSize(PanelId::Assistant, originalFontSize == 15 ? 16 : 15);
    EXPECT_EQ(visibleConfirmationCardCount(panel), 0);
    EXPECT_EQ(renderedMessages(panel), QStringList{QStringLiteral("All done.")});
}

TEST_F(AssistantPanelWiringTest, NewSubmitAbortsPendingConfirmationAndClearsCard)
{
    AIAssistantService service(nullptr);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueToolCallsResponse({ILLMProvider::ToolCall{
        QStringLiteral("call-1"), QStringLiteral("set_item_property"),
        setItemVolumeArguments()}});
    service.setProvider(provider);
    configureValidSettings(service);
    AssistantPanel panel;
    panel.setAssistantService(&service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);

    input->setText(QStringLiteral("First"));
    send->click();
    ASSERT_TRUE(provider->deliverNext());
    EXPECT_EQ(visibleConfirmationCardCount(panel), 1);
    EXPECT_TRUE(input->isEnabled());

    input->setText(QStringLiteral("Second"));
    send->click();
    EXPECT_EQ(visibleConfirmationCardCount(panel), 0);
    EXPECT_FALSE(input->isEnabled());
}

TEST_F(AssistantPanelWiringTest, InvalidToolProfileDiagnosticShowsBoundedStatus)
{
    AIAssistantService service(nullptr);
    service.setUnrestrictedMode(false);
    auto* provider = new MockLLMProvider;
    provider->setAutoDelivery(false);
    provider->enqueueTextResponse(QStringLiteral("Text only"));
    service.setProvider(provider);
    service.setSettingsValueLoaderForTesting(
        [](const QString& key, const QVariant& defaultValue) {
        if (key == QLatin1String("base_url"))
            return QVariant(QStringLiteral("https://example.test/v1"));
        if (key == QLatin1String("model"))
            return QVariant(QStringLiteral("test-model"));
        if (key == QLatin1String("tool_profile"))
            return QVariant(QStringLiteral("bogus"));
        return defaultValue;
    });
    AssistantPanel panel;
    panel.setAssistantService(&service);
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("assistantSend"));
    auto* step = panel.findChild<QLabel*>(QStringLiteral("aiStepStatus"));
    ASSERT_NE(input, nullptr);
    ASSERT_NE(send, nullptr);
    ASSERT_NE(step, nullptr);

    input->setText(QStringLiteral("Hello"));
    send->click();
    EXPECT_FALSE(step->isHidden());
}
