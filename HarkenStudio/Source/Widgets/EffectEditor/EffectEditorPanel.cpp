#include "EffectEditorPanel.h"

#include "Core/ActionManager.h"
#include "Core/Components/VEffect.h"
#include "Core/Components/VMixNode.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyEditSession.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeLayout.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Inspector/FieldGroupInspectorView.h"
#include "Widgets/Shared/InspectorFormRow.h"

#include <QChildEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QScopedValueRollback>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace VNX {

struct EffectEditorPanel::Selection
{
    QUuid ownerGuid;
    QUuid rowGuid;
    QUuid presetGuid;
    PlatformContext context;
    EffectDefinition definition;
    EffectChainLedger chain;
    FieldGroupInspectorView groupView;
    bool grouped = false;
    bool editable = false;

    bool operator==(const Selection& other) const
    {
        return ownerGuid == other.ownerGuid && rowGuid == other.rowGuid
            && presetGuid == other.presetGuid && context == other.context
            && definition.toJson() == other.definition.toJson() && chain == other.chain
            && groupView.expectedBefore == other.groupView.expectedBefore
            && grouped == other.grouped && editable == other.editable;
    }
};

std::unique_ptr<EffectEditorPanel::Selection> EffectEditorPanel::resolveSelection(
    const ProjectItem* item, const QUuid& rowGuid)
{
    if (!item) return {};
    auto selection = std::make_unique<Selection>();
    selection->ownerGuid = item->guid;
    selection->rowGuid = rowGuid;
    selection->context = PlatformContextService::instance().context();
    if (rowGuid.isNull()) {
        const auto* effect = item->type == ProjectItem::Type::Effect ? item->comp<VEffect>() : nullptr;
        if (!effect || !EffectChainSemantics::validate(effect->definition)) return {};
        selection->presetGuid = item->guid;
        selection->definition = effect->definition;
        selection->editable = true;
        return selection;
    }
    const auto* mix = item->comp<VMixNode>();
    const auto* project = ProjectManager::instance().currentProject();
    if (!mix || !project) return {};
    const auto& context = selection->context;
    selection->chain = mix->effectsForPlatform(context.all ? std::nullopt : currentWorldPlatformScope(context));
    selection->editable = context.all || context.selected.size() == 1;
    if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
        selection->grouped = true;
        selection->groupView = buildFieldGroupInspectorView(*project, item->guid,
            FieldGroupId::Effect, context);
        const auto effective = selection->groupView.effectChain();
        if (!effective) return {};
        selection->chain = *effective;
        selection->editable = selection->groupView.fieldsEditable;
    }
    const auto& entries = selection->chain.entries();
    const auto row = std::find_if(entries.begin(), entries.end(), [&rowGuid](const EffectEntry& entry) {
        return entry.guid == rowGuid;
    });
    if (row == entries.end()) return {};
    const auto* definition = EffectChainSemantics::resolve(*row, *project);
    if (!definition) return {};
    selection->definition = *definition;
    if (row->mode == EffectMode::Preset) {
        selection->presetGuid = row->effectGuid;
        selection->editable = true;
    }
    return selection;
}

