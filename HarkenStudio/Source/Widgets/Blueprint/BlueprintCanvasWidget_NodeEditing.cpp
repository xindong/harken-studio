#include "BlueprintCanvasWidget.h"
#include "SignalPathGainEditing.h"

#include "Core/PlatformContextService.h"
#include "Core/Components/VSound.h"
#include "Core/PropertyEditSession.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "NodeContentEditor.h"
#include "Core/ThemeManager.h"
#include "Core/FontManager.h"

#include <QApplication>
#include <QCursor>
#include <QGraphicsScene>
#include <QScrollBar>
#include <QTimer>

namespace VNX {

void BlueprintCanvasWidget::initializeNodeEditors_()
{
    contentRenderer_ = std::make_unique<NodeContentRenderer>();
    connect(qApp, &QApplication::focusChanged, this, [this] { scheduleContentRetirement_(); });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { scheduleContentRetirement_(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { scheduleContentRetirement_(); });
    connect(this, &BlueprintCanvasView::zoomChanged, this, [this] { scheduleContentRetirement_(); });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
        this, &BlueprintCanvasWidget::scheduleContentPresentationRefresh_);
    connect(&FontManager::instance(), &FontManager::fontChanged,
        this, &BlueprintCanvasWidget::scheduleContentPresentationRefresh_);
}

void BlueprintCanvasWidget::scheduleContentPresentationRefresh_()
{
    if (!contentRenderer_ || contentPresentationRefreshScheduled_) return;
    contentPresentationRefreshScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        contentPresentationRefreshScheduled_ = false;
        refreshContentPresentations_();
    });
}

void BlueprintCanvasWidget::refreshContentPresentations_()
{
    contentRenderer_->clear();
    NodeContentFields shownFields;
    for (const auto& [key, node] : nodeMap_)
        shownFields |= node->contentInfo().fields() & nodeContentFields_;
    nodeContentLabelWidth_ = NodeContentEditor::labelWidth(shownFields);
    for (const auto& [key, node] : nodeMap_) {
        if (auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget()))
            editor->setLabelWidth(nodeContentLabelWidth_);
        node->refreshContentPresentation();
    }
    for (auto* node : gainNodes_) {
        if (!node) continue;
        if (auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget()))
            editor->setLabelWidth(NodeContentEditor::labelWidth(node->contentFields()));
        node->refreshContentPresentation();
    }
}

void BlueprintCanvasWidget::activateNodeEditorAt_(const QPoint& position)
{
    if (interactionMode() == InteractionMode::Pan || dragSourcePin_ || nodeDragOrigin_) return;
    auto* item = itemAt(position);
    while (item) {
        if (auto* node = dynamic_cast<NodeItem*>(item)) {
            node->ensureContentWidget();
            break;
        }
        item = item->parentItem();
    }
}

void BlueprintCanvasWidget::scheduleContentRetirement_()
{
    if (contentRetirementScheduled_ || liveNodeEditors_.isEmpty()) return;
    contentRetirementScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        contentRetirementScheduled_ = false;
        auto* hovered = viewport()->underMouse() ? itemAt(viewport()->mapFromGlobal(QCursor::pos())) : nullptr;
        const auto editors = liveNodeEditors_;
        for (auto it = editors.cbegin(); it != editors.cend(); ++it) {
            if (!it.value()) continue;
            auto* node = it.key();
            if (node != hovered && (!hovered || !node->isAncestorOf(hovered)))
                node->releaseContentWidget();
        }
    });
}

void BlueprintCanvasWidget::trackNodeEditor_(NodeItem* node, NodeContentEditor* editor)
{
    liveNodeEditors_.insert(node, editor);
    connect(editor, &QObject::destroyed, this, [this, node] { liveNodeEditors_.remove(node); });
}

