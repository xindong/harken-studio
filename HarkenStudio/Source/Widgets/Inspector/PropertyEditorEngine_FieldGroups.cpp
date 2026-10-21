#include "PropertyEditorEngine.h"

#include "Core/ActionManager.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyEditSession.h"
#include "Core/QtControlSkin.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Drawers/ListFieldDrawer.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <algorithm>

namespace VNX {
namespace {

class InheritCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;

protected:
    void nextCheckState() override
    {
        const bool inherit = checkState() != Qt::Checked;
        setTristate(false);
        setChecked(inherit);
    }
};

QString groupName(FieldGroupId group)
{
    return QString::fromLatin1(FieldGroupName(group).data());
}

QVariant referenceEditorValue(const QVariant& value)
{
    const auto guid = value.toUuid();
    if (guid.isNull()) return QVariant::fromValue(uint32_t{0});
    const auto* target = ProjectManager::instance().findItemByGuid(guid);
    return target ? QVariant::fromValue(target->objectID)
                  : QVariant(guid.toString(QUuid::WithoutBraces));
}

std::optional<QJsonValue> referenceActionValue(const QVariant& value)
{
    bool ok = false;
    const uint id = value.toUInt(&ok);
    if (!ok) return {};
    if (id == 0) return QJsonValue(QUuid().toString(QUuid::WithoutBraces));
    const auto* item = ProjectManager::instance().findItemByObjectID(id);
    if (!item) return {};
    return QJsonValue(item->guid.toString(QUuid::WithoutBraces));
}

}

std::optional<FieldGroupId> PropertyEditorEngine::fieldGroup(const QString& field) const
{
    switch (itemType_) {
    case ProjectItem::Type::Sound:
    case ProjectItem::Type::Container:
    case ProjectItem::Type::Layer:
    case ProjectItem::Type::Track:
    case ProjectItem::Type::Piece:
        break;
    default:
        return {};
    }
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
    return property ? FieldGroupForProperty(
        static_cast<harkenplayableproperties::PlayablePropertyIdV1>(property->id))
        : std::nullopt;
}

void PropertyEditorEngine::buildGroupHeader(FieldGroupId group, CollapsibleSection* section, bool showRangeName)
{
    auto* header = section->headerWidget();
    if (!header) {
        header = new QWidget(section);
        header->setObjectName(QStringLiteral("fieldGroupHeader_") + groupName(group));
        auto* layout = new QHBoxLayout(header);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        section->setHeaderWidget(header);
    }
    QString label = tr("Inherit");
    if (showRangeName) {
        switch (group) {
        case FieldGroupId::MainOutput: label = tr("Inherit Main Output"); break;
        case FieldGroupId::Send: label = tr("Inherit Sends"); break;
        case FieldGroupId::SpatialRating: label = tr("Inherit Spatial Rating"); break;
        }
    }
    auto* inherit = new InheritCheckBox(label, header);
    inherit->setObjectName(QStringLiteral("inherit_") + groupName(group));
    inherit->setChecked(true);
    inherit->setEnabled(false);
    header->layout()->addWidget(inherit);
    auto& binding = groupBindings_[group];
    binding.inheritControls.push_back(inherit);
    binding.section = section;
    QWidget* menuHost = showRangeName ? static_cast<QWidget*>(inherit) : section;
    menuHost->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(menuHost, &QWidget::customContextMenuRequested, this,
        [this, group, menuHost](const QPoint& position) {
            showGroupMenu(group, menuHost->mapToGlobal(position));
        });
    connect(inherit, &QCheckBox::clicked, this, [this, group, inherit] {
        if (updating_) return;
        auto& binding = groupBindings_.at(group);
        if (!binding.view.canInherit || !groupContextMatches(binding.view, groupEditRevision_)) {
            refreshFieldGroups();
            return;
        }
        auto args = binding.view.actionArgs();
        args.insert(QStringLiteral("mode"), inherit->isChecked()
            ? QStringLiteral("Inherit") : QStringLiteral("Local"));
        executeGroupAction(group, QStringLiteral("set_field_group_mode"), std::move(args));
    });
}

