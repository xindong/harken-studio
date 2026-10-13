#include "CurveEditorWidget.h"

#include "Core/ActionManager.h"
#include "Core/AttenuationStore.h"
#include "Core/BindingEditing.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VVariable.h"
#include "Core/ProjectManager.h"
#include "Core/ThemeManager.h"
#include "Core/VariableRehearsal.h"
#include "Widgets/Common/LayoutDebugTint.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace VNX {

namespace {

QJsonArray pointsToJson(const QVector<CurvePointDef>& points, const QString& xKey)
{
    QJsonArray arr;
    for (const auto& p : points) {
        arr.append(QJsonObject{
            {xKey, static_cast<double>(p.x)},
            {QStringLiteral("value"), static_cast<double>(p.value)}
        });
    }
    return arr;
}

QJsonArray handlesToJson(const QVector<CurveHandlePairDef>& handles)
{
    QJsonArray arr;
    for (const auto& h : handles)
        arr.append(QJsonArray{static_cast<double>(h.h1x), static_cast<double>(h.h1y),
                              static_cast<double>(h.h2x), static_cast<double>(h.h2y)});
    return arr;
}

CurveCanvasProfile attenuationDimensionCanvasProfile(AttenuationDimension dimension)
{
    CurveCanvasProfile profile;
    profile.xAxisLabel = QObject::tr("Distance");
    profile.xMaxEditable = true;
    if (dimension == AttenuationDimension::Pitch) {
        profile.axisScale = CurveAxisScale::LinearValue;
        profile.yMin = -1200.0f;
        profile.yMax = 1200.0f;
        profile.defaultEndValue = 0.0f;
        profile.colorRole = CurveColorRole::AttenuationPitch;
        profile.yAxisLabel = QObject::tr("Pitch (cents)");
        profile.valueUnit = QStringLiteral("cents");
        profile.valueDecimals = 0;
        profile.clipboardFamily = QStringLiteral("pitch");
    } else {
        profile.yMin = -96.0f;
        profile.yMax = 0.0f;
        profile.valueMaximum = 0.0f;
        profile.defaultEndValue = -96.0f;
        profile.colorRole = CurveColorRole::AttenuationVolume;
        profile.yAxisLabel = QObject::tr("Volume (dB)");
    }
    return profile;
}

CurveCanvasProfile bindingCanvasProfile(const BindingContract& contract,
                                        const VVariable* variable)
{
    CurveCanvasProfile profile;
    profile.axisScale = CurveAxisScale::LinearValue;
    profile.yMin = 0.0f;
    profile.yMax = 1.0f;
    profile.defaultEndValue = 1.0f;
    profile.colorRole = CurveColorRole::ParameterBinding;
    profile.xMaxEditable = false;
    profile.yRangeEditable = true;
    profile.yRangeMinimum = contract.legalOutputRange.min;
    profile.yRangeMaximum = contract.legalOutputRange.max;
    if (variable) {
        profile.setVariableAxis(variable->kind, variable->labels);
        profile.xAxisLabel = QObject::tr("Variable");
    }
    profile.valueUnit = BindingSemantics::targetUnit(contract);
    profile.valueDecimals = contract.outputUnit == BindingOutputUnit::Cent ? 0 : 1;
    profile.clipboardFamily = contract.outputUnit == BindingOutputUnit::Cent
        ? QStringLiteral("pitch")
        : QStringLiteral("volume");
    return profile;
}

const ParameterBindingDef* findBinding(const ProjectItem* host,
                                       const CurveSelection& selection)
{
    const auto* mix = BindingEditing::ledgerHost(host, selection.address);
    if (!mix) return nullptr;
    if (selection.bindingStableId != harkenstudio::ObjectId{})
        return mix->bindings.find(selection.bindingStableId);
    return mix->bindings.findFirst(selection.address);
}

}

