#include "ExternalEditorPanel.h"

#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/BindingContract.h"
#include "Core/AttenuationStore.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/SoundSourceSemantics.h"
#include "Widgets/CurveEditor/CurveEditorWidget.h"
#include "Widgets/Inspector/BindingFieldEditor.h"
#include "Widgets/Inspector/FieldGroupInspectorView.h"
#include "Widgets/SourceEditor/SourceRangeEditor.h"

#include <QChildEvent>
#include <QFocusEvent>
#include <QScrollArea>
#include <QVBoxLayout>

namespace VNX {

ExternalEditorPanel::ExternalEditorPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ExternalEditorPanel"));
    setFocusPolicy(Qt::StrongFocus);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(0);
    curves_ = new QWidget(this);
    auto* curvesLayout = new QVBoxLayout(curves_);
    curvesLayout->setContentsMargins(0, 0, 0, 0);
    curvesLayout->setSpacing(8);
    bindings_ = new BindingFieldEditor(curves_);
    curvesLayout->addWidget(bindings_);
    curve_ = new CurveEditorWidget(curves_);
    curve_->layout()->setContentsMargins(0, 0, 0, 0);
    curvesLayout->addWidget(curve_, 1);
    curvesScroll_ = new QScrollArea(this);
    curvesScroll_->setWidgetResizable(true);
    curvesScroll_->setFrameShape(QFrame::NoFrame);
    curvesScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    curvesScroll_->setWidget(curves_);
    layout->addWidget(curvesScroll_, 1);
    source_ = new SourceRangeEditor({}, nullptr, this,
        [this](const SoundSourceDef& source) { commitSource(source); });
    layout->addWidget(source_, 1);
    showContent(nullptr);
    connect(bindings_, &BindingFieldEditor::curveSelectionChanged, this,
            [this](const CurveSelection& selection) {
        if (selection_.kind != ExternalEditorSelection::Kind::Binding) return;
        selection_.curve = selection;
        curve_->bindSelection(selection);
    });
    connect(bindings_, &BindingFieldEditor::navigateRequested,
            this, &ExternalEditorPanel::navigateRequested);
    auto& project = ProjectManager::instance();
    connect(&project, &ProjectManager::projectClosed, this, &ExternalEditorPanel::clearEditor);
    connect(&project, &ProjectManager::itemMoved, this,
            &ExternalEditorPanel::refreshAttenuationReference, Qt::QueuedConnection);
    connect(&project, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        if (item && item->guid == selection_.ownerGuid)
            clearEditor();
        else if (item && item->guid == selection_.curve.attenuationGuid) {
            if (selection_.attenuationSlot && !pinned_)
                QMetaObject::invokeMethod(this, &ExternalEditorPanel::refreshAttenuationReference,
                                          Qt::QueuedConnection);
            else
                clearEditor();
        }
    });
    connect(&project, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
        if (item && item->guid == selection_.ownerGuid
            && (property == QLatin1String("sources") || property == QLatin1String("distinguishLanguage")))
            refreshSource();
        if (property == QLatin1String("fieldGroups") || property == QLatin1String("attenuationRef")
            || property == QLatin1String("dimensions") || property == QLatin1String("maxDistance"))
            refreshAttenuationReference();
    }, Qt::QueuedConnection);
    auto& assets = AssetRegistry::instance();
    connect(&assets, &AssetRegistry::assetModified, this, [this] { refreshSource(); });
    connect(&assets, &AssetRegistry::assetUnregistered, this, [this] { refreshSource(); });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, &ExternalEditorPanel::refreshAttenuationReference);
    installFocusFilterRecursively(this);
}

ProjectItem* ExternalEditorPanel::boundItem() const
{
    return ProjectManager::instance().findItemByGuid(selection_.ownerGuid);
}

bool ExternalEditorPanel::acceptsSelection(const ProjectItem* item) const
{
    return item && !selection_.ownerGuid.isNull() && item->guid == selection_.ownerGuid;
}

void ExternalEditorPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned) return;
    pinned_ = pinned;
    if (!pinned_) refreshAttenuationReference();
    emit pinToggled(pinned_);
}

void ExternalEditorPanel::bindItem(ProjectItem* item)
{
    if (acceptsSelection(item)) return;
    clearEditor();
    if (item) selection_.ownerGuid = item->guid;
}

void ExternalEditorPanel::bindSelection(const CurveSelection& selection)
{
    bindSelection(ExternalEditorSelection::fromCurve(selection));
}

