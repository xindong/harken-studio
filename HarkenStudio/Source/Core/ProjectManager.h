
#pragma once

#include <QObject>
#include <QString>
#include <QSet>
#include <QPointer>
#include <QList>
#include <QUuid>
#include <memory>
#include <optional>
#include <span>
#include <functional>
#include <vector>
#include "AuditionOverlay.h"
#include "BindingSemantics.h"
#include "AssetPipeline/AssetTypes.h"
#include "IntegrityScanner.h"
#include "ProjectIndex.h"
#include "TransitionRehearsal.h"
#include "VariableRehearsal.h"
#include "ItemTypeCatalog.h"
#include "ProjectTypes.h"
#include "Components/VBus.h"
#include "ProjectSerializer.h"
#include "FileWatcher.h"
#include "StartupProgress.h"

class QUndoCommand;

namespace harkenbuscompiler {
struct CompiledBusResultV1;
}

namespace HarkenEditor::Bridge {
struct CurrentWorldSoundPreparation;
}

namespace VNX {

class ProjectIndex;
class FolderSaveState;
class ProjectItemSaveState;
class IntegrityScanner;
class PlacementContext;
class VMixNode;
struct PreparedProject;

class ActionManager;
class AudioEngine;
class MainWindow;
class InspectorPanel;
class InspectorPanelPrivate;
class ProjectExplorer;
class Hierarchy;
class CreateProjectItemCommand;
class DeleteProjectItemCommand;
class MoveItemCommand;
class SequenceReorderCommand;
class IPCServer;
class PropertyEditSession;
struct ActionResult;
struct FieldGroupDefinitionSnapshot;

struct PreparedProject
{
    PreparedProject();
    PreparedProject(PreparedProject&& other) noexcept;
    PreparedProject& operator=(PreparedProject&& other) noexcept;
    PreparedProject(const PreparedProject&) = delete;
    PreparedProject& operator=(const PreparedProject&) = delete;
    ~PreparedProject();

    void releaseUpgradeRecovery() { upgradePending = false; }
    void rollbackUpgrade();

    std::unique_ptr<ProjectData> project;
    std::unique_ptr<FolderSaveState> folderSaveState;
    std::unique_ptr<ProjectIndex> index;
    std::unique_ptr<IntegrityScanner> integrityScanner;
    IntegrityScanner::Report integrityReport;
    QList<AssetInfo> assets;
    QStringList diagnostics;
    QString error;
    bool rebuildAssetRegistry = false;
    int audioLoadFailures = 0;
    ProjectUpgradeResult upgrade;
    bool upgradePending = false;
};

class ProjectManager : public QObject
{
    Q_OBJECT

public:
    static ProjectManager& instance();

    friend class InspectorPanel;
    friend class InspectorPanelPrivate;
    friend class IPCServer;
    friend class MainWindow;
    friend class AudioEngine;
    friend class BusController;
    friend class CreateProjectItemCommand;
    friend class DeleteProjectItemCommand;
    friend class MoveItemCommand;
    friend class SequenceReorderCommand;
    friend class BlueprintSendConnectCommand;
    friend class BlueprintSendDisconnectCommand;
    friend class PropertyEditSession;
    friend class TemplateApplyService;
    friend void registerMixerActions(ActionManager&, AudioEngine*,
                                      ProjectManager*, MainWindow*);
    friend ActionResult applyBusPropertiesArgs(const QJsonObject&, const QObject*);
    friend ActionResult handleAudioItemProperty(ProjectManager*, ProjectItem*,
                                                const QString&, const QJsonObject&,
                                                const QObject*);
    friend ActionResult handleBusItemProperty(ProjectManager*, ProjectItem*,
                                              const QString&, const QJsonObject&,
                                              const QObject*);
    friend ActionResult handlePlayableItemProperty(ProjectManager*, ProjectItem*,
                                                   const QString&, const QJsonObject&,
                                                   const QObject*);
    friend ActionResult handleNotesItemProperty(ProjectManager*, ProjectItem*,
                                                const QString&, const QJsonObject&,
                                                const QObject*);
    friend void registerContainerActions(ActionManager&, ProjectManager*);
    friend void registerProjectActions(ActionManager&, ProjectManager*, ProjectExplorer*, std::function<Hierarchy*()>);