EffectEditorPanel::EffectEditorPanel(QWidget* parent, Presentation presentation)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("EffectEditorPanel"));
    setFocusPolicy(Qt::StrongFocus);
    auto* layout = new QVBoxLayout(this);
    ThemeLayout::margins(layout, ThemeLayout::Space::Panel);
    layout->setSpacing(0);
    if (presentation == Presentation::Embedded) {
        ThemeLayout::spacing(layout, ThemeLayout::Space::Control);
        auto* header = new QWidget(this);
        header->setObjectName(QStringLiteral("effectEditorHeader"));
        auto* headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        ThemeLayout::spacing(headerLayout, ThemeLayout::Space::Control);
        auto* leadingSpace = new QWidget(header);
        headerLayout->addWidget(leadingSpace);
        auto* title = new QLabel(tr("Details"), header);
        title->setAlignment(Qt::AlignCenter);
        QtControlSkin::setSemanticProperty(title, "textRole", QStringLiteral("title"));
        headerLayout->addWidget(title, 1);
        detachButton_ = new QToolButton(header);
        detachButton_->setObjectName(QStringLiteral("effectEditorDetach"));
        detachButton_->setAutoRaise(true);
        detachButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        QtControlSkin::setSemanticProperty(detachButton_, "flatIconButton", true);
        detachButton_->setToolTip(tr("Open Effect Editor in New Window"));
        detachButton_->setAccessibleName(detachButton_->toolTip());
        ThemeManager::instance().bindMetrics(detachButton_, [this, leadingSpace](const ThemeMetrics& metrics) {
            leadingSpace->setFixedWidth(metrics.controlHeight);
            detachButton_->setIconSize(QSize(metrics.iconExtent, metrics.iconExtent));
            detachButton_->setFixedSize(metrics.controlHeight, metrics.controlHeight);
        });
        const auto refreshIcon = [this] {
            detachButton_->setIcon(ThemeManager::instance().resolveIcon(
                QString::fromLatin1(ThemeAssetIds::InspectorDetach)).icon);
        };
        refreshIcon();
        connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged,
                detachButton_, refreshIcon);
        connect(detachButton_, &QToolButton::clicked, this, [this] {
            if (hasEffect()) emit detachRequested(ownerGuid_, rowGuid_);
        });
        headerLayout->addWidget(detachButton_);
        layout->addWidget(header);
    }
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    fields_ = new QWidget(scroll);
    fields_->setObjectName(QStringLiteral("effectParameters"));
    auto* fieldsLayout = new QVBoxLayout(fields_);
    fieldsLayout->setContentsMargins(0, 0, 0, 0);
    ThemeLayout::spacing(fieldsLayout, ThemeLayout::Space::Control);
    const auto& schema = VEffect::schema();
    for (size_t index = 0; index < parameters_.size(); ++index) {
        const auto& field = schema[index];
        auto* parameter = new ParameterSlider(field.label, fields_);
        parameter->setObjectName(QStringLiteral("effectParameter_") + field.field);
        parameter->setPresenterContext(ParameterPresenterContext::EffectParameter);
        parameter->setRange(field.min.toDouble(), field.max.toDouble(), field.decimals);
        parameter->setSuffix(field.unit);
        parameter->binding()->setDefaultValue(field.def.toDouble());
        ThemeManager::instance().bindMetrics(parameter, [parameter](const ThemeMetrics& metrics) {
            parameter->setCompactValueColumn(metrics.inspector.valueColumnWidth);
            parameter->setValueColumnStretch(7, 3);
        });
        fieldsLayout->addWidget(makeInspectorFormRow(fields_, field.field, field.label,
            parameter, {}, 0).row);
        parameters_[index] = parameter;
        connect(parameter, &ParameterSlider::valueChanged, this,
            [this, index](double value) { commitParameter(index, value); });
        connect(parameter, &ParameterSlider::interactivePreview, this,
            [this, index](double value) { previewParameter(index, value); });
        connect(parameter, &ParameterSlider::interactiveEditEnd, this,
            [this, index](double start, double value, bool committed) {
                finishParameter(index, value, committed && start != value);
            });
    }
    fieldsLayout->addStretch();
    scroll->setWidget(fields_);
    layout->addWidget(scroll);

    auto& project = ProjectManager::instance();
    connect(&project, &ProjectManager::projectClosed, this, &EffectEditorPanel::clearEditor);
    connect(&project, &ProjectManager::propertyEditsCancelling, this,
        [this](ProjectItem* item, const QString& field) {
            if (!item || !selection_) return;
            if ((!selection_->presetGuid.isNull() && item->guid == selection_->presetGuid)
                || (selection_->presetGuid.isNull() && item->guid == ownerGuid_
                    && field == QLatin1String("effects"))) cancelPendingEdits();
        });
    connect(&project, &ProjectManager::activePropertyEditsFinishing, this,
        [this](bool commit) {
            for (auto* parameter : parameters_) {
                if (commit) parameter->binding()->commitEdit();
                else parameter->cancelPendingInput();
            }
        });
    connect(&project, &ProjectManager::itemDeleted, this, [this] {
        if (!ownerGuid_.isNull() && !boundItem())
            clearEditor();
        else
            refreshEffect();
    });
    connect(&project, &ProjectManager::itemCreated, this, [this] { refreshEffect(); });
    connect(&project, &ProjectManager::itemRenamed, this, [this] { refreshEffect(); });
    connect(&project, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
        if (item && (property == QLatin1String("fieldGroups")
            || (item->guid == ownerGuid_ && property == QLatin1String("effects"))
            || item->type == ProjectItem::Type::Effect))
            QMetaObject::invokeMethod(this, &EffectEditorPanel::refreshEffect, Qt::QueuedConnection);
    });
    connect(&project, &ProjectManager::itemMoved, this, &EffectEditorPanel::refreshEffect);
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, &EffectEditorPanel::refreshEffect);

    refreshEffect();
    installFocusFilterRecursively(this);
}

