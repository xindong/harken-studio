
#pragma once

#include <QGraphicsView>
#include <QHash>
#include <QMargins>
#include <QPointF>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "NodeItem.h"
#include "BlueprintCanvasView.h"
#include "BlueprintEditTypes.h"
#include "Core/ContextInfo.h"
#include "Core/TransitionSemantics.h"
#include "IBlueprintSource.h"
#include "SignalChainSource.h"
#include "SignalPathSource.h"
#include "SignalPathScope.h"

class QGraphicsScene;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMimeData;
class QMouseEvent;
class QWidget;
class QPainter;
class QGraphicsPathItem;
class QLabel;
class QVariantAnimation;

namespace VNX {
class SignalPathGainEditSession;
class ToastPresenter;
struct SignalPathConnectPlan;

struct ProjectData;
struct ProjectItem;
class  NodeItem;
class  NodePin;
class  ConnectionPath;
class NodeContentEditor;
class NodeContentRenderer;
class PropertyEditSession;

class BlueprintCanvasWidget : public BlueprintCanvasView, public IContextInfoSource
{
    Q_OBJECT

public:
    explicit BlueprintCanvasWidget(QWidget* parent = nullptr);
    ~BlueprintCanvasWidget() override;

    ContextTopic contextTopicAt(const QPoint& pos) const override;

    void rebuildFromProject();

    void useSignalChain(ProjectItem* item);
    void useSignalChain(ProjectItem* item, const PlacementContext& placement);
    void useSignalPaths(const ProjectData& project, SignalPathGraph graph);
    bool focusItem(ProjectItem* item, const PlacementContext* placement = nullptr);
    bool pingProjectItem(ProjectItem* item);
    void setNavigationViewportMargins(const QMargins& margins);
    QList<ProjectItem*> selectedProjectItems() const;
    void setSelectedProjectItems(const QList<ProjectItem*>& items,
                                 const PlacementContext* placement = nullptr);
    std::optional<PlacementContext> selectedPlacementContext() const;
    IBlueprintSource::Scope currentScope() const;

    void useSignalChainForBus(uint32_t busID);

    void clear();

    SignalChainSource* activeSignalChainSource() const;
    SignalPathSource* activeSignalPathSource() const;
    NodeItem* signalPathNode(size_t occurrenceIndex) const;
    ConnectionPath* signalPathEdge(size_t edgeIndex) const;
    NodeItem* signalPathGainNode(size_t edgeIndex) const;
    ConnectionPath* signalPathGainOutputEdge(size_t edgeIndex) const;

    void setTransitionWatch(const TransitionSemantics::TransitionWatchView& view);
    const TransitionSemantics::TransitionWatchView& transitionWatch() const { return transitionWatch_; }

    using ConnectResult = BlueprintConnectResult;

    ConnectResult connectNodes(uint32_t fromId, uint32_t toId,
                               QString edgeType = QString(),
                               QString fromKind = QString(),
                               QString toKind = QString());
    ConnectResult connectSignalPathNodes(SignalPathEndpoint from, SignalPathEndpoint to,
                                         bool addSend = false);
    ConnectResult disconnectSignalPathEdges(const std::vector<size_t>& edgeIndices);
    bool disconnectNodes(uint32_t fromId, uint32_t toId,
                         QString edgeType = QString(),
                         QString fromKind = QString(),
                         QString toKind = QString());
    bool setNodeOverride(uint32_t itemId, bool override);

    void                setFlowDirection(CanvasFlowDirection dir);
    CanvasFlowDirection flowDirection() const;

    void setGridSnapEnabled(bool enabled);
    bool gridSnapEnabled() const { return gridSnapEnabled_; }

    void setNodeContentFields(NodeContentFields fields);
    NodeContentFields nodeContentFields() const { return nodeContentFields_; }

    void setSoundsFiltered(bool filtered);
    bool soundsFiltered() const { return soundsFiltered_; }

    static bool supportsHierarchyCollapse(ProjectItemType type);
    void setHierarchyVisible(uint32_t objectId, bool visible);
    Qt::CheckState hierarchyVisibility(uint32_t objectId) const;
    bool hasCollapsedHierarchy() const;
    const QSet<QUuid>& hiddenHierarchyRoots() const { return hiddenHierarchyRoots_; }

    void setBusVisible(uint32_t busObjectId, bool visible);
    Qt::CheckState busVisibility(uint32_t busObjectId) const;
    bool hasCollapsedBuses() const;
    const QSet<QUuid>& hiddenBuses() const { return hiddenBuses_; }

    void setChainVisible(uint32_t objectId, bool visible, bool recursive = true);
    Qt::CheckState chainVisibility(uint32_t objectId) const;
    bool canCollapseChain(uint32_t objectId) const;