    static void resyncItemToRuntime(ProjectItem* item);
    static void resyncSourceToRuntime(uint32_t sourceId);
    static void syncActionNameForPreview(ProjectItem* actionItem);
    static void syncProjectToRuntime(ProjectData* project);
    static bool prepareRuntimeSounds(const ProjectData& project,
        HarkenEditor::Bridge::CurrentWorldSoundPreparation& preparation,
        QString* error = nullptr);
    static void syncProjectToRuntimeAsync(ProjectData* project,
                                          QObject* context,
                                          std::function<void()> finished);

    static void applyRenameSideEffects(ProjectItem* item,
                                       const QString& fromName,
                                       const QString& toName);
    static bool hasRuntimePresence(const ProjectItem* item);
    static void unregisterItemFromRuntime(ProjectItem* item);

    static DeletedSubtreeRefLog cleanupIllegalPlaybackReferences(ProjectData* project);
    static DeletedSubtreeRefLog cleanupReferencesToDeletedSubtree(ProjectItem* deletedRoot,
                                                                  ProjectData* project);
    static void restoreReferencesFromLog(const DeletedSubtreeRefLog& log,
                                         ProjectData* project);

    // Undo-layer detach/attach for root-level objects. detach removes the
    // binding and hands its whole record to the caller for safekeeping;
    // attach reinserts a previously detached record. Neither emits signals
    // or touches parentRef.
    static ProjectFileBinding*   findRootBinding(ProjectData* project, const ProjectItem* item);
    static bool                  detachRootBinding(ProjectData* project, const ProjectItem* item, RootBindingSlot* out);
    static bool                  attachRootBinding(ProjectData* project, const RootBindingSlot& slot);

    static ProjectFolderBinding* findRootFolderBinding(ProjectData* project, const PhysicalFolder* folder);
    static bool                  detachRootFolderBinding(ProjectData* project, const PhysicalFolder* folder, RootFolderSlot* out);
    static bool                  attachRootFolderBinding(ProjectData* project, const RootFolderSlot& slot);

    void requestResync(ProjectItem* item);
    void flushPendingResyncs();

    static constexpr const char* kLayers                 = ItemTypeCatalog::kLayersFolderName;

    static QString bucketDisplayName(const QString& folderName);

    FileWatcher* fileWatcher() { return &m_fileWatcher; }

    AuditionOverlay& auditionOverlay() { return m_auditionOverlay; }
    const AuditionOverlay& auditionOverlay() const { return m_auditionOverlay; }

    VariableRehearsal& variableRehearsal() { return m_variableRehearsal; }
    const VariableRehearsal& variableRehearsal() const { return m_variableRehearsal; }

    TransitionRehearsal& transitionRehearsal() { return m_transitionRehearsal; }
    const TransitionRehearsal& transitionRehearsal() const { return m_transitionRehearsal; }

    bool createProject(const ProjectConfig& config);
    bool loadProject(const QString& filePath, bool allowUpgrade = false,
                     QString* error = nullptr);
    static PreparedProject prepareProject(const QString& filePath,
                                          bool allowUpgrade = false,
                                          ProjectUpgradeProgressCallback upgradeProgress = {},
                                          ProjectUpgradeApprovalCallback upgradeApproval = {});
    static PreparedProject prepareProject(const ProjectConfig& config);
    bool commitPreparedProject(PreparedProject&& prepared);
    void publishProjectOpened();
    void startWatchingCurrentProject();
    bool runtimePrepared() const { return m_runtimePrepared; }
    void setStartupProgressCallback(
        std::function<void(StartupPhase)> callback);
    bool saveProject();
    bool closeProject();

    ProjectData* currentProject() const { return m_currentProject.get(); }
    std::shared_ptr<ProjectData> currentProjectLease() const {
        return m_currentProject;
    }
    bool isProjectOpen() const { return m_currentProject != nullptr; }
    bool isClosing() const { return m_closing; }
    bool hasPendingDestruction() const { return static_cast<bool>(m_pendingDestruction); }
    int pendingResyncCount() const { return static_cast<int>(m_pendingResyncs.size()); }