bool PropertyEditorEngine::groupContextMatches(
    const FieldGroupInspectorView& view, quint64 revision) const
{
    if (!view.valid || revision != groupEditRevision_ || !currentItem_
        || view.definition != currentItem_->guid
        || view.context != PlatformContextService::instance().context())
        return false;
    const auto* item = ProjectManager::instance().findItemByGuid(view.definition);
    return item == currentItem_ && item->fieldGroups == view.expectedBefore;
}

void PropertyEditorEngine::cancelGroupEdits(bool preserveListRows)
{
    ++groupEditRevision_;
    const bool wasUpdating = updating_;
    updating_ = true;
    for (auto& binding : bindings_) {
        if (!fieldGroup(binding.field)) continue;
        finishGroupPreview(binding);
        binding.groupEditArgs.reset();
        if (auto* slider = qobject_cast<ParameterSlider*>(binding.widget.data()))
            slider->cancelPendingInput();
        if (auto* ref = qobject_cast<ObjectRefWidget*>(binding.widget.data()))
            ref->cancelPendingSelection();
        if (binding.drawer)
            ListFieldDrawer::cancelWidgetEdits(binding.widget, preserveListRows);
    }
    updating_ = wasUpdating;
}

void PropertyEditorEngine::previewGroupField(FieldBinding& binding, double value)
{
    if (updating_ || !binding.groupEditArgs || !canEditField(binding)) return;
    const auto group = fieldGroup(binding.field);
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(binding.field);
    auto& project = ProjectManager::instance();
    if (!group || !property || !project.currentProject()) return;
    const auto& view = groupBindings_.at(*group).view;
    const auto prepared = prepareFieldGroupEdit(*project.currentProject(), {
        currentItem_->guid, *group, view.editScopes,
        SetFieldGroupField{static_cast<harkenplayableproperties::PlayablePropertyIdV1>(property->id),
            static_cast<float>(value)}, view.expectedBefore});
    if (!prepared.ok()) return;
    binding.groupPreviewActive = true;
    emit project.fieldGroupsPreviewed(currentItem_, *prepared.after);
}

void PropertyEditorEngine::finishGroupPreview(FieldBinding& binding)
{
    if (!binding.groupPreviewActive) return;
    binding.groupPreviewActive = false;
    if (currentItem_)
        emit ProjectManager::instance().fieldGroupsPreviewed(currentItem_, currentItem_->fieldGroups);
}

void PropertyEditorEngine::previewGroupSends(FieldBinding& binding, const QVariant& value)
{
    if (updating_ || !canEditField(binding)) return;
    auto& manager = ProjectManager::instance();
    if (!manager.currentProject()) return;
    const auto& view = groupBindings_.at(FieldGroupId::Send).view;
    SendFieldGroup sends;
    for (const auto& entry : value.toList()) {
        const auto row = entry.toMap();
        bool valid = false;
        const auto id = row.value(QStringLiteral("targetBusID")).toUInt(&valid);
        const auto* target = valid ? manager.findItemByObjectID(id) : nullptr;
        if (!target || target->type != ProjectItem::Type::Bus) return;
        sends.sends.push_back({toFieldGroupGuid(target->guid), row.value(QStringLiteral("sendLevel")).toFloat()});
    }
    const auto prepared = prepareFieldGroupEdit(*manager.currentProject(), {
        currentItem_->guid, FieldGroupId::Send, view.editScopes,
        ReplaceFieldGroupSends{std::move(sends)}, view.expectedBefore});
    if (!prepared.ok()) return;
    binding.groupPreviewActive = true;
    emit manager.fieldGroupsPreviewed(currentItem_, *prepared.after);
}

void PropertyEditorEngine::previewBusSends(FieldBinding& binding, const QVariant& value)
{
    if (updating_ || !canEditField(binding) || currentItem_->type != ProjectItem::Type::Bus) return;
    if (!binding.editSession) {
        const auto& context = PlatformContextService::instance().context();
        const auto platforms = context.all || context.selected.isEmpty()
            ? std::optional<QVector<ProjectPlatformId>>{} : context.selected;
        binding.editSession = std::make_unique<PropertyEditSession>(
            currentItem_, binding.field, readField(binding.field), this, platforms);
    }
    binding.editSession->preview(value);
}

