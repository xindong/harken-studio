#include "AIAssistantService.h"
#include "AIConversationStore.h"
#include "AIDurableMemoryService.h"
#include "AIEndpointValidation.h"
#include "AIResultNormalizer.h"
#include "ItemTypeCatalog.h"

#include "Platform/SecretStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace VNX {
namespace {
constexpr int kMaximumResponseBytes = 65536;
constexpr int kMaximumErrorCharacters = 4096;
// Guards a single malformed/exploding model response (parallel tool_calls in one
// turn), not overall task length, so it stays small; the per-turn work budget is
// governed by the injectable maxToolExecutions_/maxModelRounds_ members instead.
constexpr int kMaximumCallsPerResponse = 8;
constexpr int kMaximumToolResultBytes = 16384;
constexpr int kMaximumAggregateResultBytes = 65536;
constexpr int kMaximumConversationHistoryBytes =
    AIConversationLimits::kMaximumHistoryBytes;
constexpr int kMaximumHistoryMessageBytes =
    AIConversationLimits::kMaximumMessageBytes;
constexpr int kContextSafetyReserveTokens = 2048;
constexpr int kMaximumDurableMemoryContextBytes = 16384;
constexpr int kMaximumRetrievedMemoryEntries = 32;
constexpr qint64 kWholeTurnTimeoutMs = 120000;
constexpr qint64 kConfirmationTimeoutMs = 300000;
constexpr auto kDefaultConversationSessionId =
    "00000000-0000-4000-8000-000000000001";
constexpr auto kAIAgentPreferencesPath = "File > Preferences > AI Agent";
// The closing summary round runs after the turn budget is spent (often after
// the whole-turn deadline has already passed), so it gets its own small bounded
// window rather than a fresh full request timeout that would keep the UI busy
// for another whole cycle.
constexpr int kForcedSummaryTimeoutMs = 15000;
QString boundedErrorMessage(const QString& message) { return message.left(kMaximumErrorCharacters); }
QString boundedUtf8(const QString& text, int maximumBytes)
{
    if (text.toUtf8().size() <= maximumBytes)
        return text;

    int low = 0;
    int high = text.size();
    while (low < high)
    {
        const int candidate = low + (high - low + 1) / 2;
        if (text.left(candidate).toUtf8().size() <= maximumBytes)
            low = candidate;
        else
            high = candidate - 1;
    }
    return text.left(low);
}
QJsonObject toolCallWire(const ILLMProvider::ToolCall& call)
{
    return QJsonObject{{QStringLiteral("id"), call.callId}, {QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), call.name}, {QStringLiteral("arguments"), QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact))}}}};
}
QJsonObject errorEnvelope(const QString& code, const QString& message)
{
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("error")}, {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}
QJsonObject successEnvelope(const QJsonObject& result)
{
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("result"), result}};
}
QJsonObject conversationSessionWire(
    const AIConversationSessionRecord& session)
{
    return QJsonObject{
        {QStringLiteral("id"), session.id},
        {QStringLiteral("title"), session.title},
        {QStringLiteral("created_at_ms"),
         static_cast<double>(session.createdAtMs)},
        {QStringLiteral("updated_at_ms"),
         static_cast<double>(session.updatedAtMs)}
    };
}
AIConversationSessionOperationResult conversationSessionSuccess()
{
    AIConversationSessionOperationResult result;
    result.success = true;
    return result;
}
AIConversationSessionOperationResult conversationSessionFailure(
    const QString& code,
    const QString& message)
{
    AIConversationSessionOperationResult result;
    result.code = code;
    result.message = message;
    return result;
}
QJsonArray toolBudgetPayload(const QList<AICommandSpec>& specs)
{
    QJsonArray tools;
    for (const AICommandSpec& spec : specs)
    {
        tools.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{
                {QStringLiteral("name"), spec.cmd},
                {QStringLiteral("description"), spec.description},
                {QStringLiteral("parameters"), spec.argumentSchema}
            }}
        });
    }
    return tools;
}
int conservativeInputTokenEstimate(const QString& systemPrompt,
                                   const QJsonArray& messages,
                                   const QList<AICommandSpec>& tools)
{
    QJsonArray wireMessages;
    if (!systemPrompt.isEmpty())
    {
        wireMessages.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("content"), systemPrompt}
        });
    }
    for (const QJsonValue& message : messages)
        wireMessages.append(message);

    const QJsonObject payload{
        {QStringLiteral("messages"), wireMessages},
        {QStringLiteral("tools"), toolBudgetPayload(tools)}
    };
    // Byte-pair tokenizers cannot produce more tokens than the UTF-8 bytes they
    // consume. Counting compact request bytes is a conservative provider-neutral
    // upper bound without binding the editor to one tokenizer.
    return QJsonDocument(payload).toJson(QJsonDocument::Compact).size();
}
AICommandSpec dispatchIpcToolSpec()
{
    AICommandSpec spec;
    spec.cmd = QStringLiteral("dispatch_ipc");
    spec.availability = AICommandSpec::Availability::Enabled;
    spec.effect = AICommandSpec::Effect::ProjectState;
    spec.description = QStringLiteral(
        "Execute any Editor command by name with a JSON arguments object. "
        "The command result or error envelope is returned verbatim.");
    spec.argumentSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("command"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("minLength"), 1},
                {QStringLiteral("description"), QStringLiteral("Editor command name to execute.")}}},
            {QStringLiteral("args"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("description"), QStringLiteral("Arguments for the command; omit for commands without arguments.")}}}
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("command")}},
        {QStringLiteral("additionalProperties"), false}};
    return spec;
}
AICommandSpec describeCommandToolSpec()
{
    AICommandSpec spec;
    spec.cmd = QStringLiteral("describe_command");
    spec.availability = AICommandSpec::Availability::Enabled;
    spec.effect = AICommandSpec::Effect::Query;
    spec.description = QStringLiteral(
        "Look up the parameter format and description of an Editor command before executing it.");
    spec.argumentSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("name"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("minLength"), 1},
                {QStringLiteral("description"), QStringLiteral("Editor command name to describe.")}}}
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("name")}},
        {QStringLiteral("additionalProperties"), false}};
    return spec;
}
QJsonObject memoryScopeProperty()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray{
            QStringLiteral("user"), QStringLiteral("project")}},
        {QStringLiteral("description"), QStringLiteral(
            "Use user for a cross-project preference or project for a fact "
            "that belongs only to the open project.")}};
}
AICommandSpec memoryListToolSpec()
{
    AICommandSpec spec;
    spec.cmd = QStringLiteral("memory_list");
    spec.availability = AICommandSpec::Availability::Enabled;
    spec.effect = AICommandSpec::Effect::Query;
    spec.description = QStringLiteral(
        "List explicitly saved durable memories in one scope.");
    spec.argumentSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("scope"), memoryScopeProperty()}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("scope")}},
        {QStringLiteral("additionalProperties"), false}};
    return spec;
}
AICommandSpec memoryUpsertToolSpec()
{
    AICommandSpec spec;
    spec.cmd = QStringLiteral("memory_upsert");
    spec.availability = AICommandSpec::Availability::Enabled;
    spec.effect = AICommandSpec::Effect::ProjectState;
    spec.description = QStringLiteral(
        "Create or update one durable memory after the designer explicitly "
        "asks you to remember it.");
    spec.argumentSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("scope"), memoryScopeProperty()},
            {QStringLiteral("key"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("minLength"), 1}}},
            {QStringLiteral("value"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("minLength"), 1}}}}},
        {QStringLiteral("required"), QJsonArray{
            QStringLiteral("scope"), QStringLiteral("key"),
            QStringLiteral("value")}},
        {QStringLiteral("additionalProperties"), false}};
    return spec;
}
AICommandSpec memoryRemoveToolSpec()
{
    AICommandSpec spec;
    spec.cmd = QStringLiteral("memory_remove");
    spec.availability = AICommandSpec::Availability::Enabled;
    spec.effect = AICommandSpec::Effect::ProjectState;
    spec.description = QStringLiteral(
        "Remove one durable memory by its stable ID after the designer "
        "explicitly asks you to forget it.");
    spec.argumentSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("scope"), memoryScopeProperty()},
            {QStringLiteral("id"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("minLength"), 1}}}}},
        {QStringLiteral("required"), QJsonArray{
            QStringLiteral("scope"), QStringLiteral("id")}},
        {QStringLiteral("additionalProperties"), false}};
    return spec;
}
AICommandSpec memoryClearToolSpec()
{
    AICommandSpec spec;
    spec.cmd = QStringLiteral("memory_clear");
    spec.availability = AICommandSpec::Availability::Enabled;
    spec.effect = AICommandSpec::Effect::ProjectState;
    spec.description = QStringLiteral(
        "Clear every durable memory in one scope after the designer "
        "explicitly asks you to forget that scope.");
    spec.argumentSchema = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("scope"), memoryScopeProperty()}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("scope")}},
        {QStringLiteral("additionalProperties"), false}};
    return spec;
}
const QList<AICommandSpec>& unrestrictedTools()
{
    static const QList<AICommandSpec> tools{
        dispatchIpcToolSpec(),
        describeCommandToolSpec(),
        memoryListToolSpec(),
        memoryUpsertToolSpec(),
        memoryRemoveToolSpec(),
        memoryClearToolSpec()};
    return tools;
}
QJsonObject memoryEntryWire(const AIDurableMemoryEntry& entry)
{
    return QJsonObject{
        {QStringLiteral("id"), entry.id},
        {QStringLiteral("kind"),
         entry.kind == AIDurableMemoryKind::UserPreference
             ? QStringLiteral("user_preference")
             : QStringLiteral("project_fact")},
        {QStringLiteral("key"), entry.key},
        {QStringLiteral("value"), entry.value},
        {QStringLiteral("source"),
         entry.source == AIDurableMemorySource::User
             ? QStringLiteral("user")
             : QStringLiteral("model")},
        {QStringLiteral("source_reference"), entry.sourceReference},
        {QStringLiteral("created_at_ms"),
         static_cast<double>(entry.createdAtMs)},
        {QStringLiteral("updated_at_ms"),
         static_cast<double>(entry.updatedAtMs)},
        {QStringLiteral("revision"),
         static_cast<double>(entry.revision)}};
}
QString unrestrictedSystemPrompt()
{
    QStringList objectTypes;
    for (const ProjectItem::Type type : ItemTypeCatalog::allTypes()) {
        if (ItemTypeCatalog::isUserFacingType(type))
            objectTypes.push_back(ItemTypeCatalog::canonicalName(type));
    }

    // The command vocabulary is conveyed as a fixed category map rather than a
    // flat list of every dispatchable name: the map has far higher information
    // density per token, and the model resolves a single command's exact
    // parameters on demand through describe_command.
    return QStringLiteral(
        "You are the embedded AI assistant of HarkenStudio, an audio middleware "
        "authoring tool. You execute editor commands on the designer's behalf via "
        "the dispatch_ipc tool, and look up a command's exact parameters with "
        "describe_command when unsure.\n\n"

        "DOMAIN MODEL. The canonical object types are: %1. Folder organizes "
        "non-content areas. Layer organizes and shapes content descendants. Sound "
        "binds media. Container supports Random, Sequence, "
        "Match, and Blend modes. Random and Sequence use Play Type Single for one "
        "selection per trigger or Queue for a full round. Match selects one child by variable values. Blend "
        "starts all table members and combines their gains. Variable Response is "
        "Fixed or Live and never extends content lifetime. Track schedules clips on one lane of absolute "
        "time and can be triggered directly. Piece aligns its Track children to a "
        "shared absolute origin. A clip schedules the start of a Sound, Container, "
        "or Piece; playback length follows its target. Overlap plays content "
        "together without an automatic crossfade. Piece supports Entry, Exit, "
        "and Custom markers. Musical time, tempo, meter, and beat/bar "
        "scheduling cannot be authored in this version. Legacy musical data is "
        "preserved and its affected timeline is read-only. Marker and Transition "
        "execution support depends on the operation and target; consult runtime "
        "capability results. "
        "Bus forms the mixing and routing tree. Action carries ordered Play, Stop, "
        "PostAction, SetVariable, and Reset steps. Variable is a named float state "
        "input. Attenuation is a reusable spatial curve definition. A Send is an "
        "extra signal route between buses. A Pack is an export bundle rooted in "
        "Actions and assets. Volume is always in decibels (dB).\n\n"

        "IDENTIFYING OBJECTS. Persistent object identity is the full GUID. Names "
        "and slash-separated logical paths can match multiple objects. Prefer "
        "the GUID returned by read commands; pass it in a GUID parameter or in "
        "the command's path selector. An ambiguous selector must be resolved "
        "before writing. Never invent or hash an id yourself. A phrase like \"delete the "
        "SFX Sound\" means the object named 'SFX' of type Sound - never fuse the "
        "type into the name (not 'SFXSound'). Ordinary object names allow Unicode "
        "and punctuation, but not blank names, controls, line breaks, or path "
        "separators. Creation and copying avoid sibling conflicts; explicit "
        "renaming may create duplicate names. Action and Variable names are "
        "bucket-unique SDK identifiers matching [A-Za-z][A-Za-z0-9_]*; conflicts "
        "automatically receive numeric suffixes. Always use the actual name "
        "returned by the command.\n\n"

        "COMMAND MAP. Commands are grouped by category; use describe_command to get "
        "one command's parameters before calling it. Read-only categories (safe, "
        "prefixed get_/list_/can_/query_/find_): project, mixer, spatial, asset, "
        "container, editor, meter (level/spectrum data), ui, audition, undo. "
        "Write/authoring categories: project (canonical object tree: create_item, "
        "delete_item, rename_item, move_item, set_item_property; pack metadata: "
        "set_action_pack_targeting), "
        "mixer (buses/sends: create_bus, set_bus_volume_db, set_send), "
        "spatial (create_attenuation, set_attenuation_curve, "
        "bind_attenuation_to_object, set_game_object_position), container "
        "(content selection and children: add_child_to_container, "
        "remove_child_from_container, "
        "set_container_property), asset (import_audio, set_conversion_settings, "
        "batch_replace_media), editor (derived Blueprint routing and overrides: "
        "connect_nodes, disconnect_nodes, set_node_override). Prefer create_item "
        "with type Action for canonical Action creation; create_action remains a "
        "permission-gated convenience command. Runtime side-effect categories (these "
        "actually make sound or change engine state - use only when the designer "
        "explicitly asks to audition, trigger, or load): audition "
        "(audition_play/stop), system runtime (post_action, trigger_action, "
        "stop_action, start_output, load_pack/unload_pack), advanced "
        "(set_master_volume, set_game_object_volume).\n\n"

        "HOW TO WORK. 1) Explore before you write: confirm a target exists and read "
        "its identity with list_items or get_item_info (or the matching list_/get_ "
        "for buses, attenuations, etc.) before create/modify/delete. 2) For a broad "
        "or batch request, list the relevant objects first, plan on the result, "
        "then execute one step at a time and read each result to confirm. 3) "
        "Destructive commands (delete_/remove_/batch_) require that you have "
        "already confirmed the exact target. Do not issue runtime side-effect "
        "commands just to check state. 4) If a command returns an error, read its "
        "code and message and change your approach - fix the arguments or choose a "
        "different command; never resend identical arguments. 5) Durable memory "
        "is explicit: call memory_upsert, memory_remove, or memory_clear only when "
        "the designer directly asks you to remember, update, forget, or clear "
        "information. Never infer or save a memory from an ordinary conversation. "
        "Use memory_list only when the task needs to inspect saved memory. After "
        "one successful memory mutation tool call, summarize and stop requesting "
        "memory mutations; the mutation commits only when the turn completes "
        "successfully. 6) Answer the "
        "designer in the language they used.\n\n"

        "HOW TO ANSWER. Treat tool results as evidence to explain, not as prose "
        "to copy. Unless the designer explicitly requests a strict output format "
        "or a terse answer, synthesize the relevant structure, relationships, "
        "scope, and count before stating the conclusion. A single result still "
        "needs its role and parent context when those facts are available. Mention "
        "important limits or uncertainty instead of implying the inspection was "
        "broader than it was. Unless the designer explicitly requests a strict "
        "output format, a terse answer, or no follow-up, finish with exactly one "
        "useful, context-aware next-step question whenever the result enables a "
        "reasonable follow-up. Briefly state the recommended action and ask whether "
        "the designer wants it performed; do not replace the question with a merely "
        "declarative suggestion. Derive the question from the current task and "
        "discovered objects, and never append a generic \"What would you like to do "
        "next?\". Do not invent relationships that were "
        "not returned by read tools, and do not dump raw identifiers or repeat "
        "names without explaining their meaning.\n\n"

        "RESPONSE LANGUAGE IS MANDATORY. Reply in the same natural language as "
        "the designer's latest message. Object names, file paths, command names, "
        "code, and isolated identifiers do not determine the response language. "
        "If the latest message contains no natural-language wording, continue "
        "the language of the most recent user message that does. If there is no "
        "such message, use the application locale. Never switch languages merely "
        "because project objects have English names.")
        .arg(objectTypes.join(QStringLiteral(", ")));
}
}