    SignalChainView buildSignalChainView(const ProjectItem* item) const;
    SignalChainView buildSignalChainView(const ProjectItem* item,
                                         const PlacementContext& placement) const;
    static SignalChainView buildSignalChainView(const ProjectData* project, const ProjectItem* item);
    static SignalChainView buildSignalChainView(const ProjectData* project,
                                                const ProjectItem* item,
                                                const PlacementContext& placement);
    SignalChainView buildSignalChainViewForBus(uint32_t busID) const;

    struct ActionReference {
        QString actionName;
        uint32_t actionObjectID = 0;
        QString actionType;
    };
    std::vector<ActionReference> findReferencingActions(uint32_t objectID) const;
    static std::vector<ActionReference> findReferencingActions(const ProjectData* project, uint32_t objectID);


    static bool canBeChildOf(ProjectItem::Type childType, ProjectItem::Type parentType);
    static bool canAcceptAnyChild(ProjectItem::Type parentType);

    static std::vector<ProjectItem::Type> allowedChildTypesOf(ProjectItem::Type parentType);

    static std::vector<ProjectItem::Type> allowedTopLevelTypesForBucket(const QString& bucketFolderName);

    static bool bucketAllowsFolder(const QString& bucketFolderName);

    static bool canAcceptImportedAudioParent(const ProjectItem* parent);

    PhysicalFolder* createFolder(PhysicalFolder* parent, const QString& name,
                                  const QString& bucket);
    bool validateFolderRename(PhysicalFolder* folder, const QString& name,
                               QString* error = nullptr) const;
    bool renameFolder(PhysicalFolder* folder, const QString& name);
    bool moveFolder(PhysicalFolder* folder, PhysicalFolder* newParent, const QString& bucket);
    bool moveEntriesToFolder(const QList<ProjectItem*>& items,
                             const QList<PhysicalFolder*>& folders,
                             const QString& bucket, const QString& relativePath);
    bool canDeleteEntries(const QList<ProjectItem*>& items,
                            const QList<PhysicalFolder*>& folders) const;
    bool isFolderDirty(const PhysicalFolder* folder) const;
    QSet<QString> bucketsWithPendingFileChanges() const;
    bool deleteEntries(const QList<ProjectItem*>& items, const QList<PhysicalFolder*>& folders);

    ProjectItem* createItem(ProjectItem* parent, const QString& name, ProjectItem::Type type,
                            const QString& path = QString(),
                            const QString& bucketFolderHint = QString(),
                            QUndoCommand* undoParent = nullptr,
                            VariableKind variableKind = VariableKind::Smooth);
    ProjectItem* createTrackWithClip(ProjectItem* piece, const QString& name, Clip clip);
    ProjectItem* createAudioItem(ProjectItem* parent, const QString& name, const QString& relativePath, uint32_t sourceID);
    // Root-level counterpart of createAudioItem: does not push an undo command.
    ProjectItem* createRootAudioItem(const QString& name, const QString& relativePath, uint32_t sourceID);
    bool renameItem(ProjectItem* item, const QString& newName, QString* error = nullptr);
    bool validateItemRename(ProjectItem* item, const QString& newName,
                             QString* error = nullptr) const;
    enum class RenamePolicy { ResolveConflicts, Exact };
    bool renameItemInternal(ProjectItem* item, const QString& newName,
                            RenamePolicy policy = RenamePolicy::Exact, QString* error = nullptr);
    bool deleteItem(ProjectItem* item);
    bool canCreateItem(ProjectItem* parent, ProjectItem::Type type,
                       const QString& bucketFolderHint = {}) const;
    bool canCreateParentContainer(const QList<ProjectItem*>& selection,
                                   ProjectItem::Type containerType,
                                   QString* error = nullptr) const;
    int  deleteItems(const QList<ProjectItem*>& items);
    QList<ProjectItem*> uniqueItemsByGuid(const QList<ProjectItem*>& items) const;
    QList<ProjectItem*> normalizeSubtreeItems(const QList<ProjectItem*>& items) const;
    bool canDeleteItems(const QList<ProjectItem*>& items) const;
    bool moveItem(ProjectItem* item, ProjectItem* newParent);
    bool moveItems(const QList<ProjectItem*>& items, ProjectItem* newParent);
    bool moveItemToBucketRoot(ProjectItem* item, const QString& bucketFolder);
    ProjectItem* duplicateItem(ProjectItem* src);
    std::vector<ProjectItem*> duplicateItems(const QList<ProjectItem*>& items);
    static bool isDuplicable(const ProjectItem* item);
    bool canDuplicateItems(const QList<ProjectItem*>& items) const;