void PropertyEditorEngine::refreshFieldGroups()
{
    groupPreviews_.clear();
    const auto context = PlatformContextService::instance().context();
    const bool sameContext = currentItem_ && std::all_of(
        groupBindings_.begin(), groupBindings_.end(), [&](const auto& entry) {
            return entry.second.view.definition == currentItem_->guid
                && entry.second.view.context == context;
        });
    cancelGroupEdits(sameContext);
    const bool wasUpdating = updating_;
    updating_ = true;
    const auto* project = ProjectManager::instance().currentProject();
    for (auto& [group, binding] : groupBindings_) {
        binding.view = project && currentItem_
            ? buildFieldGroupInspectorView(*project, currentItem_->guid, group, context)
            : FieldGroupInspectorView{};
        const auto& view = binding.view;
        QString tooltip = project ? view.sourceTooltip(*project) : QString();
        if (!view.diagnostics.isEmpty()) tooltip = view.diagnostics.join(QLatin1Char('\n'));
        if (!binding.error.isEmpty()) tooltip.prepend(binding.error + QLatin1Char('\n'));
        for (const auto& inherit : binding.inheritControls) {
            if (!inherit) continue;
            QSignalBlocker blocker(inherit);
            inherit->setEnabled(view.valid && view.canInherit);
            inherit->setTristate(view.inheritanceMixed);
            inherit->setCheckState(view.inheritanceMixed ? Qt::PartiallyChecked
                : view.effectiveInherit ? Qt::Checked : Qt::Unchecked);
            inherit->setToolTip(tooltip);
        }
    }
    for (auto& binding : bindings_)
        if (fieldGroup(binding.field)) applyGroupFieldView(binding);
    updating_ = wasUpdating;
    evalEditConditions();
}

void PropertyEditorEngine::refreshFieldGroupPreview(
    ProjectItem* provider, const DefinitionFieldGroups& groups)
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!currentItem_ || !provider || !project || groupBindings_.empty()) return;
    if (groups == provider->fieldGroups) groupPreviews_.erase(provider->guid);
    else groupPreviews_[provider->guid] = groups;
    if (updating_) return;
    std::map<FieldGroupScope, FieldGroupEffectiveSnapshot> snapshots;
    for (const auto& [group, binding] : groupBindings_) {
        if (group != FieldGroupId::MainOutput && group != FieldGroupId::Send) continue;
        if (!binding.view.valid) continue;
        for (const auto& entry : binding.view.perScope) {
            if (snapshots.contains(entry.scope)) continue;
            auto snapshot = buildFieldGroupEffectiveSnapshot(*project, entry.scope,
                [this](const ProjectItem& item) {
                    const auto preview = groupPreviews_.find(item.guid);
                    return preview == groupPreviews_.end() ? &item.fieldGroups : &preview->second;
                });
            if (!snapshot.ok()) return;
            snapshots.emplace(entry.scope, std::move(snapshot));
        }
    }
    for (auto& binding : bindings_) {
        if ((binding.field != QLatin1String("outputBusVolume") && binding.field != QLatin1String("sends"))
            || !binding.widget || binding.groupPreviewActive
            || (binding.parameterBinding && binding.parameterBinding->isEditing())) continue;
        const auto parameters = binding.widget->findChildren<ParameterBinding*>();
        if (std::any_of(parameters.begin(), parameters.end(), [](const auto* parameter) {
                return parameter->isEditing();
            })) continue;
        const auto group = fieldGroup(binding.field);
        if (!group) continue;
        const auto found = groupBindings_.find(*group);
        if (found == groupBindings_.end() || !found->second.view.valid) continue;
        auto view = found->second.view;
        const auto preview = groupPreviews_.find(currentItem_->guid);
        if (preview != groupPreviews_.end()) view.expectedBefore = preview->second;
        for (auto& entry : view.perScope) {
            const auto& definitions = snapshots.at(entry.scope).definitions;
            const auto effective = definitions.find(toFieldGroupGuid(currentItem_->guid));
            if (effective == definitions.end()) return;
            entry.resolved.payload = *group == FieldGroupId::MainOutput
                ? FieldGroupPayload(effective->second.mainOutput) : FieldGroupPayload(effective->second.send);
        }
        const auto value = groupFieldValue(binding.field, &view);
        const bool mixed = !value.isValid();
        if (binding.drawer) {
            binding.drawer->bind(binding.widget, mixed ? QVariant(QVariantList{}) : value, currentItem_);
            ListFieldDrawer::setWidgetMixed(binding.widget, mixed);
        } else if (mixed) {
            if (auto* slider = qobject_cast<ParameterSlider*>(binding.widget.data()))
                slider->setMixedValue(true);
        } else {
            setGroupFieldWidgetValue(binding, value);
        }
    }
}

