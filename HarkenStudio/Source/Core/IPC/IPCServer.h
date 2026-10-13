
#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonDocument>
#include <QVector>
#include <QMap>
#include <QSet>
#include <atomic>
#include <functional>
#include <memory>

#include <optional>

#include "IPCBinaryProtocol.h"
#include "ActionManager.h"
#include "AI/AIDispatchPolicy.h"

class QTcpServer;
class QTcpSocket;

namespace VNX {

class MainWindow;
class AudioEngine;
class ProjectManager;
class AssetImporter;
class AssetConverter;
struct MediaConversionRequest;
struct ProjectData;
class InspectorPanel;
class VQLEngine;
class VBus;
}

namespace VNX {

class IPCServer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(int clientCount READ clientCount NOTIFY clientCountChanged)

public:
    static constexpr int DEFAULT_PORT = 9527;
    
    enum class AppState { Starting, ProjectPicker, Ready };
    enum class DispatchOrigin { Internal, IPC, AI };

    struct IPCDispatchContext
    {
        ActionSource source = ActionSource::Internal;
        DispatchOrigin origin = DispatchOrigin::Internal;
        QString requestId;
        QString permissionProfile;
        QString toolProfile;
        QString grantId;
        bool preview = false;
        // When true, AI-origin dispatches skip the AI dispatch policy gates.
        // Non-AI origins never consult this flag.
        bool unrestricted = false;
    };

    explicit IPCServer(AudioEngine* engine, QObject* parent = nullptr);
    ~IPCServer() override;
    
    void setAppState(AppState state) { appState_ = state; }
    AppState appState() const { return appState_; }

    bool start(int port = DEFAULT_PORT);
    void stop();
    bool isRunning() const;
    int port() const { return port_; }
    int clientCount() const { return clients_.size(); }

    bool isDispatchingCommand() const { return ipcDispatchDepth_ > 0; }

    void setProjectManager(ProjectManager* projectManager);
    
    void setMainWindow(MainWindow* mainWindow) { mainWindow_ = mainWindow; }

    void setAssetImporter(AssetImporter* importer) { assetImporter_ = importer; }
    void setAssetConverter(AssetConverter* converter) { assetConverter_ = converter; }

    QMap<quintptr, QJsonObject>* activePlaybacksMap() { return &activePlaybacks_; }

    void broadcastEvent(const QString& eventName, const QJsonObject& data);

    QJsonObject dispatchCommand(const QString& cmd, const QJsonObject& args);
    QJsonObject dispatchCommand(const QString& cmd, const QJsonObject& args,
                                const IPCDispatchContext& context);
    QByteArray aiBindingFor(const QString& cmd, const QJsonObject& args,
                            const IPCDispatchContext& context) const;
    QString issueAIGrant(const QString& cmd, const QJsonObject& args,
                         const IPCDispatchContext& context, int ttlMs = 300000);
    void revokeAIGrants(const QString& requestId);
    QStringList commandNames() const;
    QStringList allCommandNames() const;
    // Membership test against the legacy handler table without materializing the key list.
    bool hasCommandHandler(const QString& cmd) const { return commandHandlers_.contains(cmd); }
    bool isReadOnlyCommand(const QString& cmd) const;

    VQLEngine* vqlEngine() const;

signals:
    void runningChanged(bool running);
    void clientCountChanged(int count);
    void clientConnected(const QString& address);
    void clientDisconnected(const QString& address);
    void commandReceived(const QString& command, const QJsonObject& args);
    void errorOccurred(const QString& error);
    void aiResponseReceived(const QString& response);
    void aiAgentEvent(const QJsonObject& event);

    void projectOpenedViaIPC(const QString& path);

private slots:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();
    void onClientError();

    friend void setupTransactionCallbacks(IPCServer* ipcServer);
    friend void registerReadonlyActions(ActionManager& mgr, IPCServer* ipcServer);

private:
    void assertAIPolicyThread() const;
    std::optional<QJsonObject> enforceAIPolicyGate(const QString& cmd, const QJsonObject& args,
                                                   const IPCDispatchContext& context,
                                                   bool consumeGrant);
    TransactionExecutionContext transactionContextFor(
        const IPCDispatchContext& context) const;
    IPCDispatchContext dispatchContextFrom(
        const TransactionExecutionContext& context) const;