    static bool isReferencedAttenuation(ProjectItem* item, int* refCount = nullptr,
                                       QString* error = nullptr);

    bool setItemColor(ProjectItem* item, uint32_t color);
    bool setItemsColor(const QList<ProjectItem*>& items, uint32_t color);

    bool applyItemPropertyLive(ProjectItem* item, const QString& propertyName, const QVariant& value);
    bool applyBusPropertyLive(ProjectItem* busItem, const QString& propertyName, const QVariant& value);

    static uint32_t resolveEffectiveOutputBusId(const ProjectItem* item);
    float aggregatePitchForItem(const ProjectItem* item) const;
    void pushItemPitchLive(ProjectItem* item);
    float aggregateVolumeDbForItem(const ProjectItem* item) const;
    float effectiveOutputVolumeDbForItem(const ProjectItem* item) const;
    float effectiveBusVolumeDb(const ProjectItem* busItem) const;
    void pushItemVolumeLive(ProjectItem* item);
    void propagateItemVolumeLive(ProjectItem* item);
    std::vector<Send> aggregateVoiceSendsForItem(const ProjectItem* item) const;
    void pushItemSendsLive(ProjectItem* item);
    void propagateItemSendsLive(ProjectItem* item);
    static float runtimeSpatMixForValue(float spatializationMix);
    void pushItemSpatMixLive(ProjectItem* item);
    void propagateItemSpatMixLive(ProjectItem* item);
    static float runtimeDistanceScaleForValue(float distanceScaling);
    void pushItemDistanceScaleLive(ProjectItem* item);
    void propagateItemDistanceScaleLive(ProjectItem* item);


    ProjectItem* createParentContainer(const QList<ProjectItem*>& selection,
                                       ProjectItem::Type containerType,
                                       const QString& name);

    ProjectItem* createActionTargeting(ProjectItem* target,
                                      VActionStep::Type actionType,
                                      const QString& actionName);
    std::vector<ProjectItem*> createActionsTargeting(const QList<ProjectItem*>& targets,
                                                     VActionStep::Type actionType);
    bool canCreateActionsTargeting(const QList<ProjectItem*>& targets,
                                   VActionStep::Type actionType) const;


    enum class ClipboardOp : uint8_t { None, Cut, Copy };

    static bool isClipboardEligible(const ProjectItem* item);
    bool canSetClipboard(const QList<ProjectItem*>& items) const;
    int  setClipboard(const QList<ProjectItem*>& items, ClipboardOp op);
    void clearClipboard();
    bool clipboardEmpty() const { return m_clipboardOp == ClipboardOp::None || m_clipboardGuids.empty(); }
    ClipboardOp clipboardOp() const { return m_clipboardOp; }
    int  clipboardCount() const { return static_cast<int>(m_clipboardGuids.size()); }

    std::vector<const ProjectItem*> resolveClipboardSources() const;

    std::vector<ProjectItem*> pasteClipboard(ProjectItem* target);
    std::vector<ProjectItem*> pasteClipboardAtBucketRoot(const QString& bucketFolder);
    bool canPasteClipboard(ProjectItem* target) const;
    ProjectItem* resolvePasteParent(ProjectItem* target, ProjectItem::Type childType) const;
    bool canPasteClipboardAtBucketRoot(const QString& bucketFolder) const;

    bool pasteProperties(const ProjectItem* source, const QList<ProjectItem*>& targets);
    bool canPasteProperties(const ProjectItem* source, const QList<ProjectItem*>& targets) const;
    bool pastePropertiesFromClipboard(const QList<ProjectItem*>& targets);
    bool canPastePropertiesFromClipboard(const QList<ProjectItem*>& targets) const;

    using NameNamespace = ItemTypeCatalog::NameNamespace;
    static NameNamespace namespaceForType(ProjectItem::Type type);

    bool isNameUnique(const QString& name,
                      ProjectItem::Type type,
                      const ProjectItem* excludeItem = nullptr) const;