CurveEditorWidget::CurveEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("CurveEditorWidget");
    setupUi();
    showMessage({});

    connect(&ProjectManager::instance(), &ProjectManager::projectOpened,
            this, [this](ProjectData*) { refreshFromProject(); }, Qt::QueuedConnection);
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed,
            this, &CurveEditorWidget::clear, Qt::QueuedConnection);
    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged,
            this, [this](ProjectItem* item, const QString& property) {
                if (selection_.hasAttenuation()
                    && (property == QLatin1String("dimensions")
                        || property == QLatin1String("maxDistance")
                        || property == QLatin1String("attenuationRef")))
                    refreshFromProject();
                else if (selection_.hasBinding()
                         && item
                         && (item->guid == selection_.hostGuid
                             || item->type == ProjectItem::Type::Variable))
                    refreshFromProject();
            }, Qt::QueuedConnection);
    connect(&ProjectManager::instance().variableRehearsal(),
            &VariableRehearsal::rehearsalChanged, this,
            [this](const QUuid&) {
                if (selection_.hasBinding())
                    refreshBindingPlayhead();
            });
}

CurveEditorWidget::~CurveEditorWidget()
{
    canvas_->cancelDrag();
    finishCurvePreview();
    disconnect(canvas_, nullptr, this, nullptr);
}

void CurveEditorWidget::setupUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(0);

    stack_ = new QStackedWidget();
    root->addWidget(stack_, 1);

    auto* messagePage = new QWidget();
    DebugTint::tintContainer(messagePage, DebugTint::Role::Section);
    auto* messageLayout = new QVBoxLayout(messagePage);
    messageLayout->setAlignment(Qt::AlignCenter);
    messageLabel_ = new QLabel();
    messageLabel_->setProperty("textRole", QStringLiteral("muted"));
    messageLabel_->setProperty("textScale", QStringLiteral("body"));
    messageLabel_->setAlignment(Qt::AlignCenter);
    messageLabel_->setWordWrap(true);
    messageLayout->addWidget(messageLabel_);
    stack_->addWidget(messagePage);

    content_ = new QWidget();
    DebugTint::tintContainer(content_, DebugTint::Role::Section);
    auto* contentLayout = new QVBoxLayout(content_);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    stack_->addWidget(content_);

    canvas_ = new CurveCanvas();
    canvas_->setObjectName(QStringLiteral("curveEditorCanvas"));
    contentLayout->addWidget(canvas_, 1);

    connect(canvas_, &CurveCanvas::curveEdited, this, &CurveEditorWidget::onCanvasCurveEdited);
    connect(canvas_, &CurveCanvas::curvePreviewed, this, &CurveEditorWidget::onCanvasCurvePreviewed);
    connect(canvas_, &CurveCanvas::curvePreviewFinished, this, &CurveEditorWidget::finishCurvePreview);
    connect(canvas_, &CurveCanvas::xMaxEdited, this, &CurveEditorWidget::onCanvasMaxDistanceEdited);
    connect(canvas_, &CurveCanvas::yRangeEdited,
            this, &CurveEditorWidget::onOutputRangeCommitted);
}

void CurveEditorWidget::bindSelection(const CurveSelection& selection)
{
    canvas_->cancelDrag();
    finishCurvePreview();
    canvas_->dismissValueEditor();
    selection_ = selection;
    dimension_ = selection.dimension;
    refreshFromProject();
}

void CurveEditorWidget::clear()
{
    canvas_->cancelDrag();
    finishCurvePreview();
    selection_ = {};
    attenuation_ = {};
    canvas_->setOverlays({});
    canvas_->setPlayhead(std::nullopt);
    canvas_->dismissValueEditor();
    showMessage({});
}

void CurveEditorWidget::refreshFromProject()
{
    canvas_->cancelDrag();
    finishCurvePreview();
    if (selection_.hasBinding()) {
        attenuation_ = {};
        reloadBindingView(true);
        return;
    }
    if (!selection_.hasAttenuation()) {
        attenuation_ = {};
        showMessage({});
        return;
    }

    auto* project = ProjectManager::instance().currentProject();
    const auto* def = project
        ? AttenuationStore::find(project, selection_.attenuationGuid,
                                 selection_.attenuationName)
        : nullptr;
    if (!def) {
        clear();
        return;
    }

    const bool reloadingSame =
        (!attenuation_.guid.isNull() && attenuation_.guid == def->hostItem()->guid)
        || (!attenuation_.name.isEmpty()
            && attenuation_.name.compare(def->hostItem()->name, Qt::CaseInsensitive) == 0);

    attenuation_ = def->snapshot();
    selection_.attenuationGuid = def->hostItem()->guid;
    selection_.attenuationName = def->hostItem()->name;
    reloadAttenuationView(reloadingSame);
}