QVariant PropertyEditorEngine::groupFieldValue(
    const QString& field, const FieldGroupInspectorView* preview) const
{
    const auto group = fieldGroup(field);
    if (!group) return {};
    const auto found = groupBindings_.find(*group);
    if (found == groupBindings_.end()) return {};
    const auto& view = preview ? *preview : found->second.view;
    auto value = view.field(field);
    if (*group == FieldGroupId::Send && view.editScopes.shared && view.fieldsEditable) {
        QVariantList rows;
        if (view.expectedBefore.send.shared.localHistory)
            for (const auto& row : view.expectedBefore.send.shared.localHistory->sends)
                rows.append(QVariantMap{{QStringLiteral("targetBusID"), fromFieldGroupGuid(row.targetBusID)},
                    {QStringLiteral("sendLevel"), row.sendLevel}});
        value.value = rows;
        value.uniform = true;
    }
    if (!view.valid || !value.uniform) return {};
    if (value.value.metaType().id() == QMetaType::QUuid)
        return referenceEditorValue(value.value);
    if (*group != FieldGroupId::Send) return value.value;
    QVariantList rows;
    for (const auto& entry : value.value.toList()) {
        auto row = entry.toMap();
        row[QStringLiteral("targetBusID")] = referenceEditorValue(row.value(QStringLiteral("targetBusID")));
        rows.append(row);
    }
    return rows;
}

bool PropertyEditorEngine::groupReferenceSelectable(const QString& field, uint32_t candidate) const
{
    const auto group = fieldGroup(field);
    const auto* target = ProjectManager::instance().findItemByObjectID(candidate);
    if (!group || !target) return false;
    if (*group == FieldGroupId::SpatialRating)
        return target->type == ProjectItem::Type::Attenuation;
    if (target->type != ProjectItem::Type::Bus) return false;
    const auto peer = groupBindings_.find(*group == FieldGroupId::MainOutput
        ? FieldGroupId::Send : FieldGroupId::MainOutput);
    if (peer == groupBindings_.end() || !peer->second.view.valid) return false;
    if (*group == FieldGroupId::Send) {
        const auto mainOutput = peer->second.view.field(QStringLiteral("mainOutputBusRef"));
        for (const auto& scope : mainOutput.perScope)
            if (scope.second.toUuid() == target->guid) return false;
    } else {
        const auto sends = peer->second.view.field(QStringLiteral("sends"));
        for (const auto& scope : sends.perScope)
            for (const auto& row : scope.second.toList())
                if (row.toMap().value(QStringLiteral("targetBusID")).toUuid() == target->guid)
                    return false;
    }
    return true;
}

void PropertyEditorEngine::setGroupFieldWidgetValue(const FieldBinding& binding, const QVariant& value)
{
    if (binding.field == QLatin1String("outputBusVolume"))
        if (auto* slider = qobject_cast<ParameterSlider*>(binding.widget.data()))
            slider->binding()->setRange(std::min(-96.0, value.toDouble()), std::max(24.0, value.toDouble()));
    setWidgetValue(binding.widget, binding.widgetType, value);
}

