
#include "AssistantPanel.h"
#include "../../Core/AI/AIAssistantService.h"
#include "../../Core/AI/AIMarkdownRenderer.h"
#include "../../Core/AppSettings.h"
#include "../../Core/ThemeManager.h"
#include "../../Core/QtControlSkin.h"
#include "../../Core/FontManager.h"
#include "../../Core/ThemeAssetCatalog.h"
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QScrollBar>
#include <QFontMetrics>
#include <QTextDocument>
#include <QtMath>
#include <QJsonObject>
#include <QJsonDocument>
#include <QEvent>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <functional>

namespace VNX {
namespace {

constexpr int kMaximumStatusCharacters = 4096;
constexpr int kViewportFallbackWidth = 300;

QString boundedStatus(const QString& text)
{
    return text.left(kMaximumStatusCharacters);
}

void applyMessageLabelStyle(QLabel* label, bool user, int fontSize)
{
    QFont font(FontManager::instance().sansFamily());
    font.setPixelSize(fontSize);
    label->setFont(font);
    label->setObjectName(user ? QStringLiteral("aiUserMessageContent")
                              : QStringLiteral("aiMessageContent"));
}

class AIConfirmationWidget : public QWidget
{
public:
    AIConfirmationWidget(const QJsonObject& command,
                         std::function<void(bool)> resolve, int fontSize,
                         QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("aiConfirmationCard"));
        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 10, 8, 10);
        layout->setSpacing(8);

        const QString cmd = command.value(QStringLiteral("cmd")).toString();
        const QString args = QString::fromUtf8(QJsonDocument(
            command.value(QStringLiteral("args")).toObject()).toJson(QJsonDocument::Compact));
        auto* label = new QLabel(QObject::tr("[Permission] %1\nArguments: %2")
                                     .arg(cmd, boundedStatus(args)), this);
        label->setObjectName(QStringLiteral("aiConfirmationText"));
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont font(FontManager::instance().sansFamily());
        font.setPixelSize(fontSize);
        label->setFont(font);
        layout->addWidget(label);

        QHBoxLayout* buttons = new QHBoxLayout();
        auto* approve = new QPushButton(QObject::tr("Approve"), this);
        approve->setObjectName(QStringLiteral("aiConfirmationApprove"));
        auto* reject = new QPushButton(QObject::tr("Reject"), this);
        reject->setObjectName(QStringLiteral("aiConfirmationReject"));
        buttons->addWidget(approve);
        buttons->addWidget(reject);
        buttons->addStretch();
        layout->addLayout(buttons);

        const auto resolveOnce = [resolve = std::move(resolve), approve, reject](bool approved) {
            if (!approve->isEnabled() && !reject->isEnabled())
                return;
            approve->setEnabled(false);
            reject->setEnabled(false);
            resolve(approved);
        };
        connect(approve, &QPushButton::clicked, this,
                [resolveOnce]() { resolveOnce(true); });
        connect(reject, &QPushButton::clicked, this,
                [resolveOnce]() { resolveOnce(false); });
    }
};

}

AIMessageWidget::AIMessageWidget(const QString& text, int fontSize, QWidget* parent)
    : QWidget(parent), fontSize_(fontSize)
{
    setObjectName("AIMessageWidget");
    setupUI(text, fontSize);
}

void AIMessageWidget::setupUI(const QString& text, int fontSize)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 10, 8, 10);
    mainLayout->setSpacing(10);

    contentLabel_ = new QLabel();
    contentLabel_->setTextFormat(Qt::RichText);
    contentLabel_->setText(AIMarkdownRenderer::renderToHtml(
        text, ThemeManager::instance().colors().borderSecondary));
    contentLabel_->setWordWrap(true);
    contentLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLabel_->setOpenExternalLinks(false);
    applyMessageLabelStyle(contentLabel_, false, fontSize);
    mainLayout->addWidget(contentLabel_);

    QFrame* separator = new QFrame();
    separator->setObjectName(QStringLiteral("aiMessageSeparator"));
    separator->setFrameShape(QFrame::HLine);
    mainLayout->addWidget(separator);
}