    const ProjectItem* findNameConflict(const QString& name,
                                        ProjectItem::Type type,
                                        const ProjectItem* excludeItem = nullptr) const;

    static QString nameConflictMessage(const QString& name,
                                       ProjectItem::Type type,
                                       const ProjectItem& conflict);

    QString suggestUniqueName(ProjectItem::Type type, const QString& baseName,
                              const ProjectItem* excludeItem = nullptr,
                              const QSet<QString>& reservedNames = {},
                              const ProjectItem* parent = nullptr,
                              const QString& folderPath = {}) const;

    bool isFolderNameUniqueAmongSiblings(
        const PhysicalFolder* parentFolderItem,
        const QString& bucketFolder,
        const QString& name,
        const PhysicalFolder* excludeItem = nullptr) const;

    QString suggestUniqueFolderName(const PhysicalFolder* parentFolderItem,
                                    const QString& bucketFolder,
                                    const QString& baseName,
                                    const PhysicalFolder* excludeItem = nullptr) const;


    bool restoreFieldGroupsInternal(ProjectItem* item,
        const DefinitionFieldGroups& replacement,
        const std::optional<DefinitionFieldGroups>& expectedBefore = {},
        const QObject* originator = nullptr);

    ProjectItem* getMasterBusItem() const;

    harkenstudio::ObjectId busRuntimeHandle(const ProjectItem* item) const;

    harkenstudio::ObjectId busRuntimeHandleForId(uint32_t objectID) const;

    ProjectItem* findBusItem(uint32_t busID) const;

    ProjectItem* findItemByGuid(const QUuid& guid) const;

    ProjectItem* findItemByObjectID(uint32_t objectID) const;

    ProjectItem* findItemByRuntimeStableId(harkenstudio::ObjectId runtimeStableId) const;

    VNX::VBus*       findVBus(uint32_t busID);
    const VNX::VBus* findVBus(uint32_t busID) const;

    std::vector<ProjectItem*> collectBusItems() const;

    uint32_t masterBusID() const;
    ProjectItem* findBusItemByName(const QString& name) const;
    std::vector<ProjectItem*> getChildBusItems(uint32_t parentBusID) const;
    bool busHasIncomingSends(uint32_t busId) const;
    std::vector<IncomingSendInfo> collectIncomingSends(uint32_t busId) const;
    std::vector<MainInputInfo> collectMainInputs(uint32_t busId) const;
    QString getBusPath(uint32_t busID) const;
    bool isDescendantOfBus(uint32_t busID, uint32_t ancestorID) const;

    // Cycle checks on the merged bus flow graph (parent chain + send edges).
    bool sendWouldCycle(uint32_t fromBusID, uint32_t toBusID) const;
    bool sendTargetStructurallySelectable(uint32_t srcObjectID,
                                          uint32_t targetBusID) const;
    bool sendTargetSelectable(uint32_t srcObjectID, uint32_t targetBusID) const;
    bool outputBusSelectable(uint32_t srcObjectID, uint32_t candidateBusID) const;
    bool busMoveWouldCycle(const ProjectItem* moved, const ProjectItem* newParent) const;
    bool validateMoveConstraints(const ProjectItem* item, const ProjectItem* newParent,
                                 QString* reason) const;
    std::optional<DefinitionFieldGroups> captureRootFieldGroups(
        const ProjectItem* item, QString* error = nullptr,
        std::span<const FieldGroupDefinitionSnapshot> replacements = {}) const;

