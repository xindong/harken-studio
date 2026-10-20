
#include "SelectionRouter.h"

#include "ItemTypeCatalog.h"
#include "AttenuationStore.h"
#include "BindingEditing.h"
#include "PlacementContext.h"
#include "LanguageContextService.h"
#include "PlatformContextService.h"
#include "SoundSourceEditing.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "ProjectObjectIndex.h"
#include "Components/VAttenuation.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"

#include "../Widgets/ProjectExplorer/ProjectExplorer.h"
#include "../Widgets/Hierarchy/Hierarchy.h"
#include "../Widgets/Inspector/InspectorPanel.h"
#include "../Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "../Widgets/GridEditor/GridEditorPanel.h"
#include "../Widgets/Timeline/TimelineEditorPanel.h"
#include "../Widgets/TransitionEditor/TransitionEditorPanel.h"

#include <DockWidget.h>

#include <QStatusBar>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QDesktopServices>
#ifdef Q_OS_WIN
#include <QProcess>
#endif

#include <optional>

namespace VNX {

SelectionRouter::SelectionRouter(MainWindow* host)
    : QObject(host)
    , host_(host)
{
}

void SelectionRouter::setViews(const Views& v)
{
    blueprintCanvas_        = v.blueprintCanvas;
    projectExplorer_        = v.projectExplorer;

    blueprintDock_    = v.blueprintDock;
}


void SelectionRouter::onProjectItemSelected(ProjectItem* item)
{
    auto* source = qobject_cast<ProjectTreePanel*>(sender());
    auto items = source ? source->selectedItems() : QList<ProjectItem*>{};
    if (item && !items.contains(item)) items = {item};
    if (!item && (!source || source->selectedFolders().isEmpty())) items.clear();
    routeSelection(item, false, nullptr, items, source);
}

void SelectionRouter::onBlueprintNodeSelected(uint32_t anchorId)
{
    auto& manager = ProjectManager::instance();
    auto* item = manager.findItemByObjectID(anchorId);
    if (!item) item = manager.findBusItem(anchorId);
    const auto placement = blueprintCanvas_->selectedPlacementContext();
    routeSelection(item, false, placement ? &*placement : nullptr,
                    blueprintCanvas_->selectedProjectItems(), blueprintCanvas_);
}

void SelectionRouter::onBlueprintNodeDoubleClicked(uint32_t anchorId)
{
    auto& manager = ProjectManager::instance();
    auto* item = manager.findItemByObjectID(anchorId);
    if (!item) item = manager.findBusItem(anchorId);
    const auto placement = blueprintCanvas_->selectedPlacementContext();
    activateItem(item, placement ? &*placement : nullptr);
}

void SelectionRouter::onProjectItemsSelectionChanged(const QList<ProjectItem*>& items)
{
    syncSelectionViews(items, nullptr, sender());
}

void SelectionRouter::onBlueprintSelectionCleared()
{
    routeSelection(nullptr, false, nullptr, {}, blueprintCanvas_);
}

void SelectionRouter::routeSelection(ProjectItem* item, bool reveal)
{
    routeSelection(item, reveal, nullptr);
}

bool SelectionRouter::revealProjectObject(harkenstudio::ObjectId id)
{
    auto& manager = ProjectManager::instance();
    const auto index = ProjectObjectIndex::fromProject(manager.currentProject());
    const auto* location = index.find(id);
    if (!location || !host_) return false;
    auto* owner = manager.findItemByGuid(location->ownerGuid);
    if (!owner) return false;
    routeSelection(owner, true);
    if (projectExplorer_) projectExplorer_->pingProjectItem(owner);
    switch (location->kind) {
    case ProjectObjectLocation::Kind::Item:
        activateItem(owner);
        return true;
    case ProjectObjectLocation::Kind::Effect:
        host_->openOrFocusInspectorForItem(owner);
        return true;
    case ProjectObjectLocation::Kind::Binding: {
        const auto* mix = owner->comp<VMixNode>();
        const auto* binding = mix ? mix->bindings.find(id) : nullptr;
        if (!binding) return false;
        const auto selection = CurveSelection::binding(owner->guid, binding->address, id);
        selectCurve(selection);
        return host_->openCurveEditor(selection) != nullptr;
    }
    case ProjectObjectLocation::Kind::Clip:
    case ProjectObjectLocation::Kind::Marker: {
        auto* timeline = qobject_cast<TimelineEditorPanel*>(
            host_->openStandaloneEditor(QStringLiteral("TimelineEditor"), owner));
        if (!timeline) return false;
        return location->kind == ProjectObjectLocation::Kind::Clip
            ? timeline->revealClip(authoringUuid(id))
            : timeline->revealMarker(authoringUuid(id));
    }
    }
    return false;
}

void SelectionRouter::routeSelection(ProjectItem* item, bool reveal,
                                     const PlacementContext* placement)
{
    routeSelection(item, reveal, placement,
                    item ? QList<ProjectItem*>{item} : QList<ProjectItem*>{}, nullptr);
}

void SelectionRouter::routeSelection(ProjectItem* item, bool reveal,
                                     const PlacementContext* placement,
                                     const QList<ProjectItem*>& items, const QObject* source)
{
    syncSelectionViews(items, placement, source);
    if (!item) {
        emit selectionChanged(nullptr);
        reconcileCurveSelection(nullptr);
        if (!host_)
            return;
        if (auto* ip = host_->activeInspectorPanel())
            ip->showEmpty();
        return;
    }

    emit selectionChanged(item);
    emit statusMessage(tr("Selected: %1").arg(item->name));
    reconcileCurveSelection(item);

    if (host_) {
        auto* ip = host_->activeInspectorPanel();
        auto* dock = host_->activeInspectorDock();

        auto bringUpInspector = [&]() {
            if (!reveal || !dock) return;
            if (dock->isClosed()) host_->revealDock(dock);
            else dock->setAsCurrentTab();
        };
        bool surfaceInspector = true;

        if (!ItemTypeCatalog::isUserFacingType(item->type)) {
            surfaceInspector = false;
            if (ip) ip->showEmpty();
        } else if (item->type != ProjectItem::Type::Bus &&
            ItemTypeCatalog::hasMixingCapability(item->type)) {
            if (ip) {
                if (placement) ip->showItemInspector(item, *placement);
                else ip->showItemInspector(item);
            }

            for (TransitionEditorPanel* panel : host_->allTransitionEditorPanels()) {
                if (panel && !panel->isPinned() && panel->boundItem() == item)
                    panel->setPlacementContext(placement ? std::optional<PlacementContext>(*placement)
                                                           : std::nullopt);
            }

            bringUpInspector();
        } else {
            switch (item->type)
            {
                case ProjectItem::Type::Action:
                case ProjectItem::Type::Attenuation:
                case ProjectItem::Type::Variable:
                {
                    if (ip) ip->showItemInspector(item);
                    break;
                }

                case ProjectItem::Type::Bus:
                {
                    if (item->comp<VBus>()) {
                        host_->syncBusSelectionViews(item->objectID);
                    } else {
                        surfaceInspector = false;
                    }
                    break;
                }

                default:
                    surfaceInspector = false;
                    break;
            }

            if (surfaceInspector) bringUpInspector();
        }
    }

    republishCurveSelection();
}

void SelectionRouter::syncSelectionViews(const QList<ProjectItem*>& items,
                                         const PlacementContext* placement, const QObject* source)
{
    const auto* tree = qobject_cast<const ProjectTreePanel*>(source);
    const auto folders = tree ? tree->selectedFolders() : QList<PhysicalFolder*>{};
    if (blueprintCanvas_ && source != blueprintCanvas_)
        blueprintCanvas_->setSelectedProjectItems(items, placement);
    if (projectExplorer_ && source != projectExplorer_)
        projectExplorer_->setSelectedEntries(items, folders);
    if (host_) {
        for (auto* hierarchy : host_->allHierarchyPanels()) {
            if (hierarchy != source) hierarchy->setSelectedEntries(items, folders);
        }
    }
}

void SelectionRouter::selectCurve(const CurveSelection& selection)
{
    if (selection.hasBinding()) {
        ProjectItem* host = ProjectManager::instance().findItemByGuid(selection.hostGuid);
        const auto* mix = BindingEditing::ledgerHost(host, selection.address);
        const ParameterBindingDef* binding = mix
            ? (!harkenstudio::IsNull(selection.bindingStableId)
                   ? mix->bindings.find(selection.bindingStableId)
                   : mix->bindings.findFirst(selection.address))
            : nullptr;
        if (!binding)
            return;
        curveSelection_ = CurveSelection::binding(
            host->guid, binding->address, binding->runtimeId());
        emit curveSelectionChanged(curveSelection_);
        return;
    }
    if (!selection.hasAttenuation()) {
        if (curveSelection_.isEmpty())
            return;
        curveSelection_ = {};
        emit curveSelectionChanged({});
        return;
    }

    auto* project = ProjectManager::instance().currentProject();
    const auto* attenuation = project
        ? AttenuationStore::find(project, selection.attenuationGuid,
                                 selection.attenuationName)
        : nullptr;
    if (!attenuation || !attenuation->dimension(selection.dimension))
        return;
    curveSelection_ = CurveSelection::attenuation(
        attenuation->hostItem()->guid, attenuation->hostItem()->name, selection.dimension);
    emit curveSelectionChanged(curveSelection_);
}

void SelectionRouter::reconcileCurveSelection(ProjectItem* item)
{
    if (curveSelection_.hasBinding()) {
        if (item && item->guid == curveSelection_.hostGuid)
            return;
        curveSelection_ = {};
        emit curveSelectionChanged({});
        return;
    }
    if (!curveSelection_.hasAttenuation())
        return;
    const auto* comp = item ? item->comp<VAttenuation>() : nullptr;
    if (comp && curveSelection_.belongsTo(*item))
        return;
    curveSelection_ = {};
    emit curveSelectionChanged({});
}

void SelectionRouter::republishCurveSelection()
{
    if (!curveSelection_.isEmpty())
        emit curveSelectionChanged(curveSelection_);
}

void SelectionRouter::onProjectItemDoubleClicked(ProjectItem* item)
{
    if (!item) return;
    routeSelection(item, false);
    if (blueprintCanvas_ && blueprintDock_ && !blueprintDock_->isClosed()
        && blueprintCanvas_->focusItem(item) && host_)
        host_->revealDock(blueprintDock_);
    activateItem(item);
}

void SelectionRouter::activateItem(ProjectItem* item, const PlacementContext* placement)
{
    if (!item || !host_) return;
    if (TimelineEditorPanel::timelineOwner(item))
        host_->openStandaloneEditor(QStringLiteral("TimelineEditor"), item);
    if (GridEditorPanel::acceptsItem(item))
        host_->openStandaloneEditor(QStringLiteral("GridEditor"), item);
    host_->openOrFocusInspectorForItem(item, placement);
    republishCurveSelection();
}

void SelectionRouter::selectItem(ProjectItem* item, bool reveal)
{
    if (!item) return;
    if (reveal) onProjectItemDoubleClicked(item);
    else        onProjectItemSelected(item);
}

void SelectionRouter::selectItem(ProjectItem* item, bool reveal,
                                 const PlacementContext& placement)
{
    if (!item) return;
    routeSelection(item, reveal, &placement);
}

void SelectionRouter::onAudioFileSelected(const QString& path)
{
    QString resolvedPath;
    if (projectExplorer_) {
        resolvedPath = resolveItemAudioAbsolutePath(projectExplorer_->selectedItem());
    }
    if (resolvedPath.isEmpty()) {
        resolvedPath = resolveAudioPathHintToAbsolute(path);
    }

    if (resolvedPath.isEmpty() || !QFileInfo::exists(resolvedPath)) {
        emit statusMessage(tr("Audio preview unavailable: cannot resolve file path"));
        return;
    }

    emit statusMessage(tr("Audio: %1").arg(QFileInfo(resolvedPath).fileName()));
}

QString SelectionRouter::resolveAudioPathHintToAbsolute(const QString& pathHint) const
{
    if (pathHint.isEmpty()) {
        return QString();
    }

    QFileInfo fileInfo(pathHint);
    if (fileInfo.isAbsolute()) {
        return fileInfo.absoluteFilePath();
    }

    ProjectData* project = ProjectManager::instance().currentProject();
    if (!project || project->projectPath.isEmpty()) {
        return QString();
    }

    const QString projectRoot = QFileInfo(project->projectPath).absolutePath();
    const QString AudioSourcesDir = PathUtils::getAudioSourcesDir(projectRoot);
    return PathUtils::toAbsolutePath(pathHint, AudioSourcesDir);
}

QString SelectionRouter::resolveItemAudioAbsolutePath(const ProjectItem* item) const
{
    if (!item) {
        return QString();
    }

    const QString language = SoundSourceEditing::previewLanguage(
        *item, LanguageContextService::instance().language());
    const uint32_t sourceID = SoundSourceEditing::activeSourceId(
        *item, PlatformContextService::instance().context().primary, language);
    if (sourceID != 0) {
        const AssetInfo* info = AssetRegistry::instance().getAsset(sourceID);
        if (info && !info->absolutePath.isEmpty()) {
            return info->absolutePath;
        }
    }

    const QString relativePath = SoundSourceEditing::activeRelativePath(
        *item, PlatformContextService::instance().context().primary, language);
    if (!relativePath.isEmpty()) {
        return resolveAudioPathHintToAbsolute(relativePath);
    }

    return QString();
}

void SelectionRouter::revealItemSourceInExplorer(ProjectItem* item)
{
    const QString abs = resolveItemAudioAbsolutePath(item);
    if (abs.isEmpty()) {
        host_->statusBar()->showMessage(tr("No source file on disk for this item."), 3000);
        return;
    }
    const QFileInfo info(abs);
#ifdef Q_OS_WIN
    QProcess::startDetached(
        QStringLiteral("explorer.exe"),
        QStringList{ QStringLiteral("/select,%1")
                         .arg(QDir::toNativeSeparators(info.absoluteFilePath())) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
#endif
}

void SelectionRouter::onActionSelected(const QString& name)
{
    emit statusMessage(tr("Action: %1").arg(name));
}

}