void ExternalEditorPanel::bindSelection(const ExternalEditorSelection& selection)
{
    selection_ = selection;
    using Kind = ExternalEditorSelection::Kind;
    if (selection_.kind != Kind::Binding)
        bindings_->bind(nullptr, {});
    if (selection_.kind != Kind::Source)
        source_->setSource({}, nullptr);
    bindings_->setVisible(selection_.kind == Kind::Binding);
    switch (selection_.kind) {
    case Kind::Binding: {
        const auto selectedCurve = selection.curve;
        bindings_->bind(boundItem(), selection_.bindingAddress);
        if (selectedCurve.bindingStableId != harkenstudio::ObjectId{})
            bindings_->selectBinding(selectedCurve.hostGuid, selectedCurve.bindingStableId);
        showContent(curves_);
        break;
    }
    case Kind::Attenuation:
        curve_->bindSelection(selection_.curve);
        showContent(curves_);
        refreshAttenuationReference();
        break;
    case Kind::Source:
        curve_->clear();
        showContent(source_);
        refreshSource();
        break;
    case Kind::None:
        clearEditor();
        break;
    }
}

void ExternalEditorPanel::clearEditor()
{
    selection_ = {};
    bindings_->bind(nullptr, {});
    curve_->clear();
    source_->setSource({}, nullptr);
    showContent(nullptr);
}

void ExternalEditorPanel::showContent(QWidget* content)
{
    curves_->setVisible(content == curves_);
    curvesScroll_->setVisible(content == curves_);
    source_->setVisible(content == source_);
    updateGeometry();
}

void ExternalEditorPanel::showBindingAddMenu()
{
    if (selection_.kind == ExternalEditorSelection::Kind::Binding)
        bindings_->showAddMenu();
}

const SoundSourceDef* ExternalEditorPanel::currentSource() const
{
    const auto* item = boundItem();
    const auto* sound = item ? item->comp<VSound>() : nullptr;
    if (selection_.kind != ExternalEditorSelection::Kind::Source || !sound) return nullptr;
    return SoundSourceSemantics::findEntry(*sound, selection_.sourceEntryId);
}

void ExternalEditorPanel::refreshSource()
{
    if (selection_.kind != ExternalEditorSelection::Kind::Source) return;
    const auto* source = currentSource();
    source_->setSource(source ? *source : SoundSourceDef{},
        source ? AssetRegistry::instance().getAsset(source->sourceId) : nullptr);
    showContent(source ? source_ : nullptr);
}

void ExternalEditorPanel::refreshAttenuationReference()
{
    if (selection_.kind != ExternalEditorSelection::Kind::Attenuation
        || !selection_.attenuationSlot || pinned_) return;
    const auto* project = ProjectManager::instance().currentProject();
    if (!project) return;
    const auto view = buildFieldGroupInspectorView(*project, selection_.ownerGuid,
        FieldGroupId::SpatialRating, PlatformContextService::instance().context());
    const auto reference = view.field(QStringLiteral("spatialAttenuationRef"));
    const auto* attenuation = view.valid && reference.uniform
        ? AttenuationStore::findByGuid(project, reference.value.toUuid()) : nullptr;
    const auto dimension = selection_.curve.dimension;
    selection_.curve = CurveSelection::attenuation(
        attenuation ? attenuation->hostItem()->guid : QUuid{}, attenuation ? attenuation->hostItem()->name : QString{}, dimension);
    curve_->bindSelection(selection_.curve);
    showContent(attenuation && attenuation->dimension(dimension) ? curves_ : nullptr);
}

void ExternalEditorPanel::commitSource(const SoundSourceDef& replacement)
{
    auto* item = boundItem();
    auto* sound = item ? item->comp<VSound>() : nullptr;
    const auto* current = currentSource();
    if (!sound || !current || current->sourceId != replacement.sourceId
        || current->entryId != replacement.entryId) return;
    auto sources = sound->sources;
    auto* source = SoundSourceSemantics::findEntry(sources, selection_.sourceEntryId);
    if (!source) return;
    source->trimStartFrame = replacement.trimStartFrame;
    source->trimEndFrame = replacement.trimEndFrame;
    source->rangeContentHash = replacement.rangeContentHash;
    ActionManager::instance().execute(QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
         {QStringLiteral("property"), QStringLiteral("sources")},
         {QStringLiteral("value"), SoundSourceSemantics::toJson(sources)}},
        ActionSource::GUI, this);
    refreshSource();
}

