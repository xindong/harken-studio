
#include "AssistantPanel.h"
#include "Core/ThemeLayout.h"
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
int viewportFallbackWidth() { return ThemeManager::instance().metrics().panel.preferredWidth; }

QString boundedStatus(const QString& text)
{
    return text.left(kMaximumStatusCharacters);
}

QString messageHtml(const QString& text, const QColor& border)
{
    const auto& fonts = FontManager::instance();
    const auto theme = ThemeManager::instance().snapshot();
    return QStringLiteral("<style>code, pre { %1 } b, strong { font-weight: %2; } "
                          "h1, h2, h3, h4, h5, h6 { %3 }</style>%4")
        .arg(fonts.styleSheet(theme, FontRole::Mono))
        .arg(int(fonts.getFont(theme, FontRole::Body, FontEmphasis::Strong).weight()))
        .arg(fonts.styleSheet(theme, FontRole::Title), AIMarkdownRenderer::renderToHtml(text, border));
}

void applyMessageLabelStyle(QLabel* label, bool user, int fontSize)
{
    const int selectionStart = label->selectionStart();
    const int selectionLength = label->selectedText().size();
    const QFont font = FontManager::instance().getFont(FontRole::Body, fontSize);
    label->setFont(font);
    label->setObjectName(user ? QStringLiteral("aiUserMessageContent")
                              : QStringLiteral("aiMessageContent"));
    if (selectionStart >= 0)
        label->setSelection(selectionStart, selectionLength);
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
        ThemeLayout::margins(layout, ThemeLayout::Space::Small, ThemeLayout::Space::Medium, ThemeLayout::Space::Small, ThemeLayout::Space::Medium);
        ThemeLayout::spacing(layout, ThemeLayout::Space::Small);

        const QString cmd = command.value(QStringLiteral("cmd")).toString();
        const QString args = QString::fromUtf8(QJsonDocument(
            command.value(QStringLiteral("args")).toObject()).toJson(QJsonDocument::Compact));
        label_ = new QLabel(QObject::tr("[Permission] %1\nArguments: %2")
                                     .arg(cmd, boundedStatus(args)), this);
        label_->setObjectName(QStringLiteral("aiConfirmationText"));
        label_->setTextFormat(Qt::PlainText);
        label_->setWordWrap(true);
        label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        setFontSize(fontSize);
        layout->addWidget(label_);

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

    void setFontSize(int fontSize)
    {
        label_->setFont(FontManager::instance().getFont(FontRole::Body, fontSize));
    }

private:
    QLabel* label_ = nullptr;
};

}

AIMessageWidget::AIMessageWidget(const QString& text, int fontSize, QWidget* parent)
    : QWidget(parent), fontSize_(fontSize)
{
    setObjectName("AIMessageWidget");
    setupUI(text, fontSize);
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this,
        [this, text, snapshot = ThemeManager::instance().snapshot()](ThemeSnapshotPtr next) mutable {
            if (snapshot->ui.typography == next->ui.typography
                && snapshot->uiScale == next->uiScale
                && snapshot->ui.colors.borderSecondary == next->ui.colors.borderSecondary)
                return;
            snapshot = std::move(next);
            const auto html = messageHtml(text, snapshot->ui.colors.borderSecondary);
            if (contentLabel_->text() == html) return;
            const int selectionStart = contentLabel_->selectionStart();
            const int selectionLength = contentLabel_->selectedText().size();
            contentLabel_->setText(html);
            if (selectionStart >= 0)
                contentLabel_->setSelection(selectionStart, selectionLength);
        });
}