int AIMessageWidget::heightForContentWidth(int width) const
{
    QTextDocument doc;
    doc.setDocumentMargin(0);
    // Match the label stylesheet, which sizes the font in pixels; the
    // FontManager point-size path would diverge from it under DPI scaling.
    QFont font(FontManager::instance().sansFamily());
    font.setPixelSize(fontSize_);
    doc.setDefaultFont(font);
    doc.setHtml(contentLabel_->text());
    doc.setTextWidth(qMax(100, width - 16));
    const int textHeight = qCeil(doc.size().height());

    int height = 10 + textHeight + 10 + 1 + 10 + 8;
    return qMax(40, height);
}

QSize AIMessageWidget::sizeHint() const
{
    int w = width() > 1 ? width()
                        : (parentWidget() ? parentWidget()->width() : kViewportFallbackWidth);
    return QSize(w, heightForContentWidth(w));
}

void AIMessageWidget::setFontSize(int fontSize)
{
    fontSize_ = fontSize;
    applyMessageLabelStyle(contentLabel_, false, fontSize);
}

UserMessageWidget::UserMessageWidget(const QString& text, int fontSize, QWidget* parent)
    : QWidget(parent), fontSize_(fontSize)
{
    setObjectName("UserMessageWidget");
    setupUI(text, fontSize);
}

namespace {
// Bubble geometry, shared by layout and height measurement so the two never
// diverge. The rounded background and its inset live on the frame's layout
// margins (measurable) instead of the label's stylesheet padding, which the
// height measurement could not see and which caused the bubble to crop wrapped
// or CJK text. The label itself has no padding, so its heightForWidth is exact.
constexpr int kUserBubbleOuterMargin = 8;   // gap around the bubble in the row
constexpr int kUserBubblePadH = 12;         // frame inner horizontal padding
constexpr int kUserBubblePadV = 8;          // frame inner vertical padding
constexpr double kUserBubbleWidthRatio = 0.78;

int userBubbleFrameMaxWidth(int rowWidth)
{
    return qMax(80, int(rowWidth * kUserBubbleWidthRatio));
}
int userBubbleLabelWidth(int rowWidth)
{
    return qMax(40, userBubbleFrameMaxWidth(rowWidth) - kUserBubblePadH * 2);
}
} // namespace

void UserMessageWidget::setupUI(const QString& text, int fontSize)
{
    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(kUserBubbleOuterMargin, kUserBubbleOuterMargin,
                                   kUserBubbleOuterMargin, kUserBubbleOuterMargin);
    mainLayout->addStretch();

    bubbleFrame_ = new QFrame();
    bubbleFrame_->setObjectName(QStringLiteral("userBubbleFrame"));
    QHBoxLayout* frameLayout = new QHBoxLayout(bubbleFrame_);
    // Padding as layout margins, not stylesheet padding, so it is included in
    // the measured height.
    frameLayout->setContentsMargins(kUserBubblePadH, kUserBubblePadV,
                                    kUserBubblePadH, kUserBubblePadV);
    frameLayout->setSpacing(0);

    contentLabel_ = new QLabel(text);
    contentLabel_->setTextFormat(Qt::PlainText);
    contentLabel_->setWordWrap(true);
    contentLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Set the font on the widget (not only via stylesheet px) so the height
    // measurement below uses the exact same metrics the label paints with.
    {
        QFont labelFont(FontManager::instance().sansFamily());
        labelFont.setPixelSize(fontSize);
        contentLabel_->setFont(labelFont);
    }
    QSizePolicy labelPolicy = contentLabel_->sizePolicy();
    labelPolicy.setHeightForWidth(true);
    contentLabel_->setSizePolicy(labelPolicy);

    applyMessageLabelStyle(contentLabel_, true, fontSize);

    frameLayout->addWidget(contentLabel_);
    mainLayout->addWidget(bubbleFrame_);
}

void UserMessageWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (bubbleFrame_)
        bubbleFrame_->setMaximumWidth(userBubbleFrameMaxWidth(width()));
    // Pin the label to the exact width the height measurement assumes. Left to
    // its own sizeHint, a wrapped QLabel saturates well below the frame's max
    // width on wide panels, so it wrapped to more lines than heightForContentWidth
    // measured and the bubble cropped. Fixing the width keeps render and
    // measurement on the same geometry.
    if (contentLabel_)
        contentLabel_->setFixedWidth(userBubbleLabelWidth(width()));
}

int UserMessageWidget::heightForContentWidth(int width) const
{
    // Measured with a standalone QTextDocument, not contentLabel_->heightForWidth:
    // this runs while the widget is freshly created and not yet laid out, when a
    // label's own heightForWidth returns a stale/zero-width answer and the bubble
    // crops. The document depends only on font + width, so it is correct at any
    // time. All padding is layout margin (frame + row), so the inner text width
    // is exactly frameMaxWidth minus the frame's horizontal margins, and the
    // measured text height is bracketed by those same margins below.
    QFont font(FontManager::instance().sansFamily());
    font.setPixelSize(fontSize_);

    QTextDocument doc;
    doc.setDocumentMargin(0);
    doc.setDefaultFont(font);
    doc.setPlainText(contentLabel_->text());
    doc.setTextWidth(userBubbleLabelWidth(width));
    const int textHeight = qCeil(doc.size().height());

    // One extra line of slack: measuring text height across engines can fall a
    // line short and crop. A little bottom whitespace is invisible; a cropped
    // line is the bug we keep hitting. Always err on the side of too tall.
    const int slack = QFontMetrics(font).lineSpacing();
    const int chrome = kUserBubblePadV * 2 + kUserBubbleOuterMargin * 2;
    return qMax(44, textHeight + slack + chrome);
}

QSize UserMessageWidget::sizeHint() const
{
    int w = width() > 1 ? width()
                        : (parentWidget() ? parentWidget()->width() : kViewportFallbackWidth);
    return QSize(w, heightForContentWidth(w));
}

void UserMessageWidget::setFontSize(int fontSize)
{
    fontSize_ = fontSize;
    applyMessageLabelStyle(contentLabel_, true, fontSize);
}

AssistantPanel::AssistantPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("AssistantPanel");
    currentFontSize_ = AppSettings::instance().panelFontSize(PanelId::Assistant);
    
    setupUI();
    statusClearTimer_ = new QTimer(this);
    statusClearTimer_->setSingleShot(true);
    connect(statusClearTimer_, &QTimer::timeout, this, [this]() {
        clearStepStatus(stepStatusLabel_ ? stepStatusLabel_->text() : QString());
    });
    
    connect(&AppSettings::instance(), &AppSettings::fontSizeChanged,
            this, &AssistantPanel::onFontSettingsChanged);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this]() {
                applyFontSettings();
                update();
            });
}

AssistantPanel::~AssistantPanel() = default;

