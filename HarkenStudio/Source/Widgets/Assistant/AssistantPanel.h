
#pragma once

#include <QWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QHash>
#include <QTimer>
#include <QComboBox>
#include "../../Core/AppSettings.h"
#include "../../Core/AI/AIStepEvent.h"

namespace VNX {

class AIAssistantService;

class AIMessageWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AIMessageWidget(const QString& text, int fontSize = 13, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    int heightForContentWidth(int width) const;

    void setFontSize(int fontSize);

private:
    void setupUI(const QString& text, int fontSize);

    QLabel* contentLabel_;
    int fontSize_ = 13;
};

class UserMessageWidget : public QWidget
{
    Q_OBJECT
public:
    explicit UserMessageWidget(const QString& text, int fontSize = 13, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    int heightForContentWidth(int width) const;

    void setFontSize(int fontSize);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupUI(const QString& text, int fontSize);

    QFrame* bubbleFrame_ = nullptr;
    QLabel* contentLabel_;
    int fontSize_ = 13;
};

class AssistantPanel : public QWidget
{
    Q_OBJECT

public:
    explicit AssistantPanel(QWidget* parent = nullptr);
    ~AssistantPanel() override;

    void addMessage(const QString& text, bool isUser);
    void setAssistantService(AIAssistantService* service);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

public slots:
    void onResponseReady(const QString& requestId, const QString& response);
    void onTurnFailed(const QString& requestId, const QString& code,
                      const QString& message);
    void onConfirmationRequired(const QString& requestId, const QJsonObject& command);
    void onServiceEvent(const AIStepEvent& event);
    void onServiceDiagnostic(const AIServiceDiagnostic& diagnostic);
    void onConversationCleared();
    void onConversationRestored(const QJsonArray& messages);
    void onConversationSessionsChanged(
        const QJsonArray& sessions,
        const QString& activeSessionId);

private slots:
    void onSendClicked();
    void onNewConversationClicked();
    void onConversationSelectionChanged(int index);
    void onFontSettingsChanged(PanelId panelId, int size);

private:
    void setupUI();
    void applyFontSettings();
    void refreshMessages();
    void relayoutMessages();
    void applyNewConversationButtonStyle();
    void updateInputFrameStyle(bool focused);
    void updateEmptyState();
    void applyEmptyLabelStyle();
    void removeThinkingPlaceholder();
    void onStopClicked();
    void setTurnActive(bool active);
    void renderConfirmationCard(const QString& requestId, const QJsonObject& command);
    void resolveConfirmation(const QString& requestId, bool approved);
    void clearConfirmationCards(const QString& requestId);
    void resetServiceUiState();
    void clearStepStatus(const QString& expectedText);
    void scheduleMessageRelayout();

private:
    static constexpr int INPUT_MAX_WIDTH = 520;

    QListWidget* chatList_;
    QWidget* emptyState_;
    QLabel* emptyIcon_;
    QLabel* emptyLabel_;
    QFrame* inputFrame_;
    QLineEdit* inputEdit_;
    QPushButton* sendBtn_;
    QPushButton* newConversationBtn_;
    QComboBox* conversationSelector_;
    QLabel* stepStatusLabel_ = nullptr;
    AIAssistantService* aiService_ = nullptr;
    QList<QMetaObject::Connection> serviceConnections_;
    QTimer* statusClearTimer_ = nullptr;
    QString trackedRequestId_;
    int currentFontSize_ = 13;
    bool turnActive_ = false;
    bool batchingMessages_ = false;
    bool failedStepSeen_ = false;
    bool inServiceCall_ = false;
    bool updatingConversationSelector_ = false;
    
    struct MessageData {
        QString text;
        bool isUser;
    };
    QVector<MessageData> messageHistory_;
    QHash<QString, QJsonObject> liveConfirmationCards_;
};

}