void CurveEditorWidget::reloadView(bool preserveCanvasSelection)
{
    if (selection_.hasBinding()) {
        reloadBindingView(preserveCanvasSelection);
        return;
    }
    reloadAttenuationView(preserveCanvasSelection);
}

void CurveEditorWidget::applyCanvasSelection(bool preserveCanvasSelection,
                                             const QVector<int>& previousSelection,
                                             int previousPrimarySelection)
{
    if (preserveCanvasSelection && !previousSelection.isEmpty())
        canvas_->setSelectedPointIndices(previousSelection, previousPrimarySelection);
    else
        canvas_->setSelectedPointIndex(-1);
}

void CurveEditorWidget::reloadAttenuationView(bool preserveCanvasSelection)
{
    const auto* dim = currentDimensionDef();
    if (!dim) {
        showMessage({});
        return;
    }

    updating_ = true;
    const QVector<int> previousSelection = preserveCanvasSelection
        ? canvas_->selectedPointIndices()
        : QVector<int>{};
    const int previousPrimarySelection = preserveCanvasSelection
        ? canvas_->selectedPointIndex()
        : -1;

    canvas_->setOverlays({});
    canvas_->setPlayhead(std::nullopt);
    const CurveCanvasProfile profile = attenuationDimensionCanvasProfile(dimension_);
    canvas_->setProfile(profile);
    canvas_->setXDomain(0.0f, std::max(1.0f, attenuation_.maxDistance));
    canvas_->setCurve(dim->curve.isEmpty()
                          ? attenuationDefaultCurve(dimension_, canvas_->xMax())
                          : dim->curve,
                      dim->handles);
    applyCanvasSelection(preserveCanvasSelection, previousSelection, previousPrimarySelection);
    stack_->setCurrentIndex(1);
    updating_ = false;
}

void CurveEditorWidget::reloadBindingView(bool preserveCanvasSelection)
{
    ProjectItem* host = ProjectManager::instance().findItemByGuid(selection_.hostGuid);
    const ParameterBindingDef* binding = findBinding(host, selection_);
    if (!host || !binding) {
        showMessage({});
        return;
    }
    selection_.bindingStableId = binding->runtimeId();
    selection_.address = binding->address;

    ProjectItem* variableItem = ProjectManager::instance().findItemByGuid(binding->variable);
    const auto* variable = variableItem ? variableItem->comp<VVariable>() : nullptr;
    const BindingContract contract = BindingContractSemantics::derive(
        binding->address.slotId, host->type);

    updating_ = true;
    const QVector<int> previousSelection = preserveCanvasSelection
        ? canvas_->selectedPointIndices()
        : QVector<int>{};
    const int previousPrimarySelection = preserveCanvasSelection
        ? canvas_->selectedPointIndex()
        : -1;

    CurveCanvasProfile profile = bindingCanvasProfile(contract, variable);
    profile.yAxisLabel = BindingSemantics::slotName(binding->address.slotId);
    if (!profile.yAxisLabel.isEmpty())
        profile.yAxisLabel[0] = profile.yAxisLabel[0].toUpper();
    if (!profile.valueUnit.isEmpty())
        profile.yAxisLabel += QStringLiteral(" (%1)").arg(profile.valueUnit);
    profile.yMin = std::min(binding->outputMin, binding->outputMax);
    profile.yMax = std::max(binding->outputMin, binding->outputMax);
    if (contract.outputUnit == BindingOutputUnit::Decibel) {
        profile.axisScale = CurveAxisScale::MirroredAmplitudeDb;
        profile.yMin = contract.legalOutputRange.min;
        profile.yMax = contract.legalOutputRange.max;
        profile.yRangeEditable = false;
    }
    profile.defaultEndValue = binding->outputMax;
    if (binding->address == BindingSemantics::volumeAddress(host->type)) {
        profile.defaultEndValue = -96.0f;
    }
    canvas_->setProfile(profile);

    const BindingSemantics::DisplayDomain domain = variable
        ? BindingSemantics::displayDomain(variable->rangeMin, variable->rangeMax)
        : BindingSemantics::displayDomain(0.0f, 1.0f);
    if (domain.degenerate)
        canvas_->setXDomain(0.0f, 1.0f);
    else
        canvas_->setXDomain(domain.min, domain.max);
    canvas_->setCurve(
        BindingSemantics::storedToDisplay(binding->curve, domain),
        BindingSemantics::storedToDisplay(binding->handles, domain));

    QVector<CurveOverlay> overlays;
    if (const auto* mix = BindingEditing::ledgerHost(host, binding->address)) {
        for (const ParameterBindingDef* other : mix->bindings.findAll(binding->address)) {
            if (!other || other->runtimeId() == binding->runtimeId())
                continue;
            CurveOverlay overlay;
            overlay.points = BindingSemantics::storedToDisplay(other->curve, domain);
            overlay.handles = BindingSemantics::storedToDisplay(other->handles, domain);
            overlay.colorRole = CurveColorRole::ParameterBinding;
            overlays.push_back(overlay);
        }
    }
    canvas_->setOverlays(overlays);
    refreshBindingPlayhead();

    applyCanvasSelection(preserveCanvasSelection, previousSelection, previousPrimarySelection);
    stack_->setCurrentIndex(1);
    updating_ = false;
}