void AIMessageWidget::setupUI(const QString& text, int fontSize)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    ThemeLayout::margins(mainLayout, ThemeLayout::Space::Small, ThemeLayout::Space::Medium, ThemeLayout::Space::Small, ThemeLayout::Space::Medium);
    ThemeLayout::spacing(mainLayout, ThemeLayout::Space::Medium);

    contentLabel_ = new QLabel();
    contentLabel_->setTextFormat(Qt::RichText);
    contentLabel_->setText(messageHtml(
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
    const QFont font = contentLabel_->font();
    doc.setDefaultFont(font);
    doc.setHtml(contentLabel_->text());
    const auto margins = layout()->contentsMargins();
    doc.setTextWidth(qMax(1, width - margins.left() - margins.right()));
    const int textHeight = qCeil(doc.size().height());

    const auto& metrics = ThemeManager::instance().metrics();
    return textHeight + margins.top() + margins.bottom() + layout()->spacing() + metrics.borderWidth;
}

QSize AIMessageWidget::sizeHint() const
{
    int w = width() > 1 ? width()
                        : (parentWidget() ? parentWidget()->width() : viewportFallbackWidth());
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
constexpr double kUserBubbleWidthRatio = 0.78;

int userBubbleFrameMaxWidth(int rowWidth)
{
    const int available = rowWidth - ThemeManager::instance().metrics().spacingSmall * 2;
    return qMax(1, int(available * kUserBubbleWidthRatio));
}
int userBubbleLabelWidth(int rowWidth)
{
    return qMax(1, userBubbleFrameMaxWidth(rowWidth) - ThemeManager::instance().metrics().spacingMedium * 2);
}
} // namespace

void UserMessageWidget::setupUI(const QString& text, int fontSize)
{
    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    ThemeLayout::margins(mainLayout, ThemeLayout::Space::Small);
    mainLayout->addStretch();

    bubbleFrame_ = new QFrame();
    bubbleFrame_->setObjectName(QStringLiteral("userBubbleFrame"));
    QHBoxLayout* frameLayout = new QHBoxLayout(bubbleFrame_);
    // Padding as layout margins, not stylesheet padding, so it is included in
    // the measured height.
    ThemeLayout::margins(frameLayout, ThemeLayout::Space::Medium, ThemeLayout::Space::Small, ThemeLayout::Space::Medium, ThemeLayout::Space::Small);
    frameLayout->setSpacing(0);

    contentLabel_ = new QLabel(text);
    contentLabel_->setTextFormat(Qt::PlainText);
    contentLabel_->setWordWrap(true);
    contentLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QSizePolicy labelPolicy = contentLabel_->sizePolicy();
    labelPolicy.setHeightForWidth(true);
    contentLabel_->setSizePolicy(labelPolicy);

    applyMessageLabelStyle(contentLabel_, true, fontSize);

    frameLayout->addWidget(contentLabel_);
    mainLayout->addWidget(bubbleFrame_);
    ThemeManager::instance().bindMetrics(this, [this](const ThemeMetrics&) {
        updateBubbleGeometry();
        updateGeometry();
    });
}

void UserMessageWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateBubbleGeometry();
}

void UserMessageWidget::updateBubbleGeometry()
{
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
    const QFont font = contentLabel_->font();

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
    const auto margins = layout()->contentsMargins();
    const auto padding = bubbleFrame_->layout()->contentsMargins();
    return textHeight + slack + margins.top() + margins.bottom() + padding.top() + padding.bottom();
}

QSize UserMessageWidget::sizeHint() const
{
    int w = width() > 1 ? width()
                        : (parentWidget() ? parentWidget()->width() : viewportFallbackWidth());
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

    connect(&FontManager::instance(), &FontManager::fontChanged,
            this, [this]() {
                currentFontSize_ = AppSettings::instance().panelFontSize(PanelId::Assistant);
                applyFontSettings();
                update();
            });
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
            this, &AssistantPanel::applyAppearance);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this] { QTimer::singleShot(0, this, &AssistantPanel::relayoutMessages); });
}

AssistantPanel::~AssistantPanel() = default;