    void resetNodeLayout();
    // Scales the view so all items fit the viewport.
    void zoomToFit();

signals:
    void auditionTargetRequested(uint32_t objectID);
    void nodeSelected(uint32_t anchorId);
    void nodeDoubleClicked(uint32_t anchorId);
    void selectionCleared();
    void signalChainViewChanged();

    void flowDirectionChanged(CanvasFlowDirection dir);
    void gridSnapEnabledChanged(bool enabled);
    void nodeContentFieldsChanged(NodeContentFields fields);
    void viewportFilterChanged();
    void hierarchyCollapseChanged();
    void busCollapseChanged();

protected:
    bool viewportEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

    void drawForeground(QPainter* painter, const QRectF& rect) override;

    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    void populateContextMenu(QMenu& menu, QContextMenuEvent* event) override;

private slots:
    void onProjectOpened(ProjectData* project);
    void onProjectClosed();
    void onItemCreated(ProjectItem* item);
    void onItemDeleted(ProjectItem* item);
    void onItemMoved(ProjectItem* item, ProjectItem* oldParent, ProjectItem* newParent);
    void onItemRenamed(ProjectItem* item, const QString& oldName);
    void onItemPropertyChanged(ProjectItem* item, const QString& propertyName);
    void onBusChanged(uint32_t busID, const QString& propertyName);

    void onSceneSelectionChanged();

private:
    QGraphicsScene* scene_ = nullptr;
    ToastPresenter* toast_ = nullptr;

    std::unique_ptr<SignalChainSource> chainSource_;
    std::unique_ptr<SignalPathSource> pathSource_;
    IBlueprintSource*                  activeSource_ = nullptr;

    enum class NodeKind : uint8_t { ProjectItem, Bus };
    struct NodeKey {
        NodeKind kind;
        uint32_t id;
        std::optional<size_t> occurrenceIndex{};
        bool operator==(NodeKey const& rhs) const {
            return kind == rhs.kind && id == rhs.id && occurrenceIndex == rhs.occurrenceIndex;
        }
    };
    struct NodeKeyHash {
        size_t operator()(NodeKey const& k) const noexcept;
    };
    struct PathEdgeKey {
        QUuid from;
        QUuid to;
        SignalPathEdgeKind kind;
        std::optional<QUuid> routingSource;
        bool operator==(const PathEdgeKey&) const = default;
    };

    std::unordered_map<NodeKey, NodeItem*, NodeKeyHash> nodeMap_;
    struct WaveformNode {
        NodeItem* item = nullptr;
        std::optional<WaveformFrameRange> range;
    };
    QHash<QString, std::vector<WaveformNode>> waveformNodes_;
    std::vector<ConnectionPath*> edges_;
    std::vector<NodeItem*> gainNodes_;
    std::vector<ConnectionPath*> gainOutputEdges_;
    struct PathSceneState {
        QString projectId;
        SignalPathGraph graph;
    };
    std::optional<PathSceneState> pathSceneState_;
    bool projectView_ = true;
    std::vector<SignalPathNodeKey> selectedPathKeys_;
    std::vector<PathEdgeKey> selectedPathEdgeKeys_;
    std::optional<QSet<QUuid>> selectedObjectGuids_;
    QString pathProjectId_;
    std::vector<QString> pathPositionKeys_;
    std::vector<QPointF> pathDragStartPositions_;
    std::vector<QString> gainPositionKeys_;
    std::vector<QPointF> gainDragStartPositions_;
    bool pathRefreshPending_ = false;
    bool pathContentOnlyRefresh_ = false;
    bool pathRefreshScheduled_ = false;
    bool chainRefreshPending_ = false;
    bool chainRefreshScheduled_ = false;
    std::unique_ptr<PropertyEditSession> nodeEditSession_;
    std::unique_ptr<SignalPathGainEditSession> gainEditSession_;
    bool gridSnapEnabled_ = true;
    NodeContentFields nodeContentFields_ = kAllNodeContentFields;
    bool soundsFiltered_ = false;
    QSet<QUuid> hiddenHierarchyRoots_;
    QSet<QUuid> hiddenBuses_;
    struct CollapseReveal {
        QSet<QUuid> nodes;
        QSet<QUuid> sources;
        bool operator==(const CollapseReveal&) const = default;
    };
    std::map<QUuid, CollapseReveal> collapseReveals_;
    std::unordered_map<uint32_t, Qt::CheckState> inputVisibility_;