void AssistantPanel::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    auto* conversationActions = new QHBoxLayout();
    conversationActions->setContentsMargins(0, 0, 0, 0);
    conversationSelector_ = new QComboBox();
    conversationSelector_->setObjectName(
        QStringLiteral("aiConversationSelector"));
    conversationSelector_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    conversationSelector_->setMinimumContentsLength(12);
    conversationSelector_->setToolTip(tr("Switch conversation"));
    connect(
        conversationSelector_,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        &AssistantPanel::onConversationSelectionChanged);
    conversationActions->addWidget(conversationSelector_, 1);
    newConversationBtn_ = new QPushButton();
    newConversationBtn_->setObjectName(QStringLiteral("assistantNewConversation"));
    newConversationBtn_->setFixedSize(28, 28);
    newConversationBtn_->setIconSize(QSize(20, 20));
    newConversationBtn_->setProperty("flatIconButton", true);
    newConversationBtn_->setCursor(Qt::PointingHandCursor);
    newConversationBtn_->setToolTip(tr("Start a new conversation"));
    connect(newConversationBtn_, &QPushButton::clicked,
            this, &AssistantPanel::onNewConversationClicked);
    conversationActions->addWidget(newConversationBtn_);
    mainLayout->addLayout(conversationActions);

    chatList_ = new QListWidget();
    chatList_->setObjectName(QStringLiteral("aiChatList"));
    chatList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    chatList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chatList_->setSelectionMode(QAbstractItemView::NoSelection);
    chatList_->viewport()->installEventFilter(this);
    chatList_->setFocusPolicy(Qt::NoFocus);
    mainLayout->addWidget(chatList_, 1);

    emptyState_ = new QWidget();
    QVBoxLayout* emptyLayout = new QVBoxLayout(emptyState_);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->setSpacing(12);
    emptyLayout->addStretch();

    emptyIcon_ = new QLabel();
    emptyIcon_->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyIcon_);

    emptyLabel_ = new QLabel();
    emptyLabel_->setObjectName("aiEmptyState");
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setWordWrap(true);
    emptyLayout->addWidget(emptyLabel_);

    emptyLayout->addStretch();
    applyEmptyLabelStyle();
    mainLayout->addWidget(emptyState_, 1);

    stepStatusLabel_ = new QLabel();
    stepStatusLabel_->setObjectName(QStringLiteral("aiStepStatus"));
    stepStatusLabel_->setTextFormat(Qt::PlainText);
    stepStatusLabel_->setWordWrap(true);
    stepStatusLabel_->setVisible(false);
    stepStatusLabel_->setProperty("textRole", QStringLiteral("muted"));
    mainLayout->addWidget(stepStatusLabel_);

    inputFrame_ = new QFrame();
    inputFrame_->setObjectName("aiInputFrame");
    inputFrame_->setFixedHeight(44);
    inputFrame_->setMaximumWidth(INPUT_MAX_WIDTH);

    QHBoxLayout* frameLayout = new QHBoxLayout(inputFrame_);
    frameLayout->setContentsMargins(16, 5, 6, 5);
    frameLayout->setSpacing(6);

    inputEdit_ = new QLineEdit();
    inputEdit_->setObjectName("assistantInput");
    inputEdit_->setPlaceholderText(tr("Ask Harken AI..."));
    inputEdit_->installEventFilter(this);
    connect(inputEdit_, &QLineEdit::returnPressed, this, &AssistantPanel::onSendClicked);
    frameLayout->addWidget(inputEdit_, 1);

    sendBtn_ = new QPushButton();
    sendBtn_->setObjectName("assistantSend");
    sendBtn_->setProperty("flatIconButton", true);
    sendBtn_->setFixedSize(32, 32);
    sendBtn_->setIconSize(QSize(20, 20));
    sendBtn_->setCursor(Qt::PointingHandCursor);
    connect(sendBtn_, &QPushButton::clicked, this, &AssistantPanel::onSendClicked);
    frameLayout->addWidget(sendBtn_);

    QHBoxLayout* inputLayout = new QHBoxLayout();
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->addStretch();
    inputLayout->addWidget(inputFrame_, 1);
    inputLayout->addStretch();
    mainLayout->addLayout(inputLayout);

    applyFontSettings();
    updateInputFrameStyle(false);
    applyNewConversationButtonStyle();
    setTurnActive(false);
    updateEmptyState();
}

void AssistantPanel::setAssistantService(AIAssistantService* service)
{
    if (aiService_ == service)
        return;
    if (aiService_)
        aiService_->abortCurrentTurn();
    for (const QMetaObject::Connection& connection : serviceConnections_)
        disconnect(connection);
    serviceConnections_.clear();
    aiService_ = service;
    resetServiceUiState();
    if (!aiService_)
        return;
    serviceConnections_.append(connect(aiService_, &QObject::destroyed, this, [this]() {
        aiService_ = nullptr;
        serviceConnections_.clear();
        resetServiceUiState();
    }));
    serviceConnections_.append(connect(aiService_, &AIAssistantService::responseReady,
                                       this, &AssistantPanel::onResponseReady));
    serviceConnections_.append(connect(aiService_, &AIAssistantService::failed,
                                       this, &AssistantPanel::onTurnFailed));
    serviceConnections_.append(connect(aiService_, &AIAssistantService::confirmationRequired,
                                       this, &AssistantPanel::onConfirmationRequired));
    serviceConnections_.append(connect(aiService_, &AIAssistantService::stepEventReady,
                                       this, &AssistantPanel::onServiceEvent));
    serviceConnections_.append(connect(aiService_, &AIAssistantService::diagnosticReady,
                                       this, &AssistantPanel::onServiceDiagnostic));
    serviceConnections_.append(connect(aiService_, &AIAssistantService::conversationCleared,
                                       this, &AssistantPanel::onConversationCleared));
    serviceConnections_.append(connect(aiService_, &AIAssistantService::conversationRestored,
                                       this, &AssistantPanel::onConversationRestored));
    serviceConnections_.append(connect(
        aiService_,
        &AIAssistantService::conversationSessionsChanged,
        this,
        &AssistantPanel::onConversationSessionsChanged));
    onConversationSessionsChanged(
        aiService_->conversationSessions(),
        aiService_->activeConversationSessionId());
    const QJsonArray restored = aiService_->conversationHistory();
    if (!restored.isEmpty())
        onConversationRestored(restored);
}