QVariantMap ExternalEditorPanel::snapshotState() const
{
    QVariantMap state;
    state[QStringLiteral("pinned")] = pinned_;
    state[QStringLiteral("ownerGuid")] = selection_.ownerGuid.toString(QUuid::WithoutBraces);
    if (selection_.attenuationSlot)
        state[QStringLiteral("attenuationSlot")] = true;
    if (selection_.kind == ExternalEditorSelection::Kind::Source) {
        state[QStringLiteral("sourceEntryId")] = selection_.sourceEntryId.toString(QUuid::WithoutBraces);
    } else if (selection_.kind == ExternalEditorSelection::Kind::Binding) {
        state[QStringLiteral("bindingHostGuid")] = selection_.curve.hostGuid.toString(QUuid::WithoutBraces);
        state[QStringLiteral("bindingHostClass")] = BindingContractSemantics::hostClassName(selection_.bindingAddress.hostClass);
        state[QStringLiteral("bindingSlotId")] = selection_.bindingAddress.slotId;
        state[QStringLiteral("bindingStableId")] = objectIdString(selection_.curve.bindingStableId);
    } else if (selection_.kind == ExternalEditorSelection::Kind::Attenuation) {
        state[QStringLiteral("attenuationGuid")] = selection_.curve.attenuationGuid.toString(QUuid::WithoutBraces);
        state[QStringLiteral("attenuation")] = selection_.curve.attenuationName;
        state[QStringLiteral("dimension")] = QLatin1String(attenuationDimensionName(selection_.curve.dimension));
    }
    if (selection_.placement) {
        QVariantList occurrences;
        for (const auto& occurrence : selection_.placement->occurrences())
            occurrences.append(QVariantMap{
                {QStringLiteral("track"), occurrence.trackGuid},
                {QStringLiteral("clip"), occurrence.clipGuid},
                {QStringLiteral("target"), occurrence.targetGuid}});
        state[QStringLiteral("placement")] = occurrences;
    }
    return state;
}

void ExternalEditorPanel::restoreState(const QVariantMap& state)
{
    if (state.contains(QStringLiteral("pinned")))
        setPinned(state.value(QStringLiteral("pinned")).toBool());
    const QUuid owner = state.value(QStringLiteral("ownerGuid")).toUuid();
    ExternalEditorSelection selection;
    if (state.contains(QStringLiteral("sourceEntryId"))) {
        selection = ExternalEditorSelection::source(owner, state.value(QStringLiteral("sourceEntryId")).toUuid());
    } else if (state.contains(QStringLiteral("bindingHostGuid"))) {
        BindingAddress address;
        address.hostClass = BindingContractSemantics::parseHostClass(
            state.value(QStringLiteral("bindingHostClass")).toString()).value_or(BindingHostClass::Voice);
        address.slotId = static_cast<uint8_t>(state.value(QStringLiteral("bindingSlotId")).toUInt());
        selection = ExternalEditorSelection::fromCurve(CurveSelection::binding(
            state.value(QStringLiteral("bindingHostGuid")).toUuid(), address,
            runtimeObjectId(QUuid(state.value(QStringLiteral("bindingStableId")).toString()))), owner);
        selection.kind = ExternalEditorSelection::Kind::Binding;
    } else if (state.contains(QStringLiteral("attenuationGuid")) || state.contains(QStringLiteral("attenuation"))) {
        AttenuationDimension dimension = AttenuationDimension::Volume;
        attenuationDimensionFromName(state.value(QStringLiteral("dimension")).toString(), &dimension);
        selection = ExternalEditorSelection::fromCurve(CurveSelection::attenuation(
            state.value(QStringLiteral("attenuationGuid")).toUuid(),
            state.value(QStringLiteral("attenuation")).toString(), dimension), owner);
        selection.kind = ExternalEditorSelection::Kind::Attenuation;
    }
    if (state.contains(QStringLiteral("placement"))) {
        std::vector<PlacementOccurrence> occurrences;
        for (const auto& value : state.value(QStringLiteral("placement")).toList()) {
            const auto occurrence = value.toMap();
            occurrences.push_back({occurrence.value(QStringLiteral("track")).toUuid(),
                occurrence.value(QStringLiteral("clip")).toUuid(),
                occurrence.value(QStringLiteral("target")).toUuid()});
        }
        selection.placement = PlacementContext(std::move(occurrences));
    }
    selection.attenuationSlot = state.value(QStringLiteral("attenuationSlot"), selection.attenuationSlot).toBool();
    bindSelection(selection);
}

void ExternalEditorPanel::installFocusFilterRecursively(QObject* root)
{
    root->installEventFilter(this);
    for (auto* child : root->children()) installFocusFilterRecursively(child);
}

bool ExternalEditorPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusIn && watched != this)
        emit panelFocused(panelId_);
    else if (event->type() == QEvent::ChildAdded)
        installFocusFilterRecursively(static_cast<QChildEvent*>(event)->child());
    return QWidget::eventFilter(watched, event);
}

void ExternalEditorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

}