EffectEditorPanel::~EffectEditorPanel() { cancelPendingEdits(); }

ProjectItem* EffectEditorPanel::boundItem() const
{
    return ProjectManager::instance().findItemByGuid(ownerGuid_);
}

bool EffectEditorPanel::hasEffect() const
{
    return canOpen(boundItem(), rowGuid_);
}

bool EffectEditorPanel::canOpen(const ProjectItem* item, const QUuid& rowGuid)
{
    return static_cast<bool>(resolveSelection(item, rowGuid));
}

void EffectEditorPanel::setPinned(bool pinned)
{
    if (pinned_ == pinned) return;
    pinned_ = pinned;
    emit pinToggled(pinned_);
}

void EffectEditorPanel::bindEffect(const QUuid& ownerGuid, const QUuid& rowGuid)
{
    cancelPendingEdits();
    ownerGuid_ = ownerGuid;
    rowGuid_ = rowGuid;
    refreshEffect();
}

void EffectEditorPanel::clearEditor()
{
    cancelPendingEdits();
    ownerGuid_ = QUuid{};
    rowGuid_ = QUuid{};
    refreshEffect();
}

void EffectEditorPanel::refreshEffect()
{
    QString title = tr("Effect Editor");
    const auto* item = boundItem();
    auto selection = resolveSelection(item, rowGuid_);
    if (selection) {
        const auto* preset = rowGuid_.isNull() ? nullptr
            : ProjectManager::instance().findItemByGuid(selection->presetGuid);
        title += QStringLiteral(" - %1/%2").arg(item->name,
            preset ? preset->name : EffectChainSemantics::displayName(selection->definition.type));
    }
    setWindowTitle(title);
    if (detachButton_) detachButton_->setEnabled(static_cast<bool>(selection));
    if (selection_ && selection && *selection_ == *selection) return;
    cancelPendingEdits();
    selection = resolveSelection(boundItem(), rowGuid_);
    selection_ = std::move(selection);
    fields_->setVisible(static_cast<bool>(selection_));
    fields_->setEnabled(selection_ && selection_->editable);
    if (!selection_) return;
    QScopedValueRollback updating(updating_, true);
    for (size_t index = 0; index < parameters_.size(); ++index)
        parameters_[index]->setValue(selection_->definition.parameters[index]);
}

void EffectEditorPanel::cancelPendingEdits()
{
    QScopedValueRollback updating(updating_, true);
    for (auto* parameter : parameters_)
        parameter->cancelPendingInput();
    clearParameterPreview();
}

void EffectEditorPanel::clearParameterPreview()
{
    editSession_.reset();
    if (!groupPreviewActive_) return;
    groupPreviewActive_ = false;
    if (auto* item = boundItem())
        emit ProjectManager::instance().fieldGroupsPreviewed(item, item->fieldGroups);
}

void EffectEditorPanel::finishParameter(size_t index, double value, bool committed)
{
    clearParameterPreview();
    if (!updating_ && committed) commitParameter(index, value);
}

void EffectEditorPanel::previewParameter(size_t index, double value)
{
    if (updating_ || !selection_ || !selection_->editable || !isEnabled()) return;
    auto& manager = ProjectManager::instance();
    auto* item = boundItem();
    if (!item || !manager.currentProject() ||
        selection_->context != PlatformContextService::instance().context()) return;
    auto definition = selection_->definition;
    definition.parameters[index] = static_cast<float>(value);
    if (!std::isfinite(value) || !EffectChainSemantics::validate(definition)) return;
    if (!selection_->presetGuid.isNull()) {
        auto* preset = manager.findItemByGuid(selection_->presetGuid);
        if (!preset) return;
        if (!editSession_) editSession_ = std::make_unique<PropertyEditSession>(preset,
            VEffect::schema()[index].field, selection_->definition.parameters[index], this);
        editSession_->preview(value);
        return;
    }
    auto effects = selection_->chain.entries();
    const auto row = std::find_if(effects.begin(), effects.end(), [this](const auto& entry) {
        return entry.guid == rowGuid_;
    });
    if (row == effects.end()) return;
    row->custom = std::move(definition);
    EffectFieldGroup group;
    if (!group.effects.replace(std::move(effects))) return;
    if (selection_->grouped) {
        const auto& view = selection_->groupView;
        const auto prepared = prepareFieldGroupEdit(*manager.currentProject(), {
            item->guid, FieldGroupId::Effect, view.editScopes,
            ReplaceFieldGroupEffects{std::move(group)}, view.expectedBefore});
        if (!prepared.ok()) return;
        groupPreviewActive_ = true;
        emit manager.fieldGroupsPreviewed(item, *prepared.after);
    } else {
        const auto& context = selection_->context;
        if (!editSession_) editSession_ = std::make_unique<PropertyEditSession>(item,
            QStringLiteral("effects"), selection_->chain.toJson().toVariantList(), this,
            context.all ? std::optional<QVector<ProjectPlatformId>>{} : context.selected);
        editSession_->preview(group.effects.toJson().toVariantList());
    }
}