void AssistantPanel::updateEmptyState()
{
    const bool empty = messageHistory_.isEmpty() && liveConfirmationCards_.isEmpty();
    emptyState_->setVisible(empty);
    chatList_->setVisible(!empty);
}

void AssistantPanel::applyEmptyLabelStyle()
{
    const auto& tc = ThemeManager::instance().colors();
    emptyIcon_->setPixmap(ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AssistantEmpty),
        ThemeAssetVariant{QSize(), 1.0, 0, tc.textMuted}).icon.pixmap(30, 30));
    emptyLabel_->setProperty("textScale", QStringLiteral("body"));
    emptyLabel_->setText(tr("Chat with Harken AI..."));
}

void AssistantPanel::updateInputFrameStyle(bool focused)
{
    QtControlSkin::setSemanticProperty(inputFrame_, "focused", focused);
}

void AssistantPanel::addMessage(const QString& text, bool isUser)
{
    messageHistory_.append({text, isUser});
    updateEmptyState();

    QListWidgetItem* item = new QListWidgetItem(chatList_);

    int viewportWidth = chatList_->viewport()->width();
    if (viewportWidth < 150)
        viewportWidth = kViewportFallbackWidth;

    QWidget* messageWidget = nullptr;
    int widgetHeight = 60;

    if (isUser) {
        UserMessageWidget* userWidget = new UserMessageWidget(text, currentFontSize_);
        messageWidget = userWidget;
        widgetHeight = userWidget->heightForContentWidth(viewportWidth);
    } else {
        AIMessageWidget* aiWidget = new AIMessageWidget(text, currentFontSize_);
        messageWidget = aiWidget;
        widgetHeight = aiWidget->heightForContentWidth(viewportWidth);
    }

    item->setSizeHint(QSize(viewportWidth, widgetHeight));
    chatList_->setItemWidget(item, messageWidget);

    if (!batchingMessages_)
        scheduleMessageRelayout();
}

void AssistantPanel::scheduleMessageRelayout()
{
    QTimer::singleShot(50, this, [this]() {
        relayoutMessages();
        if (chatList_)
            chatList_->scrollToBottom();
    });
}

void AssistantPanel::onSendClicked()
{
    if (turnActive_)
    {
        onStopClicked();
        return;
    }

    QString text = inputEdit_->text().trimmed();
    if (text.isEmpty()) return;

    if (!aiService_)
    {
        addMessage(boundedStatus(tr("[Unavailable] AI assistant service is not available.")),
                   false);
        return;
    }

    addMessage(text, true);
    inputEdit_->clear();
    addMessage(tr("Thinking..."), false);
    setTurnActive(true);
    {
        QScopedValueRollback<bool> guard(inServiceCall_, true);
        aiService_->submitUserMessage(text);
    }
}

void AssistantPanel::onStopClicked()
{
    if (!aiService_ || !turnActive_)
        return;
    {
        QScopedValueRollback<bool> guard(inServiceCall_, true);
        aiService_->abortCurrentTurn();
    }
    setTurnActive(false);
    removeThinkingPlaceholder();
    addMessage(boundedStatus(tr("[Aborted] The current AI operation has stopped.")), false);
}