AIAssistantService::AIAssistantService(IPCServer* ipc, QObject* parent)
    : QObject(parent),
      ipc_(ipc),
      secretLoader_([](const QString& service, const QString& account) {
          return Harken::Platform::SecretStore::load(service, account);
      }),
      durableMemoryRetriever_(
          std::make_shared<DeterministicAIDurableMemoryRetriever>()),
      clock_([]() { return QDateTime::currentMSecsSinceEpoch(); })
{
}
AIAssistantService::~AIAssistantService() { teardownActiveTurn(); }
void AIAssistantService::setProvider(ILLMProvider* provider)
{
    if (provider_ == provider) return;
    abortCurrentTurn();
    if (provider_) { provider_->disconnect(this); delete provider_; }
    provider_ = provider;
    if (!provider_) return;
    provider_->setParent(this);
    connectProvider();
}
void AIAssistantService::setUnrestrictedMode(bool enabled) { unrestrictedMode_ = enabled; }
bool AIAssistantService::unrestrictedMode() const { return unrestrictedMode_; }
void AIAssistantService::setSecretLoader(SecretLoader loader) { secretLoader_ = std::move(loader); }
void AIAssistantService::setSettingsValueLoaderForTesting(SettingsValueLoader loader) { configStore_.setValueLoaderForTesting(std::move(loader)); }
void AIAssistantService::setClockForTesting(Clock clock) { clock_ = std::move(clock); }
void AIAssistantService::setExecutionLimitsForTesting(int maxToolExecutions, int maxModelRounds)
{
    maxToolExecutions_ = maxToolExecutions;
    maxModelRounds_ = maxModelRounds;
}
void AIAssistantService::setConversationStore(
    std::shared_ptr<IAIConversationStore> store)
{
    conversationStore_ = std::move(store);
    if (!conversationScope_.isEmpty())
    {
        const QString scope = conversationScope_;
        conversationScope_.clear();
        setConversationScope(scope);
    }
}
void AIAssistantService::setConversationSessionStore(
    std::shared_ptr<IAIConversationSessionStore> store)
{
    conversationSessionStore_ = std::move(store);
    if (!conversationScope_.isEmpty())
    {
        const QString scope = conversationScope_;
        conversationScope_.clear();
        setConversationScope(scope);
    }
}
void AIAssistantService::setDurableMemoryService(
    std::shared_ptr<AIDurableMemoryService> service)
{
    durableMemoryService_ = std::move(service);
}
void AIAssistantService::setDurableMemoryRetriever(
    std::shared_ptr<IAIDurableMemoryRetriever> retriever)
{
    durableMemoryRetriever_ = std::move(retriever);
}
void AIAssistantService::setConversationScope(const QString& scope)
{
    const QString normalizedScope = scope.trimmed();
    if (normalizedScope.isEmpty())
        return;
    if (normalizedScope == conversationScope_)
        return;

    abortCurrentTurn();
    conversationScope_ = normalizedScope;
    conversationHistory_ = {};
    conversationSessionCatalog_ = {};
    lastUsedDurableMemoryIds_.clear();

    if (conversationSessionStore_)
    {
        const AIConversationSessionLoadResult sessions =
            conversationSessionStore_->load(conversationScope_);
        if (sessions.status
            == AIConversationSessionLoadResult::Status::Loaded)
        {
            conversationSessionCatalog_ = sessions.catalog;
        }
        else
        {
            if (sessions.status
                    == AIConversationSessionLoadResult::Status::Invalid
                || sessions.status
                    == AIConversationSessionLoadResult::Status::IoError)
            {
                emit diagnosticReady(
                    AIServiceDiagnostic{sessions.code, sessions.message});
            }

            const qint64 now = qMax<qint64>(1, nowMs());
            AIConversationSessionRecord initial;
            initial.id =
                QString::fromLatin1(kDefaultConversationSessionId);
            initial.title = QStringLiteral("New Conversation");
            initial.createdAtMs = now;
            initial.updatedAtMs = now;
            conversationSessionCatalog_.activeSessionId = initial.id;
            conversationSessionCatalog_.sessions = {initial};
            if (sessions.status
                == AIConversationSessionLoadResult::Status::Missing)
            {
                const AIConversationSessionCatalog initialCatalog =
                    conversationSessionCatalog_;
                AIConversationSessionCatalog resolvedCatalog;
                const auto result = conversationSessionStore_->mutate(
                    conversationScope_,
                    [&](const AIConversationSessionLoadResult& current,
                        AIConversationSessionCatalog* replacement,
                        bool* shouldPersist) {
                        if (current.status
                            == AIConversationSessionLoadResult::Status::Loaded)
                        {
                            *replacement = current.catalog;
                            resolvedCatalog = current.catalog;
                            return conversationSessionSuccess();
                        }
                        if (current.status
                            != AIConversationSessionLoadResult::Status::Missing)
                        {
                            return conversationSessionFailure(
                                current.code, current.message);
                        }
                        *replacement = initialCatalog;
                        *shouldPersist = true;
                        resolvedCatalog = initialCatalog;
                        return conversationSessionSuccess();
                    });
                if (result.success)
                {
                    conversationSessionCatalog_ = resolvedCatalog;
                }
                else
                {
                    emit diagnosticReady(
                        AIServiceDiagnostic{result.code, result.message});
                }
            }
        }
    }

    loadConversationHistory();
    emitConversationSessions();
    emit conversationRestored(conversationHistory_);
}
QJsonArray AIAssistantService::conversationHistory() const
{
    return conversationHistory_;
}
QJsonArray AIAssistantService::conversationSessions() const
{
    QJsonArray sessions;
    for (const AIConversationSessionRecord& session
         : conversationSessionCatalog_.sessions)
    {
        sessions.append(conversationSessionWire(session));
    }
    return sessions;
}
QString AIAssistantService::activeConversationSessionId() const
{
    return conversationSessionCatalog_.activeSessionId;
}
QStringList AIAssistantService::lastUsedDurableMemoryIds() const
{
    return lastUsedDurableMemoryIds_;
}
qint64 AIAssistantService::nowMs() const { return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch(); }
void AIAssistantService::emitStepEvent(AIStepPhase phase, const QString& command,
                                       const QString& requestId)
{
    const QString id = requestId.isEmpty() ? activeRequestId_ : requestId;
    if (id.isEmpty())
        return;
    emit stepEventReady(AIStepEvent{phase, id, command, modelRounds_,
                                    static_cast<int>(activeTools_.size()), maxToolExecutions_,
                                    toolExecutions_});
}
IPCServer::IPCDispatchContext AIAssistantService::makeAIDispatchContext(
    const QString& requestId, const QString& permissionProfile) const
{
    IPCServer::IPCDispatchContext context;
    context.source = ActionSource::Internal;
    context.origin = IPCServer::DispatchOrigin::AI;
    context.requestId = requestId;
    context.permissionProfile = permissionProfile;
    context.unrestricted = unrestrictedMode_;
    return context;
}
void AIAssistantService::resetTurnRuntime(TurnState nextState)
{
    toolQueue_.clear();
    repeatedReads_.clear();
    pendingConfirmation_ = {};
    pendingMemoryMutation_.reset();
    toolExecutions_ = modelRounds_ = aggregateResultBytes_ = 0;
    turnDeadlineMs_ = nextState == TurnState::AwaitingModel
        ? nowMs() + kWholeTurnTimeoutMs
        : 0;
    confirmationWaitStartedMs_ = 0;
    legacyOneShot_ = false;
    awaitingForcedSummary_ = false;
    lastToolResponse_ = {};
    turnState_ = nextState;
}
void AIAssistantService::connectProvider()
{
    connect(provider_, &ILLMProvider::delta, this, [this](const QString& id, const QString& text) { if (id == activeRequestId_) emit responseDelta(id, text); });
    connect(provider_, &ILLMProvider::completed, this, [this](const ILLMProvider::Response& response) { handleCompleted(response); });
    connect(provider_, &ILLMProvider::failed, this, [this](const QString& id, const QString& code, const QString& message) { if (id == activeRequestId_) finishWithFailure(id, code, message); });
}
void AIAssistantService::submitUserMessage(const QString& text)
{
    if (turnState_ == TurnState::AwaitingConfirmation)
    {
        abortCurrentTurn();
    }
    else if (turnState_ != TurnState::Idle)
    {
        emit failed(activeRequestId_, QStringLiteral("INVALID_ARGUMENTS"),
                   QStringLiteral("An AI turn is already active."));
        return;
    }

    const QString requestId = QStringLiteral("ai-%1").arg(nextRequestNumber_++, 4, 10, QLatin1Char('0'));
    if (!provider_)
    {
        finishWithFailure(requestId, QStringLiteral("PROVIDER_NOT_FOUND"),
                          QStringLiteral("No LLM provider is configured."));
        return;
    }
    if (text.trimmed().isEmpty())
    {
        finishWithFailure(requestId, QStringLiteral("INVALID_ARGUMENTS"),
                          QStringLiteral("The user message must not be empty."));
        return;
    }

    const AIAgentConfig config = configStore_.effectiveSnapshot();
    const QString baseUrl = config.baseUrl;
    activeModel_ = config.model;
    activePermissionProfile_ = toId(config.permissionProfile);
    const int maxTokens = config.maxTokens;
    const int timeout = config.requestTimeoutMs;

    if (!isValidAIEndpoint(baseUrl, activeModel_, &activeEndpoint_))
    {
        finishWithFailure(requestId, QStringLiteral("INVALID_ARGUMENTS"),
                          QStringLiteral(
                              "The AI Base URL must use HTTPS and the model must be set. Open %1 "
                              "to review the Base URL and model, then try again.")
                              .arg(QLatin1String(kAIAgentPreferencesPath)));
        return;
    }
    activeContextWindowTokens_ = config.contextWindowTokens;
    activeMaxTokens_ = maxTokens;
    activeTimeoutMs_ = timeout;
    activeApiKey_.clear();

    if (provider_->requiresApiKey())
    {
        activeApiKey_ = secretLoader_
            ? secretLoader_(QStringLiteral("HarkenStudio.ai"), QStringLiteral("api_key"))
            : QString();
        if (activeApiKey_.isEmpty())
        {
            finishWithFailure(
                requestId, QStringLiteral("AUTH_MISSING"),
                QStringLiteral(
                    "The configured LLM provider requires an API key. Open %1 "
                    "to enter the API key, then try again.")
                    .arg(QLatin1String(kAIAgentPreferencesPath)));
            return;
        }
    }

    if (unrestrictedMode_)
    {
        activeTools_ = unrestrictedTools();
        activeBaseSystemPrompt_ = unrestrictedSystemPrompt()
            + QStringLiteral("\n\nAPPLICATION LOCALE: %1.")
                  .arg(QLocale::system().name());
    }
    else
    {
        activeTools_ = AICommandCatalog::selectTools(activePermissionProfile_);
        activeBaseSystemPrompt_ = QStringLiteral(
            "Return free text or tool calls. The active permission profile is %1.")
            .arg(activePermissionProfile_);
    }
    activeSystemPrompt_ = activeBaseSystemPrompt_;
    activeRequestId_ = requestId;
    activeHistoryMessageCount_ = conversationHistory_.size();
    transcript_ = conversationHistory_;
    transcript_.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                   {QStringLiteral("content"), text}});
    retrieveDurableMemoryContext(text);
    resetTurnRuntime(TurnState::AwaitingModel);

    submitModelRound();
}
void AIAssistantService::submitModelRound()
{
    if (turnState_ == TurnState::Idle || !provider_)
        return;
    if (nowMs() >= turnDeadlineMs_ || modelRounds_ >= maxModelRounds_)
    {
        requestForcedSummary();
        return;
    }
    if (!fitActiveRequestToContext(
            activeBaseSystemPrompt_, activeTools_, &activeSystemPrompt_))
    {
        finishWithFailure(
            activeRequestId_, QStringLiteral("CONTEXT_LIMIT_EXCEEDED"),
            QStringLiteral(
                "The AI request exceeds the configured context window. "
                "Open %1 to increase Context Window, or shorten the "
                "current request.")
                .arg(QLatin1String(kAIAgentPreferencesPath)));
        return;
    }
    for (const AIDurableMemoryRetrievalItem& item
         : activeRetrievedMemories_)
    {
        if (!activeUsedDurableMemoryIds_.contains(item.entry.id))
            activeUsedDurableMemoryIds_.append(item.entry.id);
    }

    ++modelRounds_;
    turnState_ = TurnState::AwaitingModel;
    emitStepEvent(modelRounds_ > 1 ? AIStepPhase::ResumedModelRound
                                   : AIStepPhase::ModelWaiting);
    ILLMProvider::Request request;
    request.requestId = activeRequestId_;
    request.model = activeModel_;
    request.systemPrompt = activeSystemPrompt_;
    request.messages = transcript_;
    request.apiKey = activeApiKey_;
    request.endpoint = activeEndpoint_;
    request.tools = activeTools_;
    request.maxTokens = activeMaxTokens_;
    request.timeoutMs = qMin(
        activeTimeoutMs_,
        static_cast<int>(qMax<qint64>(1, turnDeadlineMs_ - nowMs())));

    try
    {
        provider_->submit(request);
    }
    catch (...)
    {
        finishWithFailure(activeRequestId_, QStringLiteral("INTERNAL_ERROR"),
                          QStringLiteral("The LLM provider rejected the request unexpectedly."));
    }
}
void AIAssistantService::requestForcedSummary()
{
    // The turn ran out of model rounds / tool budget while still making tool
    // calls. Rather than ending on a bare AI_LOOP_LIMIT failure, run one final
    // model round with tools disabled so the model must return a closing text
    // summary and the turn returns to Idle through the normal responseReady path.
    // Safety limits (repeated reads, aggregate result budget) still hard-fail
    // and never reach here.
    if (awaitingForcedSummary_ || !provider_)
    {
        finishWithFailure(activeRequestId_, QStringLiteral("AI_LOOP_LIMIT"),
                          QStringLiteral("The AI turn exceeded its model-round or time limit."));
        return;
    }

    // Close any tool calls that were queued but never executed so the transcript
    // has a matching tool result for every declared tool_call id.
    for (const auto& pending : toolQueue_)
        transcript_.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("tool")},
            {QStringLiteral("tool_call_id"), pending.callId},
            {QStringLiteral("content"),
             QStringLiteral("{\"status\":\"error\",\"code\":\"AI_LOOP_LIMIT\","
                            "\"summary\":\"Skipped: the turn reached its step limit.\"}")}});
    toolQueue_.clear();

    awaitingForcedSummary_ = true;
    turnState_ = TurnState::AwaitingModel;
    emitStepEvent(AIStepPhase::ResumedModelRound);

    ILLMProvider::Request request;
    request.requestId = activeRequestId_;
    request.model = activeModel_;
    const QString summaryBaseSystemPrompt =
        activeBaseSystemPrompt_ + QStringLiteral(
        " You have reached the step limit for this turn. Do not request any more "
        "tools. Summarize what you accomplished and what remains, then stop.");
    request.apiKey = activeApiKey_;
    request.endpoint = activeEndpoint_;
    request.tools = {};
    request.maxTokens = activeMaxTokens_;
    // Bounded window for the closing summary: capped so it cannot grant another
    // full request timeout after the turn budget is spent, but never clamped to
    // the (already-exhausted) turn deadline, which would leave no time to reply.
    request.timeoutMs = qMin(activeTimeoutMs_, kForcedSummaryTimeoutMs);

    if (!fitActiveRequestToContext(
            summaryBaseSystemPrompt, request.tools,
            &request.systemPrompt))
    {
        finishWithFailure(
            activeRequestId_, QStringLiteral("CONTEXT_LIMIT_EXCEEDED"),
            QStringLiteral(
                "The AI summary request exceeds the configured context window. "
                "Open %1 to increase Context Window, or start a new "
                "conversation.")
                .arg(QLatin1String(kAIAgentPreferencesPath)));
        return;
    }
    for (const AIDurableMemoryRetrievalItem& item
         : activeRetrievedMemories_)
    {
        if (!activeUsedDurableMemoryIds_.contains(item.entry.id))
            activeUsedDurableMemoryIds_.append(item.entry.id);
    }
    request.messages = transcript_;

    try
    {
        provider_->submit(request);
    }
    catch (...)
    {
        finishWithFailure(activeRequestId_, QStringLiteral("INTERNAL_ERROR"),
                          QStringLiteral("The LLM provider rejected the summary request."));
    }
}
void AIAssistantService::abortCurrentTurn()
{
    const QString id = activeRequestId_;
    if (!id.isEmpty())
        emitStepEvent(AIStepPhase::Cancelled);
    teardownActiveTurn();
}
void AIAssistantService::clearConversation()
{
    abortCurrentTurn();
    if (conversationStore_ && !conversationScope_.isEmpty())
    {
        const auto result =
            conversationStore_->clear(conversationStorageScope());
        if (!result.success)
        {
            emit diagnosticReady(AIServiceDiagnostic{result.code, result.message});
            return;
        }
    }
    conversationHistory_ = {};
    lastUsedDurableMemoryIds_.clear();
    emit conversationCleared();
}
void AIAssistantService::createConversationSession()
{
    if (!conversationSessionStore_ || conversationScope_.isEmpty())
    {
        clearConversation();
        return;
    }

    abortCurrentTurn();
    AIConversationSessionRecord session;
    session.id = QUuid::createUuid()
        .toString(QUuid::WithoutBraces).toLower();
    session.title = QStringLiteral("New Conversation");
    session.createdAtMs = qMax<qint64>(1, nowMs());
    session.updatedAtMs = session.createdAtMs;

    AIConversationSessionCatalog next;
    const auto result = conversationSessionStore_->mutate(
        conversationScope_,
        [&](const AIConversationSessionLoadResult& current,
            AIConversationSessionCatalog* replacement,
            bool* shouldPersist) {
            if (current.status
                != AIConversationSessionLoadResult::Status::Loaded)
            {
                return conversationSessionFailure(
                    current.code.isEmpty()
                        ? QStringLiteral(
                              "AI_CONVERSATION_SESSIONS_UNAVAILABLE")
                        : current.code,
                    current.message.isEmpty()
                        ? QStringLiteral(
                              "Conversation sessions are unavailable.")
                        : current.message);
            }
            if (current.catalog.sessions.size()
                >= AIConversationSessionLimits::kMaximumSessions)
            {
                return conversationSessionFailure(
                    QStringLiteral("AI_CONVERSATION_SESSION_LIMIT"),
                    QStringLiteral(
                        "The conversation session limit has been reached."));
            }
            const auto collision = std::find_if(
                current.catalog.sessions.cbegin(),
                current.catalog.sessions.cend(),
                [&session](const AIConversationSessionRecord& existing) {
                    return existing.id == session.id;
                });
            if (collision != current.catalog.sessions.cend())
            {
                return conversationSessionFailure(
                    QStringLiteral("AI_CONVERSATION_SESSION_CONFLICT"),
                    QStringLiteral(
                        "The new conversation session ID already exists."));
            }

            next = current.catalog;
            next.sessions.prepend(session);
            next.activeSessionId = session.id;
            *replacement = next;
            *shouldPersist = true;
            return conversationSessionSuccess();
        });
    if (!result.success)
    {
        emit diagnosticReady(
            AIServiceDiagnostic{result.code, result.message});
        return;
    }

    conversationSessionCatalog_ = next;
    conversationHistory_ = {};
    lastUsedDurableMemoryIds_.clear();
    emitConversationSessions();
    emit conversationRestored(conversationHistory_);
}
void AIAssistantService::switchConversationSession(
    const QString& sessionId)
{
    const QString normalizedId =
        sessionId.trimmed().toLower();
    if (!conversationSessionStore_
        || normalizedId.isEmpty()
        || normalizedId
            == conversationSessionCatalog_.activeSessionId)
    {
        return;
    }
    abortCurrentTurn();
    AIConversationSessionCatalog next;
    const auto result = conversationSessionStore_->mutate(
        conversationScope_,
        [&](const AIConversationSessionLoadResult& current,
            AIConversationSessionCatalog* replacement,
            bool* shouldPersist) {
            if (current.status
                != AIConversationSessionLoadResult::Status::Loaded)
            {
                return conversationSessionFailure(
                    current.code.isEmpty()
                        ? QStringLiteral(
                              "AI_CONVERSATION_SESSIONS_UNAVAILABLE")
                        : current.code,
                    current.message.isEmpty()
                        ? QStringLiteral(
                              "Conversation sessions are unavailable.")
                        : current.message);
            }
            const auto found = std::find_if(
                current.catalog.sessions.cbegin(),
                current.catalog.sessions.cend(),
                [&normalizedId](
                    const AIConversationSessionRecord& session) {
                    return session.id == normalizedId;
                });
            if (found == current.catalog.sessions.cend())
            {
                return conversationSessionFailure(
                    QStringLiteral(
                        "AI_CONVERSATION_SESSION_NOT_FOUND"),
                    QStringLiteral(
                        "The selected conversation session was not found."));
            }

            next = current.catalog;
            next.activeSessionId = normalizedId;
            *replacement = next;
            *shouldPersist = true;
            return conversationSessionSuccess();
        });
    if (!result.success)
    {
        emit diagnosticReady(
            AIServiceDiagnostic{result.code, result.message});
        return;
    }
    conversationSessionCatalog_ = next;
    conversationHistory_ = {};
    lastUsedDurableMemoryIds_.clear();
    loadConversationHistory();
    emitConversationSessions();
    emit conversationRestored(conversationHistory_);
}
void AIAssistantService::teardownActiveTurn()
{
    const QString id = activeRequestId_;
    if (ipc_ && !id.isEmpty()) ipc_->revokeAIGrants(id);
    if (provider_ && !id.isEmpty()) provider_->cancel(id);
    clearActiveTurn();
}
void AIAssistantService::handleCompleted(const ILLMProvider::Response& response)
{
    if (turnState_ != TurnState::AwaitingModel || response.requestId != activeRequestId_)
        return;

    if (awaitingForcedSummary_)
    {
        // The closing summary round ran with tools disabled; any tool calls the
        // model still returned are ignored so the turn always ends with text.
        const QString id = activeRequestId_;
        QString text = response.text.left(kMaximumResponseBytes);
        if (text.trimmed().isEmpty())
            text = QStringLiteral("The AI turn reached its step limit before it could finish.");
        QString memoryErrorCode;
        QString memoryErrorMessage;
        if (!commitPendingMemoryMutation(
                &memoryErrorCode, &memoryErrorMessage))
        {
            finishWithFailure(
                id, memoryErrorCode, memoryErrorMessage);
            return;
        }
        commitCompletedTurn(text);
        emitStepEvent(AIStepPhase::Completed);
        clearActiveTurn();
        emit responseReady(id, text);
        return;
    }

    QList<ILLMProvider::ToolCall> calls = response.toolCalls;
    legacyOneShot_ = calls.isEmpty() && response.hasCommand;
    if (legacyOneShot_)
    {
        const QJsonValue legacyRequestId = response.structuredCommand.value(QStringLiteral("request_id"));
        if (!legacyRequestId.isString()
            || legacyRequestId.toString() != activeRequestId_)
        {
            finishWithFailure(activeRequestId_, QStringLiteral("INVALID_RESPONSE"),
                              QStringLiteral("The structured command request_id does not match the active turn."));
            return;
        }
        calls.append({QStringLiteral("legacy-%1").arg(activeRequestId_),
                      response.structuredCommand.value(QStringLiteral("cmd")).toString(),
                      response.structuredCommand.value(QStringLiteral("args")).toObject()});
    }
    if (calls.isEmpty())
    {
        const QString id = activeRequestId_;
        const QString text = response.text.left(kMaximumResponseBytes);
        QString memoryErrorCode;
        QString memoryErrorMessage;
        if (!commitPendingMemoryMutation(
                &memoryErrorCode, &memoryErrorMessage))
        {
            finishWithFailure(
                id, memoryErrorCode, memoryErrorMessage);
            return;
        }
        commitCompletedTurn(text);
        emitStepEvent(AIStepPhase::Completed);
        clearActiveTurn();
        emit responseReady(id, text);
        return;
    }
    if (calls.size() > kMaximumCallsPerResponse)
    {
        finishWithFailure(activeRequestId_, QStringLiteral("INVALID_RESPONSE"),
                          QStringLiteral("The AI provider returned too many tool calls."));
        return;
    }

    QSet<QString> ids;
    for (const auto& call : calls)
    {
        if (call.callId.isEmpty() || call.name.isEmpty() || ids.contains(call.callId))
        {
            finishWithFailure(
                activeRequestId_, QStringLiteral("INVALID_RESPONSE"),
                QStringLiteral("The AI provider returned malformed tool calls."));
            return;
        }
        ids.insert(call.callId);
    }

    QJsonArray wireCalls;
    for (const auto& call : calls)
        wireCalls.append(toolCallWire(call));
    transcript_.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                   {QStringLiteral("content"), QJsonValue::Null},
                                   {QStringLiteral("tool_calls"), wireCalls}});
    toolQueue_ = calls;
    turnState_ = TurnState::RunningTools;
    processToolQueue();
}
void AIAssistantService::processToolQueue()
{
    while (turnState_ == TurnState::RunningTools && !toolQueue_.isEmpty())
    {
        if (nowMs() >= turnDeadlineMs_ || toolExecutions_ >= maxToolExecutions_)
        {
            requestForcedSummary();
            return;
        }

        const auto call = toolQueue_.takeFirst();
        if (unrestrictedMode_)
        {
            if (!executeUnrestrictedCall(call))
                return;
            continue;
        }

        const QJsonObject envelope{{QStringLiteral("cmd"), call.name},
                                   {QStringLiteral("args"), call.arguments},
                                   {QStringLiteral("request_id"), activeRequestId_}};
        AICommandValidator::Policy policy;
        policy.permission = parsePermissionMode(activePermissionProfile_);
        QString code;
        QString message;
        const auto command = validator_.normalizeAndValidate(envelope, policy, &code, &message);
        if (!command)
        {
            if (code == QLatin1String("AI_CONFIRMATION_REQUIRED"))
            {
                const IPCServer::IPCDispatchContext context = makeAIDispatchContext(
                    activeRequestId_, activePermissionProfile_);
                confirmationWaitStartedMs_ = nowMs();
                pendingConfirmation_ = {
                    call,
                    envelope,
                    activePermissionProfile_,
                    ipc_ ? ipc_->aiBindingFor(call.name, call.arguments, context)
                         : QByteArray{},
                    nowMs() + kConfirmationTimeoutMs};
                turnState_ = TurnState::AwaitingConfirmation;
                emitStepEvent(AIStepPhase::ConfirmationWaiting, call.name);
                emit confirmationRequired(activeRequestId_, envelope);
                return;
            }

            ++toolExecutions_;
            if (!appendToolResult(call, errorEnvelope(code, message)))
                return;
            continue;
        }

        if (ipc_ && ipc_->isReadOnlyCommand(call.name)
            && repeatedReadExceeded(call.name, call.arguments))
        {
            finishWithFailure(
                activeRequestId_, QStringLiteral("AI_LOOP_LIMIT"),
                QStringLiteral("The AI turn repeated the same read tool call."));
            return;
        }

        executeResolvedCall(call, call.name, call.arguments, makeAIDispatchContext(
            activeRequestId_, activePermissionProfile_));
    }

    if (turnState_ == TurnState::RunningTools)
    {
        if (legacyOneShot_)
            finishLegacySuccess();
        else
            submitModelRound();
    }
}
bool AIAssistantService::repeatedReadExceeded(const QString& cmd, const QJsonObject& args)
{
    const QByteArray signature = QCryptographicHash::hash(
        QJsonDocument(args).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
    return ++repeatedReads_[cmd + QLatin1Char(':') + QString::fromLatin1(signature)] >= 3;
}
bool AIAssistantService::executeUnrestrictedCall(const ILLMProvider::ToolCall& call)
{
    QString cmd = call.name;
    QJsonObject args = call.arguments;
    if (cmd == QLatin1String("dispatch_ipc"))
    {
        const QJsonValue commandValue = args.value(QStringLiteral("command"));
        if (!commandValue.isString() || commandValue.toString().isEmpty())
        {
            ++toolExecutions_;
            return appendToolResult(call, errorEnvelope(QStringLiteral("INVALID_ARGUMENTS"),
                QStringLiteral("dispatch_ipc requires a non-empty command string.")));
        }
        cmd = commandValue.toString();
        args = args.value(QStringLiteral("args")).toObject();
    }

    const bool memoryCall = cmd == QLatin1String("memory_list")
        || cmd == QLatin1String("memory_upsert")
        || cmd == QLatin1String("memory_remove")
        || cmd == QLatin1String("memory_clear");
    const bool readLike = cmd == QLatin1String("describe_command")
        || cmd == QLatin1String("memory_list")
        || (ipc_ && ipc_->isReadOnlyCommand(cmd));
    if (readLike && repeatedReadExceeded(cmd, args))
    {
        finishWithFailure(activeRequestId_, QStringLiteral("AI_LOOP_LIMIT"),
                          QStringLiteral("The AI turn repeated the same read tool call."));
        return false;
    }

    if (cmd == QLatin1String("describe_command"))
    {
        ++toolExecutions_;
        emitStepEvent(AIStepPhase::ToolExecution, cmd);
        return appendToolResult(call,
            describeCommandEnvelope(args.value(QStringLiteral("name")).toString()));
    }
    if (memoryCall)
        return executeMemoryCall(call, cmd, args);

    return executeResolvedCall(call, cmd, args, makeAIDispatchContext(
        activeRequestId_, activePermissionProfile_));
}
void AIAssistantService::retrieveDurableMemoryContext(
    const QString& userText)
{
    activeRetrievedMemories_.clear();
    activeSystemPrompt_ = activeBaseSystemPrompt_;
    if (!durableMemoryService_ || !durableMemoryRetriever_)
        return;

    QList<AIDurableMemoryCandidate> candidates;
    auto loadScope = [this, &candidates](
                         const QString& storageScope,
                         const QString& logicalScope) {
        const AIDurableMemoryLoadResult loaded =
            durableMemoryService_->list(storageScope);
        if (loaded.status == AIDurableMemoryLoadResult::Status::Invalid
            || loaded.status == AIDurableMemoryLoadResult::Status::IoError)
        {
            emit diagnosticReady(
                AIServiceDiagnostic{loaded.code, loaded.message});
            return;
        }
        if (loaded.status != AIDurableMemoryLoadResult::Status::Loaded)
            return;
        for (const AIDurableMemoryEntry& entry : loaded.entries)
            candidates.append({logicalScope, entry});
    };

    loadScope(QStringLiteral("user:default"), QStringLiteral("user"));
    if (conversationScope_.startsWith(QStringLiteral("project:")))
        loadScope(conversationScope_, QStringLiteral("project"));
    if (candidates.isEmpty())
        return;

    QJsonArray currentTurn;
    for (int index = activeHistoryMessageCount_;
         index < transcript_.size();
         ++index)
    {
        currentTurn.append(transcript_.at(index));
    }
    const int inputBudget = activeContextWindowTokens_
        - activeMaxTokens_ - kContextSafetyReserveTokens;
    const int fixedRequestBytes = conservativeInputTokenEstimate(
        activeBaseSystemPrompt_, currentTurn, activeTools_);
    const int availableBytes = qMax(0, inputBudget - fixedRequestBytes);
    const int durableBudget = qMin(
        kMaximumDurableMemoryContextBytes,
        qMin(availableBytes, qMax(0, inputBudget / 4)));

    const AIDurableMemoryRetrievalResult retrieved =
        durableMemoryRetriever_->retrieve(
            AIDurableMemoryRetrievalQuery{
                userText,
                durableBudget,
                kMaximumRetrievedMemoryEntries
            },
            candidates);
    activeRetrievedMemories_ = retrieved.items;
    if (!retrieved.contextBlock.isEmpty())
    {
        activeSystemPrompt_ = activeBaseSystemPrompt_
            + QStringLiteral("\n\n") + retrieved.contextBlock;
    }
}
QString AIAssistantService::durableMemoryScope(
    const QString& scopeName) const
{
    if (scopeName == QLatin1String("user"))
        return QStringLiteral("user:default");
    if (scopeName == QLatin1String("project")
        && conversationScope_.startsWith(QStringLiteral("project:")))
    {
        return conversationScope_;
    }
    return {};
}
bool AIAssistantService::executeMemoryCall(
    const ILLMProvider::ToolCall& call,
    const QString& command,
    const QJsonObject& arguments)
{
    ++toolExecutions_;
    emitStepEvent(AIStepPhase::ToolExecution, command);

    const QJsonValue scopeValue = arguments.value(QStringLiteral("scope"));
    if (!scopeValue.isString())
    {
        return appendToolResult(
            call, errorEnvelope(
                QStringLiteral("INVALID_ARGUMENTS"),
                QStringLiteral("Memory scope must be user or project.")));
    }
    const QString scopeName = scopeValue.toString();
    const QString scope = durableMemoryScope(scopeName);
    if (scope.isEmpty())
    {
        return appendToolResult(
            call, errorEnvelope(
                QStringLiteral("AI_DURABLE_MEMORY_SCOPE_UNAVAILABLE"),
                scopeName == QLatin1String("project")
                    ? QStringLiteral(
                        "Project memory requires an open saved project.")
                    : QStringLiteral(
                        "Memory scope must be user or project.")));
    }
    if (!durableMemoryService_)
    {
        return appendToolResult(
            call, errorEnvelope(
                QStringLiteral("AI_DURABLE_MEMORY_UNAVAILABLE"),
                QStringLiteral("Durable memory storage is unavailable.")));
    }

    if (command == QLatin1String("memory_list"))
    {
        const AIDurableMemoryLoadResult loaded =
            durableMemoryService_->list(scope);
        if (loaded.status == AIDurableMemoryLoadResult::Status::Invalid
            || loaded.status == AIDurableMemoryLoadResult::Status::IoError)
        {
            return appendToolResult(
                call, errorEnvelope(loaded.code, loaded.message));
        }

        QJsonArray entries;
        if (loaded.status == AIDurableMemoryLoadResult::Status::Loaded)
        {
            for (const AIDurableMemoryEntry& entry : loaded.entries)
                entries.append(memoryEntryWire(entry));
        }
        return appendToolResult(
            call, successEnvelope(QJsonObject{
                {QStringLiteral("scope"), scopeName},
                {QStringLiteral("entries"), entries},
                {QStringLiteral("count"), entries.size()}}));
    }

    if (command == QLatin1String("memory_upsert"))
    {
        if (pendingMemoryMutation_)
        {
            return appendToolResult(
                call, errorEnvelope(
                    QStringLiteral(
                        "AI_DURABLE_MEMORY_MUTATION_PENDING"),
                    QStringLiteral(
                        "Only one durable memory mutation is allowed per turn.")));
        }
        const QJsonValue keyValue = arguments.value(QStringLiteral("key"));
        const QJsonValue valueValue = arguments.value(QStringLiteral("value"));
        if (!keyValue.isString() || !valueValue.isString())
        {
            return appendToolResult(
                call, errorEnvelope(
                    QStringLiteral("INVALID_ARGUMENTS"),
                    QStringLiteral(
                        "Memory key and value must be strings.")));
        }
        const QString key =
            keyValue.toString().trimmed().toCaseFolded();
        const QString value = valueValue.toString().trimmed();
        if (key.isEmpty()
            || key.toUtf8().size()
                > AIDurableMemoryLimits::kMaximumKeyBytes
            || value.isEmpty()
            || value.toUtf8().size()
                > AIDurableMemoryLimits::kMaximumValueBytes)
        {
            return appendToolResult(
                call, errorEnvelope(
                    QStringLiteral("AI_DURABLE_MEMORY_INVALID"),
                    QStringLiteral(
                        "Memory key or value is invalid.")));
        }

        const AIDurableMemoryLoadResult loaded =
            durableMemoryService_->list(scope);
        if (loaded.status
                == AIDurableMemoryLoadResult::Status::Invalid
            || loaded.status
                == AIDurableMemoryLoadResult::Status::IoError)
        {
            return appendToolResult(
                call, errorEnvelope(loaded.code, loaded.message));
        }
        if (loaded.status
            == AIDurableMemoryLoadResult::Status::Loaded)
        {
            const AIDurableMemoryKind kind =
                scopeName == QLatin1String("user")
                    ? AIDurableMemoryKind::UserPreference
                    : AIDurableMemoryKind::ProjectFact;
            for (const AIDurableMemoryEntry& entry : loaded.entries)
            {
                if (entry.kind == kind && entry.key == key
                    && entry.value == value)
                {
                    return appendToolResult(
                        call, successEnvelope(QJsonObject{
                            {QStringLiteral("scope"), scopeName},
                            {QStringLiteral("changed"), false},
                            {QStringLiteral("pending"), false},
                            {QStringLiteral("entry"),
                             memoryEntryWire(entry)}}));
                }
            }
        }

        pendingMemoryMutation_ = PendingMemoryMutation{
            PendingMemoryMutation::Type::Upsert,
            scopeName,
            scope,
            key,
            value,
            {}};
        return appendToolResult(
            call, successEnvelope(QJsonObject{
                {QStringLiteral("scope"), scopeName},
                {QStringLiteral("changed"), true},
                {QStringLiteral("pending"), true},
                {QStringLiteral("key"), key}}));
    }

    if (command == QLatin1String("memory_remove"))
    {
        if (pendingMemoryMutation_)
        {
            return appendToolResult(
                call, errorEnvelope(
                    QStringLiteral(
                        "AI_DURABLE_MEMORY_MUTATION_PENDING"),
                    QStringLiteral(
                        "Only one durable memory mutation is allowed per turn.")));
        }
        const QJsonValue idValue = arguments.value(QStringLiteral("id"));
        if (!idValue.isString())
        {
            return appendToolResult(
                call, errorEnvelope(
                    QStringLiteral("INVALID_ARGUMENTS"),
                    QStringLiteral("Memory ID must be a string.")));
        }
        const QString id =
            idValue.toString().trimmed().toLower();
        const QUuid parsedId(id);
        if (parsedId.isNull()
            || parsedId.toString(QUuid::WithoutBraces).toLower()
                != id)
        {
            return appendToolResult(
                call, errorEnvelope(
                    QStringLiteral("AI_DURABLE_MEMORY_INVALID"),
                    QStringLiteral("Memory ID is invalid.")));
        }
        const AIDurableMemoryLoadResult loaded =
            durableMemoryService_->list(scope);
        if (loaded.status
                == AIDurableMemoryLoadResult::Status::Invalid
            || loaded.status
                == AIDurableMemoryLoadResult::Status::IoError)
        {
            return appendToolResult(
                call, errorEnvelope(loaded.code, loaded.message));
        }
        const bool found =
            loaded.status
                == AIDurableMemoryLoadResult::Status::Loaded
            && std::any_of(
                loaded.entries.cbegin(), loaded.entries.cend(),
                [&id](const AIDurableMemoryEntry& entry) {
                    return entry.id == id;
                });
        if (!found)
        {
            return appendToolResult(
                call, errorEnvelope(
                    QStringLiteral(
                        "AI_DURABLE_MEMORY_NOT_FOUND"),
                    QStringLiteral(
                        "Durable memory entry was not found.")));
        }

        pendingMemoryMutation_ = PendingMemoryMutation{
            PendingMemoryMutation::Type::Remove,
            scopeName,
            scope,
            {},
            {},
            id};
        return appendToolResult(
            call, successEnvelope(QJsonObject{
                {QStringLiteral("scope"), scopeName},
                {QStringLiteral("id"), id},
                {QStringLiteral("pending"), true}}));
    }

    if (pendingMemoryMutation_)
    {
        return appendToolResult(
            call, errorEnvelope(
                QStringLiteral(
                    "AI_DURABLE_MEMORY_MUTATION_PENDING"),
                QStringLiteral(
                    "Only one durable memory mutation is allowed per turn.")));
    }
    pendingMemoryMutation_ = PendingMemoryMutation{
        PendingMemoryMutation::Type::Clear,
        scopeName,
        scope,
        {},
        {},
        {}};
    return appendToolResult(
        call, successEnvelope(QJsonObject{
            {QStringLiteral("scope"), scopeName},
            {QStringLiteral("pending"), true}}));
}
bool AIAssistantService::commitPendingMemoryMutation(
    QString* errorCode,
    QString* errorMessage)
{
    if (!pendingMemoryMutation_)
        return true;
    if (!durableMemoryService_)
    {
        if (errorCode)
        {
            *errorCode =
                QStringLiteral("AI_DURABLE_MEMORY_UNAVAILABLE");
        }
        if (errorMessage)
        {
            *errorMessage =
                QStringLiteral(
                    "Durable memory storage is unavailable.");
        }
        return false;
    }

    const PendingMemoryMutation mutation =
        *pendingMemoryMutation_;
    AIDurableMemoryOperationResult result;
    if (mutation.type
        == PendingMemoryMutation::Type::Upsert)
    {
        AIDurableMemoryService::WriteRequest request;
        request.kind =
            mutation.scopeName == QLatin1String("user")
                ? AIDurableMemoryKind::UserPreference
                : AIDurableMemoryKind::ProjectFact;
        request.key = mutation.key;
        request.value = mutation.value;
        request.source = AIDurableMemorySource::Model;
        request.sourceReference = activeRequestId_;
        result = durableMemoryService_->upsert(
            mutation.scope, request);
    }
    else if (mutation.type
             == PendingMemoryMutation::Type::Remove)
    {
        result = durableMemoryService_->remove(
            mutation.scope, mutation.id);
    }
    else
    {
        result = durableMemoryService_->clear(
            mutation.scope);
    }

    if (!result.success)
    {
        if (errorCode)
            *errorCode = result.code;
        if (errorMessage)
            *errorMessage = result.message;
        return false;
    }
    pendingMemoryMutation_.reset();
    if (errorCode)
        errorCode->clear();
    if (errorMessage)
        errorMessage->clear();
    return true;
}
QJsonObject AIAssistantService::describeCommandEnvelope(const QString& name) const
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return errorEnvelope(QStringLiteral("INVALID_ARGUMENTS"),
                             QStringLiteral("describe_command requires a non-empty name string."));

    QJsonObject result{{QStringLiteral("command"), trimmed}};
    const AICommandSpec* spec = AICommandCatalog::find(trimmed);
    const ActionDef* action = ActionManager::instance().findAction(trimmed);

    const auto fromCatalog = [&] {
        result.insert(QStringLiteral("description"), spec->description);
        result.insert(QStringLiteral("parameters"), spec->argumentSchema);
        result.insert(QStringLiteral("source"), QStringLiteral("catalog"));
        return successEnvelope(result);
    };
    const auto fromAction = [&] {
        result.insert(QStringLiteral("displayName"), action->displayName);
        result.insert(QStringLiteral("category"), action->category);
        result.insert(QStringLiteral("parameters"), action->paramSchema);
        result.insert(QStringLiteral("undoable"), action->undoable);
        result.insert(QStringLiteral("source"), QStringLiteral("action"));
        result.insert(QStringLiteral("note"), QStringLiteral(
            "The parameter description is informal; an empty parameters object means "
            "the arguments are undocumented."));
        return successEnvelope(result);
    };

    // Prefer whichever source actually documents parameters. A catalog entry
    // with an empty schema (for example a disabled placeholder) must not shadow
    // a real ActionManager paramSchema for the same command; fall back to a
    // present-but-empty source only when neither documents parameters.
    if (spec && !spec->argumentSchema.isEmpty())
        return fromCatalog();
    if (action && !action->paramSchema.isEmpty())
        return fromAction();
    if (spec)
        return fromCatalog();
    if (action)
        return fromAction();

    const bool knownHandler = ipc_ && ipc_->hasCommandHandler(trimmed);
    result.insert(QStringLiteral("source"),
                  knownHandler ? QStringLiteral("handler") : QStringLiteral("unknown"));
    result.insert(QStringLiteral("note"), knownHandler
        ? QStringLiteral("No schema is available for this command; call it through "
                         "dispatch_ipc and correct the arguments from returned error messages.")
        : QStringLiteral("This command is not registered; check the command list in the "
                         "system prompt."));
    return successEnvelope(result);
}
bool AIAssistantService::executeResolvedCall(const ILLMProvider::ToolCall& call,
                                             const QString& cmd, const QJsonObject& args,
                                             const IPCServer::IPCDispatchContext& context)
{
    ++toolExecutions_;
    emitStepEvent(AIStepPhase::ToolExecution, cmd);
    lastToolResponse_ = ipc_
        ? ipc_->dispatchCommand(cmd, args, context)
        : errorEnvelope(QStringLiteral("IPC_ERROR"),
                        QStringLiteral("The Editor command dispatcher is unavailable."));
    return appendToolResult(call, lastToolResponse_);
}
bool AIAssistantService::appendToolResult(const ILLMProvider::ToolCall& call, const QJsonObject& envelope)
{
    QJsonObject normalized = AIResultNormalizer::normalize(
        envelope, activeRequestId_, call.callId, kMaximumToolResultBytes);
    normalized.insert(QStringLiteral("cmd"), call.name);
    const QByteArray serialized =
        QJsonDocument(normalized).toJson(QJsonDocument::Compact);
    if (aggregateResultBytes_ + serialized.size() > kMaximumAggregateResultBytes)
    {
        finishWithFailure(activeRequestId_, QStringLiteral("AI_LOOP_LIMIT"),
                          QStringLiteral("The AI turn exceeded its tool result budget."));
        return false;
    }

    aggregateResultBytes_ += serialized.size();
    transcript_.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                                   {QStringLiteral("tool_call_id"), call.callId},
                                   {QStringLiteral("content"), QString::fromUtf8(serialized)}});
    return true;
}
void AIAssistantService::finishLegacySuccess()
{
    const QString id = activeRequestId_;
    const QByteArray serialized =
        QJsonDocument(lastToolResponse_).toJson(QJsonDocument::Compact);
    const QString responseText =
        QString::fromUtf8(serialized.left(kMaximumResponseBytes));
    QString memoryErrorCode;
    QString memoryErrorMessage;
    if (!commitPendingMemoryMutation(
            &memoryErrorCode, &memoryErrorMessage))
    {
        finishWithFailure(
            id, memoryErrorCode, memoryErrorMessage);
        return;
    }
    commitCompletedTurn(responseText);
    emitStepEvent(AIStepPhase::Completed);
    clearActiveTurn();
    emit responseReady(id, responseText);
}
void AIAssistantService::resolveConfirmation(const QString& requestId, bool approved)
{
    if (turnState_ != TurnState::AwaitingConfirmation
        || requestId != activeRequestId_
        || pendingConfirmation_.command.isEmpty())
    {
        emit failed(requestId, QStringLiteral("AI_CONFIRMATION_EXPIRED"),
                   QStringLiteral("The confirmation request is unknown or has expired."));
        return;
    }

    const PendingConfirmation pending = pendingConfirmation_;
    pendingConfirmation_ = {};
    const qint64 now = nowMs();
    if (confirmationWaitStartedMs_ > 0)
    {
        turnDeadlineMs_ += qMax<qint64>(0, now - confirmationWaitStartedMs_);
        confirmationWaitStartedMs_ = 0;
    }
    turnState_ = TurnState::RunningTools;

    if (pending.expiresAtMs <= now)
    {
        appendToolResult(
            pending.call,
            errorEnvelope(QStringLiteral("AI_CONFIRMATION_EXPIRED"),
                          QStringLiteral("The confirmation request expired.")));
        if (legacyOneShot_)
        {
            finishWithFailure(requestId, QStringLiteral("AI_CONFIRMATION_EXPIRED"),
                              QStringLiteral("The confirmation request expired."));
        }
        else
        {
            processToolQueue();
        }
        return;
    }

    if (!approved)
    {
        appendToolResult(
            pending.call,
            errorEnvelope(QStringLiteral("AI_CONFIRMATION_REJECTED"),
                          QStringLiteral("The requested command was rejected.")));
        if (legacyOneShot_)
        {
            finishWithFailure(requestId, QStringLiteral("AI_CONFIRMATION_REJECTED"),
                              QStringLiteral("The requested command was rejected."));
        }
        else
        {
            processToolQueue();
        }
        return;
    }

    AICommandValidator::Policy policy;
    policy.permission = parsePermissionMode(pending.permissionProfile);
    policy.confirmationGranted = true;
    QString code;
    QString message;
    const auto command =
        validator_.normalizeAndValidate(pending.command, policy, &code, &message);
    IPCServer::IPCDispatchContext context = makeAIDispatchContext(
        requestId, pending.permissionProfile);

    if (!command
        || !ipc_
        || ipc_->aiBindingFor(pending.call.name, pending.call.arguments, context)
               != pending.binding)
    {
        appendToolResult(
            pending.call,
            errorEnvelope(
                code.isEmpty() ? QStringLiteral("AI_GRANT_INVALID") : code,
                message.isEmpty()
                    ? QStringLiteral("The confirmed command no longer matches its approval.")
                    : message));
        if (legacyOneShot_)
        {
            finishWithFailure(requestId, QStringLiteral("AI_GRANT_INVALID"),
                              QStringLiteral("The confirmed command no longer matches its approval."));
        }
        else
        {
            processToolQueue();
        }
        return;
    }

    context.grantId = ipc_->issueAIGrant(
        pending.call.name, pending.call.arguments, context);
    executeResolvedCall(pending.call, pending.call.name, pending.call.arguments, context);
    if (legacyOneShot_)
        finishLegacySuccess();
    else
        processToolQueue();
}
void AIAssistantService::finishWithFailure(const QString& requestId,
                                           const QString& code,
                                           const QString& message)
{
    emitStepEvent(AIStepPhase::Failed, QString(), requestId);
    if (ipc_ && !requestId.isEmpty())
        ipc_->revokeAIGrants(requestId);
    clearActiveTurn();
    emit failed(requestId,
                code.isEmpty() ? QStringLiteral("INTERNAL_ERROR") : code,
                boundedErrorMessage(message));
}
bool AIAssistantService::fitActiveRequestToContext(
    const QString& baseSystemPrompt,
    const QList<AICommandSpec>& tools,
    QString* fittedSystemPrompt)
{
    if (!fittedSystemPrompt)
        return false;

    const int inputBudget = activeContextWindowTokens_ - activeMaxTokens_
        - kContextSafetyReserveTokens;
    auto rebuildSystemPrompt = [
        this, &baseSystemPrompt, fittedSystemPrompt]() {
        *fittedSystemPrompt = baseSystemPrompt;
        const QString memoryContext =
            buildDurableMemoryContextBlock(activeRetrievedMemories_);
        if (!memoryContext.isEmpty())
        {
            *fittedSystemPrompt +=
                QStringLiteral("\n\n") + memoryContext;
        }
    };
    rebuildSystemPrompt();
    auto requestCost = [this, fittedSystemPrompt, &tools]() {
        return conservativeInputTokenEstimate(
            *fittedSystemPrompt, transcript_, tools);
    };

    while (!activeRetrievedMemories_.isEmpty()
           && requestCost() > inputBudget)
    {
        activeRetrievedMemories_.removeLast();
        rebuildSystemPrompt();
    }
    while (activeHistoryMessageCount_ >= 2 && requestCost() > inputBudget)
    {
        transcript_.removeAt(0);
        transcript_.removeAt(0);
        activeHistoryMessageCount_ -= 2;
    }
    return inputBudget > 0 && requestCost() <= inputBudget;
}
void AIAssistantService::commitCompletedTurn(const QString& responseText)
{
    QJsonObject userMessage;
    for (int index = transcript_.size() - 1; index >= 0; --index)
    {
        const QJsonObject message = transcript_[index].toObject();
        if (message.value(QStringLiteral("role")).toString() == QLatin1String("user"))
        {
            userMessage = message;
            break;
        }
    }
    if (userMessage.isEmpty())
        return;

    lastUsedDurableMemoryIds_ = activeUsedDurableMemoryIds_;

    userMessage.insert(
        QStringLiteral("content"),
        boundedUtf8(userMessage.value(QStringLiteral("content")).toString(),
                    kMaximumHistoryMessageBytes));
    conversationHistory_.append(userMessage);
    conversationHistory_.append(
        QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                    {QStringLiteral("content"),
                     boundedUtf8(responseText, kMaximumHistoryMessageBytes)}});
    trimConversationHistory();
    persistConversationHistory();
    updateActiveConversationMetadata(
        userMessage.value(QStringLiteral("content")).toString());
}
void AIAssistantService::trimConversationHistory()
{
    auto serializedSize = [this]() {
        return QJsonDocument(conversationHistory_).toJson(QJsonDocument::Compact).size();
    };

    while (conversationHistory_.size() > 2
           && (conversationHistory_.size()
                   > AIConversationLimits::kMaximumMessages
               || serializedSize() > kMaximumConversationHistoryBytes))
    {
        conversationHistory_.removeAt(0);
        conversationHistory_.removeAt(0);
    }

    while (!conversationHistory_.isEmpty()
           && serializedSize() > kMaximumConversationHistoryBytes)
    {
        bool shortened = false;
        for (int index = 0; index < conversationHistory_.size(); ++index)
        {
            QJsonObject message = conversationHistory_[index].toObject();
            const QString content = message.value(QStringLiteral("content")).toString();
            const int contentBytes = content.toUtf8().size();
            if (contentBytes <= 1)
                continue;
            message.insert(QStringLiteral("content"),
                           boundedUtf8(content, qMax(1, contentBytes / 2)));
            conversationHistory_[index] = message;
            shortened = true;
        }
        if (!shortened)
        {
            conversationHistory_ = {};
            break;
        }
    }
}
void AIAssistantService::persistConversationHistory()
{
    if (!conversationStore_ || conversationScope_.isEmpty())
        return;
    const auto result =
        conversationStore_->save(
            conversationStorageScope(), conversationHistory_);
    if (!result.success)
        emit diagnosticReady(AIServiceDiagnostic{result.code, result.message});
}
QString AIAssistantService::conversationStorageScope() const
{
    if (conversationScope_.isEmpty()
        || !conversationSessionStore_
        || conversationSessionCatalog_.activeSessionId.isEmpty()
        || conversationSessionCatalog_.activeSessionId
            == QLatin1String(kDefaultConversationSessionId))
    {
        return conversationScope_;
    }
    return conversationScope_
        + QStringLiteral("\nsession:")
        + conversationSessionCatalog_.activeSessionId;
}
void AIAssistantService::loadConversationHistory()
{
    conversationHistory_ = {};
    if (!conversationStore_ || conversationScope_.isEmpty())
        return;

    const auto result =
        conversationStore_->load(conversationStorageScope());
    if (result.status
        == IAIConversationStore::LoadResult::Status::Loaded)
    {
        conversationHistory_ = result.messages;
        trimConversationHistory();
    }
    else if (result.status
                 == IAIConversationStore::LoadResult::Status::Invalid
             || result.status
                 == IAIConversationStore::LoadResult::Status::IoError)
    {
        emit diagnosticReady(
            AIServiceDiagnostic{result.code, result.message});
    }
}
void AIAssistantService::emitConversationSessions()
{
    emit conversationSessionsChanged(
        conversationSessions(),
        conversationSessionCatalog_.activeSessionId);
}
void AIAssistantService::updateActiveConversationMetadata(
    const QString& firstUserMessage)
{
    if (!conversationSessionStore_
        || conversationSessionCatalog_.activeSessionId.isEmpty())
    {
        return;
    }

    const QString localActiveSessionId =
        conversationSessionCatalog_.activeSessionId;
    AIConversationSessionCatalog persisted;
    const auto result = conversationSessionStore_->mutate(
        conversationScope_,
        [&](const AIConversationSessionLoadResult& current,
            AIConversationSessionCatalog* replacement,
            bool* shouldPersist) {
            if (current.status
                != AIConversationSessionLoadResult::Status::Loaded)
            {
                return conversationSessionFailure(
                    current.code.isEmpty()
                        ? QStringLiteral(
                              "AI_CONVERSATION_SESSIONS_UNAVAILABLE")
                        : current.code,
                    current.message.isEmpty()
                        ? QStringLiteral(
                              "Conversation sessions are unavailable.")
                        : current.message);
            }

            persisted = current.catalog;
            auto found = std::find_if(
                persisted.sessions.begin(),
                persisted.sessions.end(),
                [&localActiveSessionId](
                    const AIConversationSessionRecord& session) {
                    return session.id == localActiveSessionId;
                });
            if (found == persisted.sessions.end())
            {
                return conversationSessionFailure(
                    QStringLiteral(
                        "AI_CONVERSATION_SESSION_NOT_FOUND"),
                    QStringLiteral(
                        "The active conversation session was not found."));
            }

            found->updatedAtMs =
                qMax(found->updatedAtMs, nowMs());
            if (found->title
                == QLatin1String("New Conversation"))
            {
                QString title = firstUserMessage.simplified();
                title = boundedUtf8(
                    title,
                    AIConversationSessionLimits::kMaximumTitleBytes);
                if (!title.isEmpty())
                    found->title = title;
            }
            *replacement = persisted;
            *shouldPersist = true;
            return conversationSessionSuccess();
        });
    if (!result.success)
    {
        emit diagnosticReady(
            AIServiceDiagnostic{result.code, result.message});
        return;
    }

    conversationSessionCatalog_ = persisted;
    conversationSessionCatalog_.activeSessionId =
        localActiveSessionId;
    emitConversationSessions();
}
void AIAssistantService::clearActiveTurn()
{
    activeApiKey_.clear();
    activeRequestId_.clear();
    activePermissionProfile_.clear();
    activeBaseSystemPrompt_.clear();
    activeSystemPrompt_.clear();
    activeModel_.clear();
    activeEndpoint_.clear();
    activeTools_.clear();
    activeRetrievedMemories_.clear();
    activeUsedDurableMemoryIds_.clear();
    activeContextWindowTokens_ = 0;
    activeHistoryMessageCount_ = 0;
    transcript_ = {};
    resetTurnRuntime(TurnState::Idle);
}

}