    // Extension point ②: fine-grained per-instance structural placement rules
    // that the coarse type-level canBeChildOf() cannot express (bus depth /
    // fan-out, send-loop safety on reparent). Returns false and fills *err on
    // rejection; non-bus types currently carry no such rules here. `instance`
    // is the bus being moved (nullptr on creation — a fresh bus carries no
    // sends and cannot close a loop).
    bool validateStructuralPlacement(ProjectItem::Type type,
                                     const ProjectItem* parent,
                                     QString* err = nullptr,
                                     const ProjectItem* instance = nullptr) const;


private:
    QList<PhysicalFolder*> normalizeFolderEntries(const QList<PhysicalFolder*>& folders) const;
    bool prepareItemRename(ProjectItem* item, const QString& requestedName, RenamePolicy policy,
                            QString& newName, QString* error) const;
    bool restoreItemInternal(
        ProjectItem* parent,
        const std::shared_ptr<ProjectItem>& item,
        int insertIndex = -1,
        const std::optional<SelectionEntry>& restoredOccurrence = std::nullopt,
        const std::vector<SelectionEntry>* restoredOccurrences = nullptr,
        bool createDefaultClip = true);
    bool ensureTrackChildClip(ProjectItem* track, ProjectItem* child);
    bool deleteItemInternal(ProjectItem* parent, const std::shared_ptr<ProjectItem>& item, int* removedIndex = nullptr,
                            DeletedSubtreeRefLog* outLog = nullptr);
    bool moveItemInternal(
        ProjectItem* item,
        ProjectItem* newParent,
        int insertIndex = -1,
        const std::optional<SelectionEntry>& restoredDestination = std::nullopt,
        std::optional<SelectionEntry>* removedSource = nullptr,
        std::optional<SelectionEntry>* installedDestination = nullptr,
        std::vector<SelectionEntry>* removedSources = nullptr,
        bool restoreEmptySourceOccurrences = false,
        DeletedSubtreeRefLog* outLog = nullptr,
        bool createDefaultClip = true);

    std::shared_ptr<ProjectItem> cloneSubtreeDetached(const ProjectItem* src,
                                                    const ProjectItem* parent,
                                                    const QString& folderPath = {},
                                                    const ProjectSerializer* snapshotSerializer = nullptr);

    bool mutateItemInternal(ProjectItem* item, const std::function<void(ProjectItem*)>& mutator,
                            const QString& propertyName = QString());
    bool writeAudioPropertySilent(ProjectItem* item, const QString& propertyName, const QVariant& value);
    void pushAudioPropertyLive(ProjectItem* item, const QString& propertyName);
    bool setAudioPropertyInternal(ProjectItem* item, const QString& propertyName, const QVariant& value,
                                  const QObject* originator = nullptr);
    bool setItemVolumeInternal(ProjectItem* item, float volume, const QObject* originator = nullptr);
    bool setItemPitchInternal(ProjectItem* item, float pitch, const QObject* originator = nullptr);
    bool setItemMakeUpGainInternal(ProjectItem* item, float makeUpGain, const QObject* originator = nullptr);
    bool setItemLoopCountInternal(ProjectItem* item, uint32_t loopCount, const QObject* originator = nullptr);
    bool setItemOutputBusRefInternal(ProjectItem* item, uint32_t busID, const QObject* originator = nullptr);
    bool reorderItemChildrenInternal(ProjectItem* parent, const std::vector<std::shared_ptr<ProjectItem>>& orderedChildren);

    // Open-time asset reconciliation: drops registry records no tree references,
    // then removes AudioSources files no record maps to.
    void reconcileAssetsOnOpen(int bindingLoadFailures);

    // Extension point ④: per-type runtime lifecycle dispatch. The shared
    // create/delete/move flow drives these; buses mirror into the engine
    // BusTree, other types into the runtime object registry.
    void materializeRuntime(ProjectItem* item, harkenstudio::ObjectId parentRuntimeId);
    void dematerializeRuntime(ProjectItem* item);
    bool reparentRuntime(ProjectItem* item, harkenstudio::ObjectId newParentRuntimeId);
    void materializeBusSubtree(ProjectItem* item, harkenstudio::ObjectId parentRuntimeId);
    void applyBusRuntimeState(ProjectItem* item);
    void syncBusTreeToRuntime(
        ProjectData* project,
        std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1>
            compiledBus = {});