void AssistantPanel::onNewConversationClicked()
{
    if (aiService_)
    {
        QScopedValueRollback<bool> guard(inServiceCall_, true);
        aiService_->createConversationSession();
        return;
    }
    onConversationCleared();
}
void AssistantPanel::onConversationSelectionChanged(int index)
{
    if (updatingConversationSelector_ || !aiService_
        || index < 0)
    {
        return;
    }
    const QString sessionId =
        conversationSelector_->itemData(index).toString();
    if (sessionId.isEmpty()
        || sessionId
            == aiService_->activeConversationSessionId())
    {
        return;
    }
    QScopedValueRollback<bool> guard(inServiceCall_, true);
    aiService_->switchConversationSession(sessionId);
}

void AssistantPanel::onConversationCleared()
{
    resetServiceUiState();
    messageHistory_.clear();
    liveConfirmationCards_.clear();
    chatList_->clear();
    updateEmptyState();
}

void AssistantPanel::onConversationRestored(const QJsonArray& messages)
{
    onConversationCleared();
    batchingMessages_ = true;
    for (const QJsonValue& value : messages)
    {
        const QJsonObject message = value.toObject();
        const QString role = message.value(QStringLiteral("role")).toString();
        addMessage(message.value(QStringLiteral("content")).toString(),
                   role == QLatin1String("user"));
    }
    batchingMessages_ = false;
    scheduleMessageRelayout();
    updateEmptyState();
}
void AssistantPanel::onConversationSessionsChanged(
    const QJsonArray& sessions,
    const QString& activeSessionId)
{
    QScopedValueRollback<bool> guard(
        updatingConversationSelector_, true);
    const QSignalBlocker blocker(conversationSelector_);
    conversationSelector_->clear();
    int activeIndex = -1;
    for (const QJsonValue& value : sessions)
    {
        const QJsonObject session = value.toObject();
        const QString id =
            session.value(QStringLiteral("id")).toString();
        const QString title =
            session.value(QStringLiteral("title")).toString();
        conversationSelector_->addItem(title, id);
        if (id == activeSessionId)
            activeIndex = conversationSelector_->count() - 1;
    }
    conversationSelector_->setCurrentIndex(activeIndex);
    conversationSelector_->setEnabled(
        !turnActive_ && conversationSelector_->count() > 0);
}

void AssistantPanel::setTurnActive(bool active)
{
    turnActive_ = active;
    const auto& tc = ThemeManager::instance().colors();
    sendBtn_->setIcon(ThemeManager::instance().resolveIcon(
        QString::fromLatin1(active ? ThemeAssetIds::AssistantStop
                                  : ThemeAssetIds::AssistantSend),
        ThemeAssetVariant{QSize(), 1.0, 0, active ? tc.accentText : tc.textPrimary}).icon);
    sendBtn_->setToolTip(active ? tr("Stop the current AI operation") : tr("Send"));
    QtControlSkin::setSemanticProperty(sendBtn_, "turnActive", active);
    inputEdit_->setEnabled(!active);
    newConversationBtn_->setEnabled(!active);
    conversationSelector_->setEnabled(
        !active && conversationSelector_->count() > 0);
    inputEdit_->setPlaceholderText(active
        ? tr("AI is working... click the stop button to abort")
        : tr("Ask Harken AI..."));
}

void AssistantPanel::removeThinkingPlaceholder()
{
    for (int i = messageHistory_.size() - 1; i >= 0; --i)
    {
        const auto& m = messageHistory_[i];
        if (!m.isUser && (m.text == tr("Thinking...") || m.text == QStringLiteral("Thinking...")))
        {
            messageHistory_.removeAt(i);
            delete chatList_->takeItem(i);
            return;
        }
    }
}

void AssistantPanel::onResponseReady(const QString& requestId, const QString& response)
{
    if (trackedRequestId_.isEmpty() ? !inServiceCall_ : requestId != trackedRequestId_)
        return;
    trackedRequestId_.clear();
    failedStepSeen_ = false;
    clearConfirmationCards(requestId);
    if (stepStatusLabel_)
        stepStatusLabel_->setVisible(false);
    setTurnActive(false);
    removeThinkingPlaceholder();
    addMessage(response, false);
}