bool PropertyEditorEngine::applyGroupFieldView(FieldBinding& binding)
{
    const auto group = fieldGroup(binding.field);
    if (!group || !binding.widget) return false;
    const auto found = groupBindings_.find(*group);
    if (found == groupBindings_.end()) return false;
    const auto& view = found->second.view;
    const QVariant value = groupFieldValue(binding.field);
    const bool mixed = !value.isValid();
    const auto guard = [self = QPointer<PropertyEditorEngine>(this), field = binding.field,
                        view, revision = groupEditRevision_] {
        if (!self || !self->groupContextMatches(view, revision)) return false;
        for (const auto& candidate : self->bindings_)
            if (candidate.field == field) return self->canEditField(candidate);
        return false;
    };
    if (auto* ref = qobject_cast<ObjectRefWidget*>(binding.widget.data())) {
        ref->setEditGuard(guard);
        binding.drawer->bind(ref, value, currentItem_);
    } else if (binding.drawer) {
        ListFieldDrawer::setWidgetEditGuard(binding.widget, guard);
        if (*group == FieldGroupId::Send) {
            const auto self = QPointer<PropertyEditorEngine>(this);
            ListFieldDrawer::setWidgetPreviewCallbacks(binding.widget,
                [self, field = binding.field](const QVariant& preview) {
                    if (!self) return;
                    for (auto& entry : self->bindings_)
                        if (entry.field == field) { self->previewGroupSends(entry, preview); return; }
                },
                [self, field = binding.field] {
                    if (!self) return;
                    for (auto& entry : self->bindings_)
                        if (entry.field == field) { self->finishGroupPreview(entry); return; }
                });
        }
        binding.drawer->bind(binding.widget, mixed ? QVariant(QVariantList{}) : value, currentItem_);
        ListFieldDrawer::setWidgetMixed(binding.widget, mixed);
    } else if (mixed) {
        if (auto* slider = qobject_cast<ParameterSlider*>(binding.widget.data()))
            slider->setMixedValue(true);
        if (auto* check = qobject_cast<QCheckBox*>(binding.widget.data())) {
            QSignalBlocker blocker(check);
            check->setTristate(true);
            check->setCheckState(Qt::PartiallyChecked);
        }
    } else {
        setGroupFieldWidgetValue(binding, value);
    }
    if (const auto* project = ProjectManager::instance().currentProject()) {
        QString tooltip = view.sourceTooltip(*project, binding.field);
        if (mixed) tooltip.prepend(*group == FieldGroupId::Send
            ? tr("Mixed table. Select a single platform to edit.\n") : tr("Mixed values across platforms.\n"));
        binding.mixedTooltip = tooltip;
        binding.widget->setToolTip(tooltip);
    }
    if (binding.decorator) binding.decorator->bind(currentItem_);
    return true;
}

bool PropertyEditorEngine::canEditField(const FieldBinding& binding) const
{
    if (!currentItem_ || binding.readOnly || !binding.widget) return false;
    if ((binding.field == QLatin1String("outputBusId")
         || binding.field == QLatin1String("outputBusVolume"))
        && currentItem_ == ProjectManager::instance().getMasterBusItem())
        return false;
    if (!binding.editCondition.isEmpty()
        && !evalCondition(binding.editCondition, [this](const QString& field) { return readField(field); }))
        return false;
    const auto group = fieldGroup(binding.field);
    if (!group) {
        const auto* property = PlayablePropertyAuthoringV1::fieldByName(binding.field);
        const auto& context = PlatformContextService::instance().context();
        return !property || context.selected.isEmpty()
            || context.all
            || (binding.type != PropType::ObjectRef && binding.type != PropType::List);
    }
    const auto found = groupBindings_.find(*group);
    if (found == groupBindings_.end()) return false;
    const auto& view = found->second.view;
    return groupContextMatches(view, groupEditRevision_) && view.fieldsEditable
        && (*group != FieldGroupId::Send || view.editScopes.shared || view.sendsUniform);
}