void CurveEditorWidget::refreshBindingPlayhead()
{
    auto& project = ProjectManager::instance();
    const auto* binding = findBinding(project.findItemByGuid(selection_.hostGuid), selection_);
    const auto* item = binding ? project.findItemByGuid(binding->variable) : nullptr;
    const auto* variable = item ? item->comp<VVariable>() : nullptr;
    if (variable) {
        const float current = VariableSemantics::currentValue(
            ProjectManager::instance().variableRehearsal().overrideFor(binding->variable),
            variable->kind, variable->rangeMin, variable->rangeMax,
            variable->defaultValue);
        canvas_->setPlayhead(current);
    } else {
        canvas_->setPlayhead(std::nullopt);
    }
}

void CurveEditorWidget::showMessage(const QString& text)
{
    messageLabel_->setText(text);
    stack_->setCurrentIndex(0);
}

void CurveEditorWidget::onCanvasMaxDistanceEdited(float value)
{
    if (updating_ || !hasAttenuation())
        return;
    auto* dim = currentDimensionDef();
    if (!dim)
        return;
    attenuation_.maxDistance = value;
    canvas_->setXDomain(0.0f, std::max(1.0f, attenuation_.maxDistance));
    if (!dim->curve.isEmpty()) {
        const float delta = attenuation_.maxDistance - dim->curve.back().x;
        dim->curve.back().x = attenuation_.maxDistance;
        if (!dim->handles.isEmpty())
            dim->handles.back().h2x += delta;
        attenuationNormalizeCurve(dim->type, dim->curve, dim->handles);
    }
    executeCurveUpdate(dim->curve, dim->handles, tr("Change Radius Max"));
}

void CurveEditorWidget::onCanvasCurvePreviewed(const QVector<CurvePointDef>& points,
                                              const QVector<CurveHandlePairDef>& handles)
{
    if (updating_) return;
    auto& project = ProjectManager::instance();
    if (selection_.hasBinding()) {
        const auto binding = bindingWithCurve(points, handles);
        if (!binding) return;
        if (previewId_.isNull()) previewId_ = QUuid::createUuid();
        emit project.bindingCurvePreviewed(project.findItemByGuid(selection_.hostGuid),
            selection_.bindingStableId, previewId_, binding);
    } else if (hasAttenuation()) {
        auto preview = attenuation_;
        auto* dim = preview.dimension(dimension_);
        if (!dim) return;
        dim->curve = points;
        dim->handles = handles;
        if (previewId_.isNull()) previewId_ = QUuid::createUuid();
        emit project.attenuationCurvePreviewed(preview.guid, previewId_, preview);
    }
}

void CurveEditorWidget::finishCurvePreview()
{
    if (previewId_.isNull()) return;
    const auto previewId = previewId_;
    previewId_ = QUuid{};
    auto& project = ProjectManager::instance();
    if (selection_.hasBinding())
        emit project.bindingCurvePreviewed(project.findItemByGuid(selection_.hostGuid),
            selection_.bindingStableId, previewId, std::nullopt);
    else if (hasAttenuation())
        emit project.attenuationCurvePreviewed(attenuation_.guid, previewId, std::nullopt);
}