    using PathPositions = std::vector<std::pair<QString, QVariant>>;
    void layoutPathNodes_();
    void applyPathPositions_(const PathPositions& positions);
    void commitPathPositions_(const PathPositions& positions, const QString& description);
    void scheduleChainRefresh_();
    void schedulePathRefresh_();
    void queuePathRefresh_(bool contentOnly);
    void refreshPathContents_();
    void refreshProjectPaths_();
    void applyTreeVisibility_();
    enum class RevealScope { Node, Inputs, Upstream };
    void revealChain_(const ProjectItem& item, RevealScope scope);
    void refreshChainPins_();
    bool applyConnectPlan_(SignalPathConnectPlan plan);
    void applyProjectItemSelection_();
    void refreshPlaybackActivity_();
    void refreshNodeTitles_();
    void refreshNodeWaveforms_(uint32_t sourceID = 0);
    void refreshPathFocus_();
    void updateNodeGridSnap_();
    bool nodeFieldEditing_() const;
    std::unique_ptr<NodeContentRenderer> contentRenderer_;
    QHash<NodeItem*, QPointer<NodeContentEditor>> liveNodeEditors_;
    int nodeContentLabelWidth_ = 0;
    bool contentRetirementScheduled_ = false;
    bool contentPresentationRefreshScheduled_ = false;
    void initializeNodeEditors_();
    void trackNodeEditor_(NodeItem* node, NodeContentEditor* editor);
    void activateNodeEditorAt_(const QPoint& position);
    void scheduleContentRetirement_();
    void scheduleContentPresentationRefresh_();
    void refreshContentPresentations_();
    void installNodeEditor_(NodeItem* node, const BlueprintNodeDesc& desc);
    void installGainEditor_(NodeItem* node);
    ContextTopic contentFieldTopic_(NodeContentField field, ProjectItem* item) const;
    void tagContentEditor_(NodeContentEditor* editor, ProjectItem* item);
    ProjectItem* routeGainTopicItem_(size_t edgeIndex) const;
    void refreshNodeContent_(ProjectItem* item, const QString& propertyName);
    void refreshOutputVolumePreview_(ProjectItem* item, const DefinitionFieldGroups& groups);
    void refreshGainContent_(ProjectItem* previewItem = nullptr,
                             const DefinitionFieldGroups* previewGroups = nullptr);
    void finishNodeEdits_(bool committed);

    void refreshTransitionWatch_();
    void materializeFromSource_();

    void populateNodeContextMenu_(QMenu& menu, NodeItem* node);
    PathEdgeKey signalPathEdgeKey_(size_t index) const;
    void setSignalPathEdgeSelected_(size_t index, bool selected);
    bool signalPathEdgeDeletable_(size_t index) const;
    std::vector<size_t> selectedSignalPathEdges_(bool deletableOnly) const;
    void populateCreationMenu_(QMenu& menu, const QPointF& position);
    void showCreatedItem_(ProjectItem* item, std::optional<QPointF> position);

    void paintTransitionWatch_(QPainter* painter);
    void paintLocationPing_(QPainter* painter);

    QVariantAnimation* locationPing_ = nullptr;
    uint32_t locationPingObjectID_ = 0;
    QMargins navigationViewportMargins_;

    TransitionSemantics::TransitionWatchView transitionWatch_;

    NodeItem* hoverDropTarget_ = nullptr;
    bool externalDragActive_ = false;
    QStringList externalAudioFiles_;

    SignalPathConnectPlan prepareObjectDrop_(const QMimeData* mime, NodeItem* target) const;
    bool canImportAudioAt_(NodeItem* target) const;
    void clearExternalDrop_();

    QGraphicsPathItem* tempPath_        = nullptr;
    NodePin*           dragSourcePin_   = nullptr;
    NodePin*           connectHoverPin_ = nullptr;
    NodeItem*          connectHoverNode_ = nullptr;
    bool dragAddsSend_ = false;
    QPoint connectStartPosition_;
    QPoint connectPosition_;
    QLabel* connectHint_ = nullptr;
    QString connectHintText_;
    NodeItem* nodeDragOrigin_ = nullptr;
    QPoint nodeDragStart_;
    uint32_t auditionClickObjectID_ = 0;
    QPoint auditionClickStart_;
    std::vector<SignalPathEndpoint> nodeDragSources_;

    bool evaluateConnect_(NodePin* source, NodePin* target, QString& reason, bool addSend = false) const;
    std::optional<SignalPathEndpoint> pathEndpoint_(NodePin* pin) const;
    NodeItem* connectTargetAt_(const QPoint& position, NodePin*& pin, bool nodeDrop = false) const;
    SignalPathConnectPlan prepareConnect_(NodePin* source, NodeItem* target, NodePin* targetPin, bool addSend) const;
    QString connectPreviewText_(const NodeItem* target, const SignalPathConnectPlan& plan) const;
    void beginConnectionDrag_(const QPoint& position);
    void updateConnectPreview_(const QPoint& position);
    void positionConnectHint_();
    void updateNodeDropPreview_(const QPoint& position);
    void restoreNodeDragPositions_();
    void cancelConnection_();
    QString connectErrorText(const QString& code) const;
    void clearConnectTarget_();

    CanvasFlowDirection flowDir_         = CanvasFlowDirection::LR;
    void persistFlowDirection_() const;
    void loadPersistedFlowDirection_();

    void loadPersistedLayerVisibility_();

    void clearScene();
    static QPointF defaultLayoutPos(int index);
};

}