void AssistantPanel::onTurnFailed(const QString& requestId, const QString& code,
                                    const QString& message)
{
    const bool accepted = trackedRequestId_.isEmpty()
        ? inServiceCall_
        : requestId == trackedRequestId_ && (failedStepSeen_ || inServiceCall_);
    if (!accepted)
        return;
    trackedRequestId_.clear();
    failedStepSeen_ = false;
    clearConfirmationCards(requestId);
    if (stepStatusLabel_)
        stepStatusLabel_->setVisible(false);
    setTurnActive(false);
    removeThinkingPlaceholder();
    addMessage(boundedStatus(tr("[Error] %1: %2").arg(code, message)), false);
}

void AssistantPanel::clearStepStatus(const QString& expectedText)
{
    if (stepStatusLabel_ && stepStatusLabel_->text() == expectedText)
    {
        stepStatusLabel_->clear();
        stepStatusLabel_->setVisible(false);
    }
}

void AssistantPanel::onServiceEvent(const AIStepEvent& event)
{
    if (trackedRequestId_.isEmpty())
    {
        if (!inServiceCall_)
            return;
        trackedRequestId_ = event.requestId;
        failedStepSeen_ = false;
    }
    else if (event.requestId != trackedRequestId_)
    {
        return;
    }

    switch (event.phase)
    {
    case AIStepPhase::ModelWaiting:
    case AIStepPhase::ResumedModelRound:
    case AIStepPhase::ToolExecution:
        setTurnActive(true);
        break;
    case AIStepPhase::ConfirmationWaiting:
        setTurnActive(false);
        break;
    case AIStepPhase::Failed:
        failedStepSeen_ = true;
        break;
    case AIStepPhase::Cancelled:
        setTurnActive(false);
        clearConfirmationCards(trackedRequestId_);
        trackedRequestId_.clear();
        failedStepSeen_ = false;
        break;
    case AIStepPhase::Completed:
        break;
    }

    if (!stepStatusLabel_)
        return;

    QString text;
    switch (event.phase)
    {
    case AIStepPhase::ModelWaiting:
        text = tr("AI is waiting for the model...");
        break;
    case AIStepPhase::ResumedModelRound:
        text = tr("AI is continuing with the model...");
        break;
    case AIStepPhase::ToolExecution:
        text = tr("Running tool: %1").arg(event.command);
        break;
    case AIStepPhase::ConfirmationWaiting:
        text = tr("Waiting for confirmation: %1").arg(event.command);
        break;
    case AIStepPhase::Completed:
        text = tr("AI completed.");
        break;
    case AIStepPhase::Failed:
        text = tr("AI request failed.");
        break;
    case AIStepPhase::Cancelled:
        text = tr("AI request cancelled.");
        break;
    }
    if (text.isEmpty())
        return;

    stepStatusLabel_->setText(boundedStatus(text));
    stepStatusLabel_->setVisible(true);
    if (statusClearTimer_)
        statusClearTimer_->start(4000);
}

void AssistantPanel::onServiceDiagnostic(const AIServiceDiagnostic& diagnostic)
{
    if (!stepStatusLabel_)
        return;
    stepStatusLabel_->setText(boundedStatus(diagnostic.message));
    stepStatusLabel_->setVisible(true);
    if (statusClearTimer_)
        statusClearTimer_->start(4000);
}

void AssistantPanel::renderConfirmationCard(const QString& requestId,
                                               const QJsonObject& command)
{
    emptyState_->setVisible(false);
    chatList_->setVisible(true);

    auto* item = new QListWidgetItem(chatList_);
    auto* card = new AIConfirmationWidget(
        command,
        [this, requestId](bool approved) {
            resolveConfirmation(requestId, approved);
        },
        currentFontSize_, chatList_);
    item->setSizeHint(QSize(chatList_->viewport()->width(), card->sizeHint().height()));
    chatList_->setItemWidget(item, card);
}