    using CommandHandler = std::function<QJsonObject(const QJsonObject&)>;
    
    void registerCommandHandlers();

    void registerSystemHandlers();
    void registerProjectHandlers();
    void registerMixerHandlers();
    void registerEditorHandlers();
    void registerSpatialHandlers();
    void registerAssetHandlers();
    void registerContainerHandlers();
    void registerAdvancedHandlers();
    
    QJsonObject handleCommand(const QString& cmd, const QJsonObject& args);
    QJsonObject invokeCommandHandler(const QString& cmd, const QJsonObject& args);
    QJsonObject executeActionManagerCommand(const QString& cmd, const QJsonObject& args,
                                            ActionSource source);
    
    void sendResponse(QTcpSocket* client, const QJsonObject& response);
    
    QJsonObject successResponse(const QJsonObject& result = QJsonObject());
    
    QJsonObject errorResponse(const QString& code, const QString& message, 
                              const QString& suggestion = QString());

    VBus* resolveBusByName(const QString& rawName) const;

    bool isJsonCommandAvailableInCurrentState(const QString& cmd) const;
    bool isBinaryCommandAvailableInCurrentState(IPCBinaryCommandType cmd) const;
    QJsonObject commandUnavailableInCurrentStateResponse(const QString& cmd);
    bool isPreviewSupportedCommand(const QString& cmd) const;
    VBus* resolveBusByNameForTx(const QString& rawName) const;
    bool prepareRollbackStepForAction(const QString& cmd, const QJsonObject& args,
                                      QJsonObject* rollbackStep, QJsonObject* previewEntry, QString* error);
    bool executeRollbackStep(const QJsonObject& step, QString* error);

    
    QJsonObject handlePing(const QJsonObject& args);
    QJsonObject handleGetVersion(const QJsonObject& args);
    QJsonObject handleGetStatus(const QJsonObject& args);
    
    QJsonObject handleInit(const QJsonObject& args);
    QJsonObject handleShutdown(const QJsonObject& args);
    QJsonObject handleStartOutput(const QJsonObject& args);
    QJsonObject handleStopOutput(const QJsonObject& args);
    
    QJsonObject handleStopAll(const QJsonObject& args);
    
    QJsonObject handleSetMasterVolume(const QJsonObject& args);

    QJsonObject handleListModulators(const QJsonObject& args);
    QJsonObject handleListModulatorChains(const QJsonObject& args);

    QJsonObject handleGetModulatorPresets(const QJsonObject& args);
    QJsonObject handleApplyModulatorPreset(const QJsonObject& args);
    QJsonObject handleSaveModulatorUserPreset(const QJsonObject& args);
    QJsonObject handleDeleteModulatorUserPreset(const QJsonObject& args);

    QJsonObject handleGetStreamStatus(const QJsonObject& args);
    QJsonObject handleGetStreamPosition(const QJsonObject& args);

    QJsonObject handleListCodecs(const QJsonObject& args);
    QJsonObject handleGetCodecInfo(const QJsonObject& args);
    QJsonObject handleGetStreamDeviceInfo(const QJsonObject& args);
    QJsonObject handleGetStreamDeviceCount(const QJsonObject& args);

    QJsonObject handleGetPackMetrics(const QJsonObject& args);
    QJsonObject handleGetPackLoadHistory(const QJsonObject& args);
    QJsonObject handleGetActionRouting(const QJsonObject& args);
    QJsonObject handleSetPackRoutingPriority(const QJsonObject& args);
    QJsonObject handleResolveActionPacks(const QJsonObject& args);
    QJsonObject handleValidateMultipackCollisions(const QJsonObject& args);
    QJsonObject handleGetCodecCapabilities(const QJsonObject& args);

    QJsonObject handleSetListener(const QJsonObject& args);
    QJsonObject handleGet3DScene(const QJsonObject& args);

    QJsonObject handleLoadPack(const QJsonObject& args);
    QJsonObject handleUnloadPack(const QJsonObject& args);
    QJsonObject handleGetLoadedPacks(const QJsonObject& args);
    
    QJsonObject handleGetPanelRect(const QJsonObject& args);
    QJsonObject handleGetProjectState(const QJsonObject& args);
    QJsonObject handleGetPlaybacks(const QJsonObject& args);