void BlueprintCanvasWidget::installNodeEditor_(NodeItem* node, const BlueprintNodeDesc& desc)
{
    const auto definition = pathSource_->graph().nodes[*desc.occurrenceIndex].key.objectGuid;
    node->setContentFactory([this, node, definition]() {
        auto* editor = new NodeContentEditor(node->contentInfo(), node->contentFields(), nodeContentLabelWidth_);
        editor->setContext(definition.toString(QUuid::WithoutBraces));
        tagContentEditor_(editor, ProjectManager::instance().findItemByGuid(definition));
        const auto begin = [this, editor, node, definition](NodeContentField field) {
            for (const auto& [key, candidate] : nodeMap_)
                if (auto* content = qobject_cast<NodeContentEditor*>(candidate->contentWidget()))
                    content->finishEditing(true, content == editor ? std::make_optional(field) : std::nullopt);
            for (auto* candidate : gainNodes_)
                if (candidate)
                    if (auto* content = qobject_cast<NodeContentEditor*>(candidate->contentWidget()))
                        content->finishEditing(true);
            if (nodeEditSession_) nodeEditSession_->cancel();
            nodeEditSession_.reset();
            gainEditSession_.reset();
            auto* item = ProjectManager::instance().findItemByGuid(definition);
            if (!item) return;
            const auto& info = node->contentInfo();
            QVariant value;
            switch (field) {
            case NodeContentField::Volume: value = info.volume.value_or(0.0f); break;
            case NodeContentField::Pitch: value = info.pitch.value_or(0.0f); break;
            case NodeContentField::OffsetOfStart: value = info.offsetOfStart.value_or(0.0); break;
            case NodeContentField::Loop: value = info.loopCount.value_or(1); break;
            default: return;
            }
            const auto& context = PlatformContextService::instance().context();
            const auto platforms = context.all || context.selected.isEmpty()
                ? std::nullopt : std::make_optional(context.selected);
            nodeEditSession_ = std::make_unique<PropertyEditSession>(item,
                nodeContentProperty(field).field, value, this, platforms);
        };
        const auto preview = [this](NodeContentField, const QVariant& value) {
            if (nodeEditSession_ && nodeEditSession_->isActive())
                nodeEditSession_->preview(value);
        };
        const auto finish = [this](NodeContentField field, bool committed) {
            if (auto session = std::move(nodeEditSession_)) {
                if (committed) session->commit();
                else session->cancel();
            }
            queuePathRefresh_(field == NodeContentField::Volume);
        };
        connect(editor, &NodeContentEditor::editBegan, this, begin);
        connect(editor, &NodeContentEditor::editPreviewed, this, preview);
        connect(editor, &NodeContentEditor::editEnded, this, finish);
        connect(editor, &NodeContentEditor::editingClosed, this, [this]() {
            if (!pathRefreshPending_) return;
            queuePathRefresh_(pathContentOnlyRefresh_);
        });
        connect(editor, &NodeContentEditor::valueCommitted, this,
            [begin, preview, finish](NodeContentField field, const QVariant& value) {
                begin(field);
                preview(field, value);
                finish(field, true);
            });
        trackNodeEditor_(node, editor);
        return editor;
    }, [this, node]() {
        return contentRenderer_->presentation(node->contentInfo(), node->contentFields(),
            nodeContentLabelWidth_, false);
    });
}

void BlueprintCanvasWidget::installGainEditor_(NodeItem* node)
{
    node->setContentFactory([this, node]() {
        const auto found = std::find(gainNodes_.begin(), gainNodes_.end(), node);
        const auto edgeIndex = static_cast<size_t>(found - gainNodes_.begin());
        const auto fields = node->contentInfo().fields();
        auto* editor = new NodeContentEditor(node->contentInfo(), fields, NodeContentEditor::labelWidth(fields));
        tagContentEditor_(editor, routeGainTopicItem_(edgeIndex));
        const auto begin = [this, editor](NodeContentField field) {
            for (const auto& [key, candidate] : nodeMap_)
                if (auto* content = qobject_cast<NodeContentEditor*>(candidate->contentWidget()))
                    content->finishEditing(true);
            for (auto* candidate : gainNodes_)
                if (candidate)
                    if (auto* content = qobject_cast<NodeContentEditor*>(candidate->contentWidget()))
                        content->finishEditing(true, content == editor ? std::make_optional(field) : std::nullopt);
            nodeEditSession_.reset();
            gainEditSession_.reset();
        };
        const auto start = [this, begin, node](NodeContentField field) {
            begin(field);
            const auto found = std::find(gainNodes_.begin(), gainNodes_.end(), node);
            if (found == gainNodes_.end()) return;
            const auto edgeIndex = static_cast<size_t>(found - gainNodes_.begin());
            gainEditSession_ = std::make_unique<SignalPathGainEditSession>(pathSource_->graph(), edgeIndex, this);
        };
        const auto preview = [this](NodeContentField, const QVariant& value) {
            if (gainEditSession_ && gainEditSession_->isActive()) gainEditSession_->preview(value.toDouble());
        };
        const auto finish = [this](NodeContentField, bool committed) {
            if (auto session = std::move(gainEditSession_)) {
                if (committed) session->commit();
                else session->cancel();
            }
            pathRefreshPending_ = false;
            schedulePathRefresh_();
        };
        connect(editor, &NodeContentEditor::editBegan, this, start);
        connect(editor, &NodeContentEditor::editPreviewed, this, preview);
        connect(editor, &NodeContentEditor::editEnded, this, finish);
        connect(editor, &NodeContentEditor::valueCommitted, this,
            [start, preview, finish](NodeContentField field, const QVariant& value) {
                start(field);
                preview(field, value);
                finish(field, true);
            });
        connect(editor, &NodeContentEditor::editingClosed, this, [this]() {
            if (!pathRefreshPending_) return;
            queuePathRefresh_(pathContentOnlyRefresh_);
        });
        trackNodeEditor_(node, editor);
        return editor;
    }, [this, node]() {
        return contentRenderer_->presentation(node->contentInfo(), node->contentFields(),
            NodeContentEditor::labelWidth(node->contentFields()), true);
    });
}