void EffectEditorPanel::commitParameter(size_t index, double value)
{
    if (updating_ || !selection_ || !selection_->editable || !isEnabled()) return;
    auto current = resolveSelection(boundItem(), rowGuid_);
    if (!current || *current != *selection_) {
        refreshEffect();
        return;
    }
    auto definition = current->definition;
    definition.parameters[index] = static_cast<float>(value);
    if (!std::isfinite(value) || !EffectChainSemantics::validate(definition)) {
        parameters_[index]->setValue(current->definition.parameters[index]);
        return;
    }
    if (definition.parameters == current->definition.parameters) return;
    QJsonObject args;
    QString command = QStringLiteral("set_item_property");
    if (!current->presetGuid.isNull()) {
        const auto* preset = ProjectManager::instance().findItemByGuid(current->presetGuid);
        if (!preset) return;
        args = {{QStringLiteral("authoring_handle"), static_cast<qint64>(preset->objectID)},
                {QStringLiteral("property"), VEffect::schema()[index].field},
                {QStringLiteral("value"), value}};
    } else {
        auto effects = current->chain.entries();
        const auto row = std::find_if(effects.begin(), effects.end(), [this](const EffectEntry& entry) {
            return entry.guid == rowGuid_;
        });
        row->custom = std::move(definition);
        EffectChainLedger chain;
        if (!chain.replace(std::move(effects))) return;
        if (current->grouped) {
            command = QStringLiteral("replace_field_group_effects");
            args = current->groupView.actionArgs();
            args.insert(QStringLiteral("effects"), chain.toJson());
        } else {
            args = {{QStringLiteral("authoring_handle"), static_cast<qint64>(boundItem()->objectID)},
                    {QStringLiteral("property"), QStringLiteral("effects")},
                    {QStringLiteral("value"), chain.toJson()}};
            if (!current->context.all && !current->context.selected.isEmpty()) {
                QJsonArray platforms;
                for (const auto& platform : current->context.selected)
                    platforms.append(ProjectPlatformSemantics::idString(platform));
                args.insert(QStringLiteral("platform_ids"), platforms);
            }
        }
    }
    const auto result = ActionManager::instance().execute(command, args, ActionSource::GUI, this);
    if (!result.success) parameters_[index]->setValue(current->definition.parameters[index]);
    refreshEffect();
}

QVariantMap EffectEditorPanel::snapshotState() const
{
    return {
        {QStringLiteral("ownerGuid"), ownerGuid_.toString(QUuid::WithoutBraces)},
        {QStringLiteral("rowGuid"), rowGuid_.toString(QUuid::WithoutBraces)}
    };
}

void EffectEditorPanel::restoreState(const QVariantMap& state)
{
    bindEffect(state.value(QStringLiteral("ownerGuid")).toUuid(),
               state.value(QStringLiteral("rowGuid")).toUuid());
}

void EffectEditorPanel::installFocusFilterRecursively(QObject* root)
{
    root->installEventFilter(this);
    for (QObject* child : root->children())
        installFocusFilterRecursively(child);
}

bool EffectEditorPanel::event(QEvent* event)
{
    if (event->type() == QEvent::Hide) cancelPendingEdits();
    return QWidget::event(event);
}

bool EffectEditorPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusIn && watched != this)
        emit panelFocused(panelId_);
    else if (event->type() == QEvent::ChildAdded)
        installFocusFilterRecursively(static_cast<QChildEvent*>(event)->child());
    return QWidget::eventFilter(watched, event);
}

void EffectEditorPanel::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

}
