#pragma once

#include "AICommandValidator.h"
#include "AIAgentConfig.h"
#include "AIDurableMemoryRetrieval.h"
#include "AIStepEvent.h"
#include "AIConversationSessionStore.h"
#include "ILLMProvider.h"
#include "IPC/IPCServer.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <functional>
#include <memory>
#include <optional>

namespace VNX {

class IAIConversationStore;
class AIDurableMemoryService;

class AIAssistantService : public QObject
{
    Q_OBJECT

public:
    using SecretLoader = std::function<QString(const QString&, const QString&)>;
    using SettingsValueLoader = AIAgentConfigStore::ValueLoader;
    using Clock = std::function<qint64()>;

    explicit AIAssistantService(IPCServer* ipc, QObject* parent = nullptr);
    ~AIAssistantService() override;
    void setProvider(ILLMProvider* provider);
    void setUnrestrictedMode(bool enabled);
    bool unrestrictedMode() const;
    void setSecretLoader(SecretLoader loader);
    void setSettingsValueLoaderForTesting(SettingsValueLoader loader);
    void setClockForTesting(Clock clock);
    void setExecutionLimitsForTesting(int maxToolExecutions, int maxModelRounds);
    void setConversationStore(std::shared_ptr<IAIConversationStore> store);
    void setConversationSessionStore(
        std::shared_ptr<IAIConversationSessionStore> store);
    void setDurableMemoryService(
        std::shared_ptr<AIDurableMemoryService> service);
    void setDurableMemoryRetriever(
        std::shared_ptr<IAIDurableMemoryRetriever> retriever);
    void setConversationScope(const QString& scope);
    QJsonArray conversationHistory() const;
    QJsonArray conversationSessions() const;
    QString activeConversationSessionId() const;
    QStringList lastUsedDurableMemoryIds() const;

public slots:
    void submitUserMessage(const QString& text);
    void abortCurrentTurn();
    void clearConversation();
    void createConversationSession();
    void switchConversationSession(const QString& sessionId);
    void resolveConfirmation(const QString& requestId, bool approved);

signals:
    void responseDelta(const QString& requestId, const QString& text);
    void responseReady(const QString& requestId, const QString& text);
    void stepEventReady(const AIStepEvent& event);
    void diagnosticReady(const AIServiceDiagnostic& diagnostic);
    void confirmationRequired(const QString& requestId, const QJsonObject& command);
    void failed(const QString& requestId, const QString& code, const QString& message);
    void conversationCleared();
    void conversationRestored(const QJsonArray& messages);
    void conversationSessionsChanged(
        const QJsonArray& sessions,
        const QString& activeSessionId);

private:
    enum class TurnState { Idle, AwaitingModel, RunningTools, AwaitingConfirmation };

    struct PendingConfirmation
    {
        ILLMProvider::ToolCall call;
        QJsonObject command;
        QString permissionProfile;
        QString toolProfile;
        QByteArray binding;
        qint64 expiresAtMs = 0;
    };

    struct PendingMemoryMutation
    {
        enum class Type
        {
            Upsert,
            Remove,
            Clear
        };

        Type type = Type::Upsert;
        QString scopeName;
        QString scope;
        QString key;
        QString value;
        QString id;
    };