void CurveEditorWidget::onCanvasCurveEdited(const QVector<CurvePointDef>& points,
                                            const QVector<CurveHandlePairDef>& handles,
                                            const QString& editLabel)
{
    if (updating_)
        return;
    if (selection_.hasBinding()) {
        executeBindingUpdate(points, handles, editLabel);
        return;
    }
    auto* dim = currentDimensionDef();
    if (!dim)
        return;
    dim->curve = points;
    dim->handles = handles;
    executeCurveUpdate(points, handles, editLabel);
}

void CurveEditorWidget::onOutputRangeCommitted(float low, float high)
{
    if (updating_ || !selection_.hasBinding())
        return;
    ProjectItem* host = ProjectManager::instance().findItemByGuid(selection_.hostGuid);
    const ParameterBindingDef* current = findBinding(host, selection_);
    if (!host || !current)
        return;
    ParameterBindingDef binding = *current;
    if (binding.outputMin > binding.outputMax)
        std::swap(low, high);
    BindingSemantics::remapOutputDomain(binding, low, high);
    ActionManager::instance().execute(
        QStringLiteral("set_binding"),
        BindingEditing::upsertArgs(host, binding, tr("Edit Binding Output")),
        ActionSource::GUI, this);
}

AttenuationDimensionDef* CurveEditorWidget::currentDimensionDef()
{
    return attenuation_.dimension(dimension_);
}

const AttenuationDimensionDef* CurveEditorWidget::currentDimensionDef() const
{
    return attenuation_.dimension(dimension_);
}

bool CurveEditorWidget::executeCurveUpdate(const QVector<CurvePointDef>& points,
                                           const QVector<CurveHandlePairDef>& handles,
                                           const QString& undoText)
{
    if (!hasAttenuation())
        return false;
    const auto* dim = currentDimensionDef();
    if (!dim)
        return false;
    const auto result = ActionManager::instance().execute(
        QStringLiteral("set_attenuation_curve"),
        QJsonObject{
            {"name", attenuation_.name},
            {"attenuation_guid", attenuation_.guid.toString(QUuid::WithoutBraces)},
            {"curve", QLatin1String(attenuationDimensionName(dimension_))},
            {"points", pointsToJson(points, QStringLiteral("distance"))},
            {"handles", handlesToJson(handles)},
            {"max_distance", static_cast<double>(attenuation_.maxDistance)},
            {"undo_text", undoText}
        },
        ActionSource::GUI);
    return result.success;
}

bool CurveEditorWidget::executeBindingUpdate(
    const QVector<CurvePointDef>& displayPoints,
    const QVector<CurveHandlePairDef>& displayHandles,
    const QString& undoText)
{
    const auto binding = bindingWithCurve(displayPoints, displayHandles);
    if (!binding) return false;
    ProjectItem* host = ProjectManager::instance().findItemByGuid(selection_.hostGuid);
    return ActionManager::instance().execute(
        QStringLiteral("set_binding"),
        BindingEditing::upsertArgs(host, *binding, undoText),
        ActionSource::GUI, this).success;
}

std::optional<ParameterBindingDef> CurveEditorWidget::bindingWithCurve(
    const QVector<CurvePointDef>& displayPoints,
    const QVector<CurveHandlePairDef>& displayHandles) const
{
    ProjectItem* host = ProjectManager::instance().findItemByGuid(selection_.hostGuid);
    const ParameterBindingDef* current = findBinding(host, selection_);
    if (!host || !current)
        return std::nullopt;
    ProjectItem* variableItem = ProjectManager::instance().findItemByGuid(current->variable);
    const auto* variable = variableItem ? variableItem->comp<VVariable>() : nullptr;
    const BindingSemantics::DisplayDomain domain = variable
        ? BindingSemantics::displayDomain(variable->rangeMin, variable->rangeMax)
        : BindingSemantics::displayDomain(0.0f, 1.0f);
    ParameterBindingDef binding = *current;
    binding.curve = BindingSemantics::displayToStored(displayPoints, domain);
    binding.handles = BindingSemantics::displayToStored(displayHandles, domain);
    const auto contract = BindingContractSemantics::derive(binding.address.slotId, host->type);
    if (contract.outputUnit == BindingOutputUnit::Decibel) {
        const bool reversed = binding.outputMin > binding.outputMax;
        binding.outputMin = reversed ? contract.legalOutputRange.max : contract.legalOutputRange.min;
        binding.outputMax = reversed ? contract.legalOutputRange.min : contract.legalOutputRange.max;
    }
    return binding;
}

}