void BlueprintCanvasWidget::refreshNodeContent_(ProjectItem* item, const QString& propertyName)
{
    if (!item || activeSource_ != pathSource_.get()) return;
    if (propertyName == QLatin1String("outputBusVolume") || propertyName == QLatin1String("sends")) {
        refreshGainContent_(item);
        return;
    }
    const auto& graph = pathSource_->graph();
    const PlatformScope scope = graph.platform
        ? PlatformScope{fromFieldGroupGuid(*graph.platform)} : std::nullopt;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(propertyName);
    QVariant value;
    if (locator && property) {
        const auto state = PlayablePropertyAuthoringV1::readProperty(*item, *locator, property->id, scope);
        if (state) value = state->value.toVariant();
    } else if (propertyName == QLatin1String("scheduleOffsetSec")) {
        if (const auto* sound = item->comp<VSound>()) value = sound->scheduleOffsetSec;
    }
    if (!value.isValid()) return;
    for (const auto& [key, node] : nodeMap_) {
        if (key.id != item->objectID) continue;
        auto info = node->contentInfo();
        if (propertyName == QLatin1String("volume")) info.volume = value.toFloat();
        else if (propertyName == QLatin1String("pitch")) info.pitch = value.toFloat();
        else if (propertyName == QLatin1String("scheduleOffsetSec")) info.offsetOfStart = value.toDouble();
        else if (propertyName == QLatin1String("loopCount")) info.loopCount = value.toUInt();
        else return;
        if (key.occurrenceIndex) pathSource_->setNodeContent(*key.occurrenceIndex, info);
        node->setContentInfo(info);
    }
}

void BlueprintCanvasWidget::refreshOutputVolumePreview_(ProjectItem* item, const DefinitionFieldGroups& groups)
{
    refreshGainContent_(item, &groups);
}

void BlueprintCanvasWidget::refreshGainContent_(ProjectItem* previewItem,
                                               const DefinitionFieldGroups* previewGroups)
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!project || activeSource_ != pathSource_.get()) return;
    const auto& graph = pathSource_->graph();
    std::optional<FieldGroupEffectiveSnapshot> snapshot;
    for (size_t index = 0; index < gainNodes_.size(); ++index) {
        if (!gainNodes_[index]) continue;
        if (index >= graph.edges.size()) break;
        const auto& edge = graph.edges[index];
        if (previewItem && edge.routingSource && edge.routingSource->objectGuid != previewItem->guid) continue;
        if (!snapshot)
            snapshot = buildFieldGroupEffectiveSnapshot(*project, graph.platform,
                [&](const ProjectItem& item) {
                    return previewItem && previewGroups && item.guid == previewItem->guid
                        ? previewGroups : &item.fieldGroups;
                });
        const auto value = signalPathGainValue(*snapshot, graph, index, previewItem && previewGroups);
        if (!value) continue;
        auto info = gainNodes_[index]->contentInfo();
        if (edge.kind == SignalPathEdgeKind::Send) info.sendVolume = *value;
        else info.outputVolume = *value;
        gainNodes_[index]->setContentInfo(info);
    }
}

bool BlueprintCanvasWidget::nodeFieldEditing_() const
{
    for (const auto& [key, node] : nodeMap_) {
        if (node->isTitleEditing()) return true;
        if (const auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget());
            editor && editor->isEditing()) return true;
    }
    for (const auto* node : gainNodes_)
        if (node)
            if (const auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget());
                editor && editor->isEditing()) return true;
    return false;
}

void BlueprintCanvasWidget::finishNodeEdits_(bool committed)
{
    for (const auto& [key, node] : nodeMap_) {
        node->finishTitleEditing(committed);
        if (auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget()))
            editor->finishEditing(committed);
    }
    for (auto* node : gainNodes_)
        if (node)
            if (auto* editor = qobject_cast<NodeContentEditor*>(node->contentWidget()))
                editor->finishEditing(committed);
    if (auto session = std::move(nodeEditSession_)) {
        if (committed) session->commit();
        else session->cancel();
    }
    if (auto session = std::move(gainEditSession_)) {
        if (committed) session->commit();
        else session->cancel();
    }
    if (pathRefreshPending_) {
        queuePathRefresh_(pathContentOnlyRefresh_);
    }
    if (chainRefreshPending_) scheduleChainRefresh_();
}

}