    void connectProvider();
    void handleCompleted(const ILLMProvider::Response& response);
    void submitModelRound();
    void requestForcedSummary();
    void processToolQueue();
    bool executeUnrestrictedCall(const ILLMProvider::ToolCall& call);
    bool executeMemoryCall(const ILLMProvider::ToolCall& call,
                           const QString& command,
                           const QJsonObject& arguments);
    bool commitPendingMemoryMutation(
        QString* errorCode,
        QString* errorMessage);
    void retrieveDurableMemoryContext(const QString& userText);
    QString durableMemoryScope(const QString& scopeName) const;
    QJsonObject describeCommandEnvelope(const QString& name) const;
    bool repeatedReadExceeded(const QString& cmd, const QJsonObject& args);
    bool executeResolvedCall(const ILLMProvider::ToolCall& call,
                             const QString& cmd, const QJsonObject& args,
                             const IPCServer::IPCDispatchContext& context);
    bool appendToolResult(const ILLMProvider::ToolCall& call,
                          const QJsonObject& envelope);
    void finishLegacySuccess();
    void finishWithFailure(const QString& requestId, const QString& code,
                           const QString& message);
    bool fitActiveRequestToContext(
        const QString& baseSystemPrompt,
        const QList<AICommandSpec>& tools,
        QString* fittedSystemPrompt);
    void commitCompletedTurn(const QString& responseText);
    void trimConversationHistory();
    void persistConversationHistory();
    QString conversationStorageScope() const;
    void loadConversationHistory();
    void emitConversationSessions();
    void updateActiveConversationMetadata(
        const QString& firstUserMessage);
    void teardownActiveTurn();
    void clearActiveTurn();
    IPCServer::IPCDispatchContext makeAIDispatchContext(
        const QString& requestId, const QString& permissionProfile,
        const QString& toolProfile) const;
    void resetTurnRuntime(TurnState nextState);
    qint64 nowMs() const;
    void emitStepEvent(AIStepPhase phase, const QString& command = QString(),
                       const QString& requestId = QString());

    IPCServer* ipc_ = nullptr;
    ILLMProvider* provider_ = nullptr;
    AICommandValidator validator_;
    SecretLoader secretLoader_;
    AIAgentConfigStore configStore_;
    std::shared_ptr<IAIConversationStore> conversationStore_;
    std::shared_ptr<IAIConversationSessionStore> conversationSessionStore_;
    std::shared_ptr<AIDurableMemoryService> durableMemoryService_;
    std::shared_ptr<IAIDurableMemoryRetriever> durableMemoryRetriever_;
    Clock clock_;
    quint64 nextRequestNumber_ = 1;
    TurnState turnState_ = TurnState::Idle;
    bool unrestrictedMode_ = true;
    QString activeRequestId_;
    QString activePermissionProfile_;
    QString activeToolProfile_;
    QString activeBaseSystemPrompt_;
    QString activeSystemPrompt_;
    QString conversationScope_;
    AIConversationSessionCatalog conversationSessionCatalog_;
    QJsonArray conversationHistory_;
    QJsonArray transcript_;
    QList<AIDurableMemoryRetrievalItem> activeRetrievedMemories_;
    QStringList activeUsedDurableMemoryIds_;
    QStringList lastUsedDurableMemoryIds_;
    QList<AICommandSpec> activeTools_;
    QList<ILLMProvider::ToolCall> toolQueue_;
    QHash<QString, int> repeatedReads_;
    PendingConfirmation pendingConfirmation_;
    std::optional<PendingMemoryMutation> pendingMemoryMutation_;
    QString activeModel_;
    QString activeApiKey_;
    QUrl activeEndpoint_;
    int activeContextWindowTokens_ = 0;
    int activeMaxTokens_ = 0;
    int activeTimeoutMs_ = 0;
    int activeHistoryMessageCount_ = 0;
    int toolExecutions_ = 0;
    int modelRounds_ = 0;
    // Per-turn work budget. Large enough that ordinary multi-step tasks finish,
    // while still capping runaway loops; the repeated-read and whole-turn-deadline
    // guards remain the primary brakes. Overridable in tests.
    int maxToolExecutions_ = 100;
    int maxModelRounds_ = 100;
    int aggregateResultBytes_ = 0;
    qint64 turnDeadlineMs_ = 0;
    qint64 confirmationWaitStartedMs_ = 0;
    bool legacyOneShot_ = false;
    bool awaitingForcedSummary_ = false;
    QJsonObject lastToolResponse_;
};

}