    QJsonObject handleGetUiTree(const QJsonObject& args);
    QJsonObject handleFindWidget(const QJsonObject& args);
    QJsonObject handleGrabWidget(const QJsonObject& args);
    QJsonObject handleGetWidgetValue(const QJsonObject& args);
    QJsonObject handleGetWidgetChildren(const QJsonObject& args);
    QJsonObject handleGetTableData(const QJsonObject& args);
    QJsonObject handleGetListItems(const QJsonObject& args);
    QJsonObject handleGetTreeData(const QJsonObject& args);
    QJsonObject handleFlushUIEvents(const QJsonObject& args);
    QJsonObject handleGetFocusedWidget(const QJsonObject& args);
    QJsonObject handleGetLastInteractedWidget(const QJsonObject& args);

    QJsonObject handleClickWidget(const QJsonObject& args);
    QJsonObject handleKeyPress(const QJsonObject& args);
    QJsonObject handleSetWidgetValue(const QJsonObject& args);
    QJsonObject handleSelectTableRow(const QJsonObject& args);
    QJsonObject handleTriggerMenuAction(const QJsonObject& args);
    QJsonObject handleAnswerDialog(const QJsonObject& args);
    

    QJsonObject handleGetEngineSnapshot(const QJsonObject& args);
    QJsonObject handleGetActiveVoices(const QJsonObject& args);
    QJsonObject handleGetVoiceNodeVirtualStates(const QJsonObject& args);
    QJsonObject handleGetBusTopologyPlanCount(const QJsonObject& args);
    QJsonObject handleGetVoiceMetadataChain(const QJsonObject& args);
    QJsonObject handleGetBusInfo(const QJsonObject& args);
    QJsonObject handleGetPerformance(const QJsonObject& args);

    QJsonObject handleGetCpuBudgetState(const QJsonObject& args);
    QJsonObject handleSetCpuBudgetConfig(const QJsonObject& args);
    QJsonObject handleGetRenderPlan(const QJsonObject& args);
    QJsonObject handleGetJobStats(const QJsonObject& args);
    QJsonObject handleGetObjectAudioStatus(const QJsonObject& args);
    QJsonObject handleSetSpatialFallbackPolicy(const QJsonObject& args);

    QJsonObject handleGetMemoryBudgetStatus(const QJsonObject& args);
    QJsonObject handleSetMemoryBudgetLimit(const QJsonObject& args);
    QJsonObject handleSetListenerOutputBus(const QJsonObject& args);
    QJsonObject handleGetListenerOutputBus(const QJsonObject& args);

    QJsonObject handleSwitchView(const QJsonObject& args);
    QJsonObject handleGetCurrentView(const QJsonObject& args);
    QJsonObject handleGetCurrentLayout(const QJsonObject& args);
    QJsonObject handleGetLayoutState(const QJsonObject& args);
    QJsonObject handleResetLayout(const QJsonObject& args);
    QJsonObject handleGetInspectorContext(const QJsonObject& args);
    InspectorPanel* resolveInspectorForRouting(const QJsonObject& args, QJsonObject& errOut);
    QJsonObject handleSaveNamedLayout(const QJsonObject& args);
    QJsonObject handleLoadNamedLayout(const QJsonObject& args);
    QJsonObject handleListNamedLayouts(const QJsonObject& args);
    QJsonObject handleDeleteNamedLayout(const QJsonObject& args);
    QJsonObject handleListPanels(const QJsonObject& args);
    QJsonObject handleTogglePanel(const QJsonObject& args);

    QJsonObject handleGetRecentLogs(const QJsonObject& args);
    
    QJsonObject handleCreateProject(const QJsonObject& args);
    QJsonObject handleOpenProject(const QJsonObject& args);
    QJsonObject handleGetProjectInfo(const QJsonObject& args);
    QJsonObject handleListPacks(const QJsonObject& args);
    QJsonObject handleListItems(const QJsonObject& args);
    QJsonObject handleGetItemInfo(const QJsonObject& args);
    QJsonObject handleGetActionPackPlan(const QJsonObject& args);
    QJsonObject handleGetRuntimeCapabilityMatrix(const QJsonObject& args);
    QJsonObject handleGetLastPackBuildRecords(const QJsonObject& args);