void AssistantPanel::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    ThemeLayout::margins(mainLayout, ThemeLayout::Space::Small);
    ThemeLayout::spacing(mainLayout, ThemeLayout::Space::Small);

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
    ThemeManager::instance().bindMetrics(newConversationBtn_, [this](const ThemeMetrics& metrics) {
        newConversationBtn_->setFixedSize(metrics.control.largeHeight, metrics.control.largeHeight);
    });
    ThemeManager::instance().bindMetrics(newConversationBtn_, [this](const ThemeMetrics& metrics) {
        newConversationBtn_->setIconSize(QSize(metrics.objectIconExtent, metrics.objectIconExtent));
    });
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
    ThemeLayout::spacing(emptyLayout, ThemeLayout::Space::Medium);
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
    ThemeManager::instance().bindMetrics(inputFrame_, [this](const ThemeMetrics& metrics) {
        inputFrame_->setFixedHeight(metrics.control.largeHeight + metrics.spacingLarge);
    });
    ThemeManager::instance().bindMetrics(inputFrame_, [this](const ThemeMetrics& metrics) {
        inputFrame_->setMaximumWidth(metrics.dialog.preferredWidth);
    });

    QHBoxLayout* frameLayout = new QHBoxLayout(inputFrame_);
    ThemeLayout::margins(frameLayout, ThemeLayout::Space::None, ThemeLayout::Space::XSmall, ThemeLayout::Space::Small, ThemeLayout::Space::XSmall);
    ThemeLayout::spacing(frameLayout, ThemeLayout::Space::Small);

    inputEdit_ = new QLineEdit();
    inputEdit_->setObjectName("assistantInput");
    inputEdit_->setPlaceholderText(tr("Ask Harken AI..."));
    inputEdit_->installEventFilter(this);
    connect(inputEdit_, &QLineEdit::returnPressed, this, &AssistantPanel::onSendClicked);
    frameLayout->addWidget(inputEdit_, 1);

    sendBtn_ = new QPushButton();
    sendBtn_->setObjectName("assistantSend");
    sendBtn_->setProperty("flatIconButton", true);
    ThemeManager::instance().bindMetrics(sendBtn_, [this](const ThemeMetrics& metrics) {
        sendBtn_->setFixedSize(metrics.control.largeHeight, metrics.control.largeHeight);
    });
    ThemeManager::instance().bindMetrics(sendBtn_, [this](const ThemeMetrics& metrics) {
        sendBtn_->setIconSize(QSize(metrics.objectIconExtent, metrics.objectIconExtent));
    });
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
    applyAppearance();
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
    const int extent = ThemeManager::instance().metrics().control.largeIconExtent;
    emptyIcon_->setPixmap(ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AssistantEmpty),
        ThemeAssetVariant{QSize(), 1.0, 0, tc.textPrimary}).icon.pixmap(extent, extent));
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
        viewportWidth = viewportFallbackWidth();

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
    refreshSendIcon();
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
    const QFont inputFont = FontManager::instance().getFont(FontRole::Body, currentFontSize_);
    inputEdit_->ensurePolished();
    inputEdit_->setFont(inputFont);

    for (int index = 0; index < chatList_->count(); ++index) {
        auto* widget = chatList_->itemWidget(chatList_->item(index));
        if (auto* message = qobject_cast<AIMessageWidget*>(widget))
            message->setFontSize(currentFontSize_);
        else if (auto* message = qobject_cast<UserMessageWidget*>(widget))
            message->setFontSize(currentFontSize_);
        else if (auto* confirmation = dynamic_cast<AIConfirmationWidget*>(widget))
            confirmation->setFontSize(currentFontSize_);
    }
    QTimer::singleShot(0, this, &AssistantPanel::relayoutMessages);
}

void AssistantPanel::applyAppearance()
{
    applyNewConversationButtonStyle();
    refreshSendIcon();
    applyEmptyLabelStyle();
}

void AssistantPanel::refreshSendIcon()
{
    const auto& colors = ThemeManager::instance().colors();
    sendBtn_->setIcon(ThemeManager::instance().resolveIcon(
        QString::fromLatin1(turnActive_ ? ThemeAssetIds::AssistantStop
                                      : ThemeAssetIds::AssistantSend),
        ThemeAssetVariant{QSize(), 1.0, 0, turnActive_ ? colors.accentText : colors.textPrimary}).icon);
}

void AssistantPanel::applyNewConversationButtonStyle()
{
    const auto& tc = ThemeManager::instance().colors();
    newConversationBtn_->setIcon(ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AssistantNewConversation),
        ThemeAssetVariant{QSize(), 1.0, 0, tc.textPrimary}).icon);
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
    if (!chatList_ || relayoutActive_)
        return;
    const int w = chatList_->viewport()->width();
    if (w <= 0)
        return;

    const QScopedValueRollback<bool> guard(relayoutActive_, true);
    auto* scrollbar = chatList_->verticalScrollBar();
    const bool atBottom = scrollbar->value() == scrollbar->maximum();
    QListWidgetItem* anchor = nullptr;
    qreal offset = 0.0;
    for (int index = 0; !atBottom && index < chatList_->count(); ++index) {
        auto* item = chatList_->item(index);
        const QRect rect = chatList_->visualItemRect(item);
        if (rect.bottom() < 0 || rect.height() <= 0) continue;
        anchor = item;
        offset = qMax(0.0, -qreal(rect.top()) / rect.height());
        break;
    }

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
    chatList_->doItemsLayout();
    if (atBottom)
        scrollbar->setValue(scrollbar->maximum());
    else if (anchor) {
        const QRect rect = chatList_->visualItemRect(anchor);
        scrollbar->setValue(scrollbar->value() + rect.top() + qRound(offset * rect.height()));
    }
}

}