void AssistantPanel::resolveConfirmation(const QString& requestId, bool approved)
{
    if (!liveConfirmationCards_.remove(requestId))
        return;
    refreshMessages();
    if (aiService_)
    {
        QScopedValueRollback<bool> guard(inServiceCall_, true);
        aiService_->resolveConfirmation(requestId, approved);
    }
}

void AssistantPanel::clearConfirmationCards(const QString& requestId)
{
    if (liveConfirmationCards_.remove(requestId))
        refreshMessages();
}

void AssistantPanel::resetServiceUiState()
{
    trackedRequestId_.clear();
    failedStepSeen_ = false;
    if (statusClearTimer_)
        statusClearTimer_->stop();
    if (stepStatusLabel_)
    {
        stepStatusLabel_->clear();
        stepStatusLabel_->setVisible(false);
    }
    if (turnActive_)
        setTurnActive(false);
    removeThinkingPlaceholder();
    if (!liveConfirmationCards_.isEmpty())
    {
        liveConfirmationCards_.clear();
        refreshMessages();
    }
}

void AssistantPanel::onConfirmationRequired(const QString& requestId,
                                               const QJsonObject& command)
{
    trackedRequestId_ = requestId;
    failedStepSeen_ = false;
    setTurnActive(false);
    removeThinkingPlaceholder();
    liveConfirmationCards_.insert(requestId, command);
    renderConfirmationCard(requestId, command);
    chatList_->scrollToBottom();
}

void AssistantPanel::onFontSettingsChanged(PanelId panelId, int size)
{
    if (panelId == PanelId::Assistant)
    {
        currentFontSize_ = size;
        applyFontSettings();
    }
}

void AssistantPanel::applyFontSettings()
{
    QFont inputFont(FontManager::instance().sansFamily());
    inputFont.setPixelSize(currentFontSize_);
    inputEdit_->setFont(inputFont);
    updateInputFrameStyle(inputEdit_->hasFocus());
    applyNewConversationButtonStyle();
    setTurnActive(turnActive_);
    applyEmptyLabelStyle();

    refreshMessages();
}

void AssistantPanel::applyNewConversationButtonStyle()
{
    const auto& tc = ThemeManager::instance().colors();
    newConversationBtn_->setIcon(ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AssistantNewConversation),
        ThemeAssetVariant{QSize(), 1.0, 0, tc.textSecondary}).icon);
}

void AssistantPanel::refreshMessages()
{
    QVector<MessageData> messages = messageHistory_;
    batchingMessages_ = true;
    chatList_->clear();
    messageHistory_.clear();
    
    for (const auto& msg : messages)
    {
        addMessage(msg.text, msg.isUser);
    }
    for (auto it = liveConfirmationCards_.cbegin(); it != liveConfirmationCards_.cend(); ++it)
        renderConfirmationCard(it.key(), it.value());
    batchingMessages_ = false;
    scheduleMessageRelayout();
    updateEmptyState();
}

bool AssistantPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (chatList_ && watched == chatList_->viewport()
        && event->type() == QEvent::Resize) {
        relayoutMessages();
    }
    if (inputEdit_ && watched == inputEdit_) {
        if (event->type() == QEvent::FocusIn)
            updateInputFrameStyle(true);
        else if (event->type() == QEvent::FocusOut)
            updateInputFrameStyle(false);
    }
    return QWidget::eventFilter(watched, event);
}

void AssistantPanel::relayoutMessages()
{
    if (!chatList_)
        return;
    const int w = chatList_->viewport()->width();
    if (w <= 0)
        return;

    for (int i = 0; i < chatList_->count(); ++i) {
        QListWidgetItem* item = chatList_->item(i);
        if (!item)
            continue;
        QWidget* mw = chatList_->itemWidget(item);
        int h = 0;
        if (auto* ai = qobject_cast<AIMessageWidget*>(mw))
            h = ai->heightForContentWidth(w);
        else if (auto* usr = qobject_cast<UserMessageWidget*>(mw))
            h = usr->heightForContentWidth(w);
        else if (mw)
            h = mw->sizeHint().height();
        if (h > 0 && item->sizeHint() != QSize(w, h))
            item->setSizeHint(QSize(w, h));
    }
}

}