    QJsonObject handleExportPack(const QJsonObject& args);
    QJsonObject handleGetPackBuildStatus(const QJsonObject& args);
    QJsonObject handleCancelPackBuild(const QJsonObject& args);
    void finishPackBuild();
    QJsonObject handleGetPackInfo(const QJsonObject& args);
    QJsonObject handleValidatePack(const QJsonObject& args);
    QJsonObject handleGetPackDependencies(const QJsonObject& args);
    QJsonObject handleGetUnusedAssets(const QJsonObject& args);

    QJsonObject handleGetMixerLevels(const QJsonObject& args);
    
    QJsonObject handleGetNodeGraph(const QJsonObject& args);
    QJsonObject handleGetSignalChain(const QJsonObject& args);
    QJsonObject handleSetBlueprintLayer(const QJsonObject& args);
    QJsonObject handleGetBlueprintLayers(const QJsonObject& args);
    QJsonObject handleGetReferencingActions(const QJsonObject& args);



    QJsonObject handleGetActionDefinition(const QJsonObject& args);
    QJsonObject handleListActions(const QJsonObject& args);

    QJsonObject handleUndo(const QJsonObject& args);
    QJsonObject handleRedo(const QJsonObject& args);
    QJsonObject handleCanUndo(const QJsonObject& args);
    QJsonObject handleCanRedo(const QJsonObject& args);

    QJsonObject handleGetAppState(const QJsonObject& args);

    QJsonObject handleAIResponse(const QJsonObject& args);
    QJsonObject handleAIEvent(const QJsonObject& args);

    QJsonObject handleImportAudio(const QJsonObject& args);
    QJsonObject handleTranscodeAsset(const QJsonObject& args);
    QJsonObject handleGetAssetInfo(const QJsonObject& args);
    QJsonObject handleCleanupCache(const QJsonObject& args);
    QJsonObject handleListAssets(const QJsonObject& args);

    QJsonObject handleGetConversionSettings(const QJsonObject& args);
    QJsonObject handleSetConversionSettings(const QJsonObject& args);
    QJsonObject handleGetPlatformProfile(const QJsonObject& args);
    QJsonObject handleSetPlatformProfile(const QJsonObject& args);

    QJsonObject handleGetMediaStatus(const QJsonObject& args);
    QJsonObject handleRescanMediaSources(const QJsonObject& args);
    QJsonObject handleBatchReplaceMedia(const QJsonObject& args);
    QJsonObject handleBatchConvertMedia(const QJsonObject& args);

    QJsonObject handleQuery(const QJsonObject& args);
    QJsonObject handleBatch(const QJsonObject& args);

    void handleBinaryFrame(QTcpSocket* client, const QByteArray& frameData);
    void sendBinaryResponse(QTcpSocket* client, IPCBinaryStatus status,
                            uint32_t seqId, const QByteArray& payload = {});

    QJsonObject handleGetContainerInfo(const QJsonObject& args);

    QTcpServer* server_ = nullptr;
    AudioEngine* engine_ = nullptr;
    ProjectManager* projectManager_ = nullptr;
    MainWindow* mainWindow_ = nullptr;
    AssetImporter* assetImporter_ = nullptr;
    AssetConverter* assetConverter_ = nullptr;
    QVector<QTcpSocket*> clients_;
    QSet<QTcpSocket*> commandOnlyClients_;
    int port_ = DEFAULT_PORT;
    AppState appState_ = AppState::Starting;
    quint64 actionCounter_ = 0;
    int ipcDispatchDepth_ = 0;
    QVector<IPCDispatchContext> dispatchContextStack_;
    AIDispatchPolicy aiDispatchPolicy_;
    QMap<QString, CommandHandler> commandHandlers_;
    QMap<quintptr, QJsonObject> activePlaybacks_;
    bool packBuildInProgress_ = false;
    int packBuildProgress_ = 0;
    QString packBuildMessage_;
    QJsonObject packBuildResult_;
    std::function<void()> packBuildCompletion_;
    std::weak_ptr<ProjectData> packBuildProject_;
    QThread* packBuildThread_ = nullptr;
    std::shared_ptr<std::atomic_bool> packBuildCancel_;
    bool transcodeInProgress_ = false;
    QJsonObject transcodeResult_;
    std::shared_ptr<MediaConversionRequest> transcodeRequest_;
    QThread* transcodeThread_ = nullptr;
    std::unique_ptr<VQLEngine> vqlEngine_;
    QMap<QTcpSocket*, QByteArray> binaryBuffers_;
};

}