void PropertyEditorEngine::commitFieldChange(
    const QString& field, const QVariant& value, const std::optional<QJsonObject>& groupArgs)
{
    if (updating_ || !currentItem_) return;
    const auto found = std::find_if(bindings_.begin(), bindings_.end(),
        [&field](const FieldBinding& binding) { return binding.field == field; });
    if (found == bindings_.end() || !canEditField(*found)) return;
    const auto group = fieldGroup(field);
    if (!group) {
        if (currentItem_->type == ProjectItem::Type::Bus && field == QLatin1String("sends")
            && found->editSession) {
            auto session = std::move(found->editSession);
            session->preview(value);
            session->commit();
            return;
        }
        emit fieldChanged(field, readField(field), value);
        return;
    }
    auto args = groupArgs.value_or(groupBindings_.at(*group).view.actionArgs());
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
    if (!property) return;
    QString action = QStringLiteral("set_field_group_field");
    if (*group == FieldGroupId::Send) {
        QJsonArray rows;
        for (const auto& entry : value.toList()) {
            const auto row = entry.toMap();
            const auto ref = referenceActionValue(row.value(QStringLiteral("targetBusID")));
            if (!ref) return;
            rows.append(QJsonObject{{QStringLiteral("targetBusID"), *ref},
                {QStringLiteral("sendLevel"), QJsonValue::fromVariant(row.value(QStringLiteral("sendLevel")))}});
        }
        action = QStringLiteral("replace_field_group_sends");
        args.insert(QStringLiteral("sends"), rows);
    } else {
        args.insert(QStringLiteral("field"), QString::fromLatin1(property->jsonKey));
        if (property->referenceRole == harkenplayableproperties::PlayablePropertyReferenceRoleV1::MainOutputBus
            || property->referenceRole == harkenplayableproperties::PlayablePropertyReferenceRoleV1::Attenuation) {
            const auto ref = referenceActionValue(value);
            if (!ref) return;
            args.insert(QStringLiteral("value"), *ref);
        } else {
            args.insert(QStringLiteral("value"), QJsonValue::fromVariant(value));
        }
    }
    executeGroupAction(*group, action, std::move(args));
}

void PropertyEditorEngine::executeGroupAction(FieldGroupId group, const QString& action, QJsonObject args)
{
    cancelGroupEdits(action == QStringLiteral("set_field_group_field")
        || action == QStringLiteral("replace_field_group_sends"));
    const auto result = ActionManager::instance().execute(action, args, ActionSource::GUI, this);
    QString error;
    if (!result.success) {
        for (const auto& diagnostic : result.data.value(QStringLiteral("diagnostics")).toArray()) {
            const auto detail = diagnostic.toObject();
            if (!error.isEmpty()) error += QLatin1Char('\n');
            error += QStringLiteral("%1 / %2: %3").arg(detail.value(QStringLiteral("scope")).toString(),
                detail.value(QStringLiteral("field")).toString(), detail.value(QStringLiteral("message")).toString());
        }
        if (error.isEmpty()) error = result.errorMessage;
    }
    groupBindings_.at(group).error = error;
    refreshFieldGroups();
    if (result.success) emit fieldGroupChanged();
}

void PropertyEditorEngine::showGroupMenu(FieldGroupId group, const QPoint& globalPosition)
{
    refreshFieldGroups();
    auto& binding = groupBindings_.at(group);
    if (!binding.view.valid || !binding.section) return;
    auto* menu = new QMenu(binding.section);
    menu->setObjectName(QStringLiteral("fieldGroupMenu_") + groupName(group));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const auto add = [&](const QString& title, const QString& mode, bool rebuild = false) {
        auto* action = menu->addAction(title);
        action->setObjectName(rebuild ? QStringLiteral("rebuildLocal") : mode);
        if (rebuild) action->setEnabled(binding.view.canInherit);
        connect(action, &QAction::triggered, this,
            [this, group, mode, rebuild, view = binding.view, revision = groupEditRevision_] {
                if (!groupContextMatches(view, revision) || (rebuild && !view.canInherit)) return;
                auto args = view.actionArgs();
                if (!rebuild) args.insert(QStringLiteral("mode"), mode);
                executeGroupAction(group, rebuild ? QStringLiteral("rebuild_field_group_local")
                    : QStringLiteral("set_field_group_mode"), std::move(args));
            });
    };
    if (!binding.view.editScopes.shared) {
        add(tr("Reset Inherit"), QStringLiteral("UseShared"));
        menu->addSeparator();
    }
    add(tr("Rebuild Local from Current Effective Values"), {}, true);
    menu->popup(globalPosition);
}

}