    bool writeBusPropertySilent(ProjectItem* busItem, const QString& propertyName, const QVariant& value);
    void pushBusPropertyLive(ProjectItem* busItem, const QString& propertyName);
    bool setBusVolumeInternal(uint32_t busID, float volumeDb, const QObject* originator = nullptr);
    bool setBusMakeUpGainInternal(uint32_t busID, float makeUpGainDb,
                                  const QObject* originator = nullptr);
    bool setBusPanInternal(uint32_t busID, float pan, const QObject* originator = nullptr);
    bool setBusChannelConfigInternal(uint32_t busID, uint8_t channelConfig);
    bool setBusListenerRelativeInternal(uint32_t busID, bool enabled);
    bool setBusMuteInternal(uint32_t busID, bool mute, const QObject* originator = nullptr);
    bool setBusSoloInternal(uint32_t busID, bool solo, const QObject* originator = nullptr);
    bool setBusPitchInternal(uint32_t busID, float cents, const QObject* originator = nullptr);
    void propagateBusPitchLive(ProjectItem* busItem);
    bool setBusOutputBusVolumeInternal(uint32_t busID, float outputBusVolumeDb, const QObject* originator = nullptr);

    bool upsertSendInternal(uint32_t srcObjectID, uint32_t targetBusID, float sendLevel);
    bool removeSendInternal(uint32_t srcObjectID, uint32_t targetBusID);
    ProjectItem* findSendOwnerItem(uint32_t objectID) const;
    void forEachMixNodeItem(const std::function<void(ProjectItem*, VMixNode*)>& fn) const;

public:

    static QStringList getStandardDirectories();
    ProjectItem* findDirectoryItem(const QString& dirName) const;

    bool addProjectLanguage(const QString& languageName);
    bool removeProjectLanguage(const QString& languageName);
    QStringList getProjectLanguages() const;
    void setProjectLanguagesInternal(const QStringList& value);
    void setDefaultLanguage(const QString& languageName);
    QString getDefaultLanguage() const;

    QString packBuildOutputDir() const;
    bool setPackBuildOutputDir(const QString& value);
    void setPackBuildOutputDirInternal(const QString& value);

    ConversionFailurePolicy getConversionFailurePolicy() const;
    void setConversionFailurePolicy(ConversionFailurePolicy value);

    QString getPackExportCurrentPlatform() const;
    void setPackExportCurrentPlatformInternal(const QString& value);

    QStringList getPackExportSelectedPlatforms() const;
    void setPackExportSelectedPlatformsInternal(const QStringList& value);

    std::vector<PackGroup> getPackGroups() const;
    void setPackGroupsInternal(std::vector<PackGroup> value);

    std::vector<ActionPackTargeting> getActionPackTargeting() const;
    void setActionPackTargetingInternal(std::vector<ActionPackTargeting> value);

    QStringList getTargetPlatforms() const;
    void setProjectPlatformsInternal(const QVector<ProjectPlatform>& value);
    void notifyProjectPlatformEdits(const QVector<ProjectItem*>& changedItems, const QStringList& platformIds);
    void notifyProjectPlatformEdit(const QVector<ProjectItem*>& changedItems, const QString& platformId);
    void refreshSoundSourceProjections(ProjectPlatformId primary, const QString& language);

signals:
    void folderCreated(PhysicalFolder* folder);
    void folderRenamed(PhysicalFolder* folder, const QString& oldName);
    void folderDeleted(PhysicalFolder* folder);
    void folderMoved(PhysicalFolder* folder);
    void activePropertyEditsFinishing(bool commitChanges);
    void projectPlatformEdited(const QString& platformId);
    void projectOpened(ProjectData* project);
    void projectClosed();
    void projectSaved();
    void warningOccurred(const QString& warning);
    void itemCreated(ProjectItem* item);
    void itemRenamed(ProjectItem* item, const QString& oldName);
    void itemDeleted(ProjectItem* item);
    void itemMoved(ProjectItem* item, ProjectItem* oldParent, ProjectItem* newParent);
    void itemPropertyChanged(ProjectItem* item, const QString& propertyName,
                             const QObject* originator = nullptr);
    void itemPropertyPreviewed(ProjectItem* item, const QString& propertyName,
                              const QObject* originator = nullptr);
    void fieldGroupsPreviewed(ProjectItem* item, const DefinitionFieldGroups& groups);
    void bindingCurvePreviewed(ProjectItem* item, harkenstudio::ObjectId bindingId,
                              const QUuid& previewId,
                              const std::optional<ParameterBindingDef>& binding);
    void attenuationCurvePreviewed(const QUuid& guid,
                                  const QUuid& previewId,
                                  const std::optional<AttenuationSnapshot>& attenuation);
    void busChanged(uint32_t busID, const QString& propertyName,
                    const QObject* originator = nullptr);
    void packBuildOutputDirChanged(QString newValue);
    void conversionFailurePolicyChanged(ConversionFailurePolicy value);
    void packExportCurrentPlatformChanged(QString newValue);
    void packExportSelectedPlatformsChanged(QStringList newValue);
    void packGroupsChanged();
    void actionPackTargetingChanged();
    void targetPlatformsChanged(QStringList newValue);
    void projectLanguagesChanged(QStringList newValue);
    void errorOccurred(const QString& error);
    void externalFilesAdded(const QStringList& paths);
    void externalFilesRemoved(const QStringList& paths);
    void externalFilesModified(const QStringList& paths);
    void packExternallyModified(const QString& wuPath);

private:
    explicit ProjectManager(QObject* parent = nullptr);
    ~ProjectManager() override;
    void initializeRuntimeSync();

    bool createDirectoryStructure(const QString& projectRoot);
    bool createMainProjectFile(const ProjectConfig& config, const QString& projectRoot);
    ProjectItem* createItemImpl(ProjectItem* parent, const QString& name,
                                ProjectItem::Type type, const QString& path,
                                const QString& bucketFolderHint, QUndoCommand* undoParent,
                                const Clip* initialClip,
                                VariableKind variableKind = VariableKind::Smooth);
    void recordDeletedNameHistory(ProjectItem* item);
    void recordCreatedNameHistory(ProjectItem* item);

    std::shared_ptr<ProjectData> m_currentProject;
    std::unique_ptr<FolderSaveState> m_folderSaveState;
    std::unique_ptr<ProjectItemSaveState> m_itemSaveState;
    ProjectSerializer m_serializer;
    std::function<void(StartupPhase)> m_startupProgressCallback;

    ClipboardOp        m_clipboardOp = ClipboardOp::None;
    std::vector<QUuid> m_clipboardGuids;
    struct ClipboardCopy {
        std::shared_ptr<const ProjectItem> item;
        std::optional<DefinitionFieldGroups> rootGroups;
    };
    std::vector<ClipboardCopy> m_clipboardCopies;
    std::unique_ptr<ProjectSerializer> m_clipboardSerializer;

    bool isMonoRootBucketRoot(const ProjectItem* item) const;
    std::unique_ptr<VNX::ProjectIndex> m_projectIndex;
    std::unique_ptr<VNX::IntegrityScanner> m_integrityScanner;
    FileWatcher m_fileWatcher;
    AuditionOverlay m_auditionOverlay;
    VariableRehearsal m_variableRehearsal;
    TransitionRehearsal m_transitionRehearsal;
    bool m_closing = false;
    bool m_runtimePrepared = false;

    struct RuntimeSoundPreparationKey {
        QString path;
        QString contentHash;
        qint64 fileSize = 0;
        qint64 modifiedMs = 0;
        SoundSourceDef source;
        double scheduleOffsetSec = 0;

        bool operator==(const RuntimeSoundPreparationKey&) const = default;
    };
    struct RuntimeSoundPreparationEntry {
        RuntimeSoundPreparationKey key;
        uint64_t revision = 0;
    };
    QHash<harkenstudio::ObjectId, RuntimeSoundPreparationEntry> m_runtimeSoundPreparation;

    std::shared_ptr<ProjectData> m_pendingDestruction;
    void drainPendingDestruction();
    void finishRuntimeCloseIfReady();
    void clearRuntimeCloseState();
    void commitQueuedPreparedProject();
    void cancelQueuedPreparedProject(const QString& projectPath);
    std::optional<std::uint64_t> m_runtimeCloseRevision;
    std::optional<PreparedProject> m_queuedPreparedProject;

    struct PendingRuntimePreparation {
        std::uint64_t projectSessionId = 0;
        std::uint64_t runtimeEpoch = 0;
        std::uint64_t revision = 0;
        QPointer<QObject> context;
        std::function<void()> finished;
    };
    std::optional<PendingRuntimePreparation> m_pendingRuntimePreparation;
    void finishRuntimePreparationIfReady();

    QSet<ProjectItem*> m_pendingResyncs;
    bool m_resyncTimerScheduled = false;

    Q_DISABLE_COPY(ProjectManager)
};

}
