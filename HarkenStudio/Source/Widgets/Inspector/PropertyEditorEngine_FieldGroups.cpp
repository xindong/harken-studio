#include "PropertyEditorEngine.h"
#include "Core/ThemeLayout.h"

#include "Core/ActionManager.h"
#include "Core/Components/VMixNode.h"
#include "Core/ConversionEditing.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyEditSession.h"
#include "Core/QtControlSkin.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Drawers/ListFieldDrawer.h"
#include "Widgets/Shared/PlatformLocalMarker.h"
#include "Core/FilterAuthoring.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <algorithm>

namespace harken::studio {
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

const QString kRouteTarget = QStringLiteral("targetBusID");

QStringList routeCells(const QString& field)
{
    if (field == QLatin1String("sidechains"))
        return {kRouteTarget, QStringLiteral("sendLevel"), QStringLiteral("duckTarget")};
    return {kRouteTarget, QStringLiteral("sendLevel")};
}

template <typename Convert>
std::optional<QVariantList> convertRouteTargets(const QVariantList& rows, Convert&& convert)
{
    QVariantList result;
    for (const auto& entry : rows) {
        auto row = entry.toMap();
        const auto shared = convert(row.value(kRouteTarget));
        if (!shared) return {};
        row[kRouteTarget] = *shared;
        result.append(row);
    }
    return result;
}

std::optional<QVariant> editorTarget(const QVariant& value)
{
    return referenceEditorValue(value);
}

std::optional<QVariant> actionTarget(const QVariant& value)
{
    const auto ref = referenceActionValue(value);
    return ref ? std::optional<QVariant>(ref->toVariant()) : std::nullopt;
}

QVariantList mergeRouteRows(const QVariantList& raw, const QVariantList& displayed, const QVariantList& edited,
                            const QStringList& cells, const std::optional<QString>& platform)
{
    const QString idKey = QStringLiteral("id");
    const QString platformsKey = QStringLiteral("platforms");
    QHash<QString, QVariantMap> rawById;
    for (const auto& entry : raw) rawById.insert(entry.toMap().value(idKey).toString(), entry.toMap());
    QHash<QString, QVariantMap> shownById;
    for (const auto& entry : displayed) shownById.insert(entry.toMap().value(idKey).toString(), entry.toMap());
    QSet<QString> used;
    QVariantList result;
    for (const auto& entry : edited) {
        const auto row = entry.toMap();
        auto id = row.value(idKey).toString();
        const auto found = rawById.constFind(id);
        if (found == rawById.cend() || used.contains(id)) {
            if (id.isEmpty() || used.contains(id)) id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QVariantMap created{{idKey, id}};
            for (const auto& cell : cells) created.insert(cell, row.value(cell));
            used.insert(id);
            result.append(created);
            continue;
        }
        used.insert(id);
        auto merged = *found;
        const auto shown = shownById.value(id);
        auto platforms = merged.value(platformsKey).toMap();
        for (const auto& cell : cells) {
            if (row.value(cell) == shown.value(cell)) continue;
            if (platform && cell != kRouteTarget) {
                auto values = platforms.value(*platform).toMap();
                values.insert(cell, row.value(cell));
                platforms.insert(*platform, values);
                continue;
            }
            merged.insert(cell, row.value(cell));
            for (auto it = platforms.begin(); it != platforms.end();) {
                auto values = it.value().toMap();
                values.remove(cell);
                if (values.isEmpty()) {
                    it = platforms.erase(it);
                } else {
                    it.value() = values;
                    ++it;
                }
            }
        }
        if (platforms.isEmpty()) merged.remove(platformsKey);
        else merged.insert(platformsKey, platforms);
        result.append(merged);
    }
    return result;
}

}

std::optional<FieldGroupId> PropertyEditorEngine::fieldGroup(const QString& field) const
{
    switch (itemType_) {
    case ProjectItem::Type::Sound:
    case ProjectItem::Type::Container:
    case ProjectItem::Type::Layer:
    case ProjectItem::Type::Track:
    case ProjectItem::Type::Segment:
        break;
    default:
        return {};
    }
    return FieldGroupForField(field.toStdString());
}

bool PropertyEditorEngine::supportsPlatformLocal(const QString& field) const
{
    if (const auto group = fieldGroup(field)) {
        const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
        return !property || (FieldGroupFieldMask(static_cast<harkenplayableproperties::PlayablePropertyIdV1>(property->id))
            & FieldGroupPlatformMask(*group)) != 0;
    }
    if (field == QLatin1String("filter")) return true;
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
    return property && !PlayablePropertyAuthoringV1::isComponentStored(property->id)
        && PlayablePropertyAuthoringV1::isEditorExposed(property->id)
        && PlayablePropertyAuthoringV1::isSlotApplicable(itemType_, property->id);
}

std::optional<PlatformLocalAddress> PropertyEditorEngine::platformLocalAddress(const QString& field) const
{
    using Kind = PlatformLocalAddress::Kind;
    if (!currentItem_ || !supportsPlatformLocal(field) || (conversionViews_.size() > 1 && fieldGroup(field) == FieldGroupId::Conversion)) return {};
    if (const auto group = fieldGroup(field)) {
        const auto found = groupBindings_.find(*group);
        if (found == groupBindings_.end() || !found->second.view.valid) return {};
        return PlatformLocalAddress{Kind::GroupField, field, *group, found->second.view.canInherit};
    }
    if (FilterAuthoring::isField(field)) return PlatformLocalAddress{Kind::Filter, field};
    return PlatformLocalAddress{Kind::Property,
        QString::fromLatin1(PlayablePropertyAuthoringV1::fieldByName(field)->fieldName)};
}

std::optional<PlatformLocalAddress> PropertyEditorEngine::routeCellAddress(
    const QString& field, const QUuid& row, const QString& cell) const
{
    using Kind = PlatformLocalAddress::Kind;
    if (!currentItem_ || cell == kRouteTarget) return {};
    if (currentItem_->type == ProjectItem::Type::Bus)
        return PlatformLocalAddress{Kind::RowCell, field, FieldGroupId::Send, false, row, cell};
    const auto group = fieldGroup(field);
    const auto found = group ? groupBindings_.find(*group) : groupBindings_.end();
    if (found == groupBindings_.end() || !found->second.view.valid) return {};
    return PlatformLocalAddress{Kind::RowCell, field, *group, found->second.view.canInherit, row, cell};
}

void PropertyEditorEngine::decorateRouteCells(QWidget* control, const PropertyDescriptor& descriptor)
{
    ListFieldDrawer::setWidgetCellDecorator(control,
        [self = QPointer<PropertyEditorEngine>(this), control, descriptor]
        (QWidget* cellControl, const QVariantMap& row, const QString& cell) -> QWidget* {
            const auto element = std::find_if(descriptor.elementSchema.begin(), descriptor.elementSchema.end(),
                [&cell](const PropertyDescriptor& field) { return field.field == cell; });
            const auto address = self && element != descriptor.elementSchema.end()
                ? self->routeCellAddress(descriptor.field, QUuid(row.value(QStringLiteral("id")).toString()), cell)
                : std::nullopt;
            if (!address || address->row.isNull()) return cellControl;
            auto* marker = new PlatformLocalMarker(cellControl->parentWidget());
            marker->setValueFormatter(PlatformLocalMarker::formatterFor(*element));
            marker->bind(self->currentItem_, address);
            for (const auto& binding : self->bindings_)
                if (binding.widget == control) marker->setEditable(self->canEditField(binding));
            return attachPlatformMarker(cellControl, marker, cellControl->parentWidget());
        });
}

void PropertyEditorEngine::refreshPlatformMarkers()
{
    for (const auto& binding : bindings_) {
        if (binding.marker) {
            binding.marker->bind(currentItem_, platformLocalAddress(binding.field));
            binding.marker->setEditable(canEditField(binding));
        } else if (binding.drawer && binding.widget) {
            for (auto* marker : binding.widget->findChildren<PlatformLocalMarker*>())
                marker->setEditable(canEditField(binding));
        }
    }
}

void PropertyEditorEngine::buildGroupHeader(FieldGroupId group, CollapsibleSection* section, bool showRangeName)
{
    auto* header = section->headerWidget();
    if (!header) {
        header = new QWidget(section);
        header->setObjectName(QStringLiteral("fieldGroupHeader_") + groupName(group));
        auto* layout = new QHBoxLayout(header);
        layout->setContentsMargins(0, 0, 0, 0);
        ThemeLayout::spacing(layout, ThemeLayout::Space::Section);
        section->setHeaderWidget(header);
    }
    QString label = tr("Inherit");
    if (showRangeName) {
        switch (group) {
        case FieldGroupId::Output: label = tr("Inherit Output"); break;
        case FieldGroupId::Send: label = tr("Inherit Sends"); break;
        case FieldGroupId::Sidechain: label = tr("Inherit Sidechains"); break;
        case FieldGroupId::Effect: label = tr("Inherit Effects"); break;
        case FieldGroupId::SpatialRating: label = tr("Inherit Spatial Rating"); break;
        case FieldGroupId::Conversion: label = tr("Inherit Conversion"); break;
        }
    }
    auto* inherit = new InheritCheckBox(label, header);
    inherit->setObjectName(QStringLiteral("inherit_") + groupName(group));
    inherit->setChecked(true);
    inherit->setEnabled(false);
    header->layout()->addWidget(inherit);
    auto& binding = groupBindings_[group];
    binding.inheritControls.push_back(inherit);
    QWidget* menuHost = showRangeName ? static_cast<QWidget*>(inherit) : section;
    menuHost->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(menuHost, &QWidget::customContextMenuRequested, this,
        [this, group, menuHost](const QPoint& position) {
            showGroupMenu(group, menuHost, menuHost->mapToGlobal(position));
        });
    connect(inherit, &QCheckBox::clicked, this, [this, group, inherit] {
        if (updating_) return;
        auto& binding = groupBindings_.at(group);
        if (!binding.view.canInherit || !groupContextMatches(binding.view, groupEditRevision_)) {
            refreshFieldGroups();
            return;
        }
        auto args = binding.view.actionArgs(true);
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
    const auto& manager = ProjectManager::instance();
    const auto* item = manager.findItemByGuid(view.definition);
    if (item != currentItem_ || item->fieldGroups != view.expectedBefore) return false;
    if (view.group == FieldGroupId::Conversion)
        for (const auto& selected : conversionViews_) {
            const auto* target = manager.findItemByGuid(selected.definition);
            if (!target || target->fieldGroups != selected.expectedBefore) return false;
        }
    return true;
}

void PropertyEditorEngine::setConversionSelection(const QList<ProjectItem*>& items)
{
    QVector<QUuid> selected;
    for (const auto* item : items)
        if (item && !selected.contains(item->guid)) selected.append(item->guid);
    if (!currentItem_ || !selected.contains(currentItem_->guid)) selected.clear();
    if (selected == conversionSelection_) return;
    conversionSelection_ = std::move(selected);
    refreshFieldGroups();
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

QVariantList PropertyEditorEngine::mergedRouteRows(const QString& field, const QVariant& edited) const
{
    QVariantList raw;
    QVariantList displayed;
    if (currentItem_->type == ProjectItem::Type::Bus) {
        raw = currentItem_->comp<VMixNode>()->getField(field).toList();
        displayed = readField(field).toList();
    } else {
        raw = convertRouteTargets(groupBindings_.at(*fieldGroup(field)).view.field(field).shared.toList(),
                                  editorTarget).value_or(QVariantList{});
        displayed = groupFieldValue(field).toList();
    }
    const auto& context = PlatformContextService::instance().context();
    const auto platform = context.all || context.selected.isEmpty()
        ? std::optional<QString>{} : std::optional<QString>(context.primary.toString(QUuid::WithoutBraces));
    return mergeRouteRows(raw, displayed, edited.toList(), routeCells(field), platform);
}

void PropertyEditorEngine::previewGroupRoutes(FieldBinding& binding, const QVariant& value)
{
    if (updating_ || !canEditField(binding)) return;
    auto& manager = ProjectManager::instance();
    const auto group = fieldGroup(binding.field);
    const auto rows = convertRouteTargets(mergedRouteRows(binding.field, value), actionTarget);
    if (!manager.currentProject() || !group || !rows) return;
    const auto json = QJsonArray::fromVariantList(*rows);
    FieldGroupEditOperation operation;
    if (*group == FieldGroupId::Send) {
        SendFieldGroup sends;
        if (!fieldGroupRouteRowsFromJson(json, sends.sends)) return;
        operation = ReplaceFieldGroupSends{std::move(sends)};
    } else {
        SidechainFieldGroup sidechains;
        if (!fieldGroupRouteRowsFromJson(json, sidechains.sidechains)) return;
        operation = ReplaceFieldGroupSidechains{std::move(sidechains)};
    }
    FieldGroupEditScopes shared;
    shared.shared = true;
    const auto prepared = prepareFieldGroupEdit(*manager.currentProject(), {
        currentItem_->guid, *group, shared, std::move(operation), groupBindings_.at(*group).view.expectedBefore});
    if (!prepared.ok()) return;
    binding.groupPreviewActive = true;
    emit manager.fieldGroupsPreviewed(currentItem_, *prepared.after);
}

void PropertyEditorEngine::previewBusRoutes(FieldBinding& binding, const QVariant& value)
{
    if (updating_ || !canEditField(binding) || currentItem_->type != ProjectItem::Type::Bus) return;
    if (!binding.editSession)
        binding.editSession = std::make_unique<PropertyEditSession>(
            currentItem_, binding.field, currentItem_->comp<VMixNode>()->getField(binding.field), this);
    binding.editSession->preview(mergedRouteRows(binding.field, value));
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
    conversionViews_.clear();
    for (auto& [group, binding] : groupBindings_) {
        binding.view = project && currentItem_
            ? buildFieldGroupInspectorView(*project, currentItem_->guid, group, context)
            : FieldGroupInspectorView{};
        if (group == FieldGroupId::Conversion && project && currentItem_
            && conversionSelection_.size() > 1) {
            auto& aggregate = binding.view;
            for (const auto& guid : conversionSelection_) {
                auto selected = buildFieldGroupInspectorView(*project, guid, group, context);
                aggregate.valid &= selected.valid;
                aggregate.canInherit &= selected.canInherit;
                aggregate.fieldsEditable &= selected.fieldsEditable;
                aggregate.inheritanceMixed |= selected.inheritanceMixed
                    || selected.effectiveInherit != aggregate.effectiveInherit;
                aggregate.diagnostics.append(selected.diagnostics);
                conversionViews_.push_back(std::move(selected));
            }
        }
        const auto& view = binding.view;
        QString tooltip = project ? view.sourceTooltip(*project) : QString();
        if (!view.diagnostics.isEmpty()) tooltip = view.diagnostics.join(QLatin1Char('\n'));
        if (!binding.error.isEmpty()) tooltip.prepend(binding.error + QLatin1Char('\n'));
        for (const auto& inherit : binding.inheritControls) {
            if (!inherit) continue;
            QSignalBlocker blocker(inherit);
            inherit->setVisible(group != FieldGroupId::Conversion || view.canInherit);
            inherit->setEnabled(view.valid && view.canInherit);
            inherit->setTristate(view.inheritanceMixed);
            inherit->setCheckState(view.inheritanceMixed ? Qt::PartiallyChecked
                : view.effectiveInherit ? Qt::Checked : Qt::Unchecked);
            inherit->setToolTip(tooltip);
        }
    }
    for (auto& binding : bindings_)
        if (fieldGroup(binding.field)) applyGroupFieldView(binding);
    refreshPlatformMarkers();
    for (const auto& section : sectionBindings_)
        if (section.provider && section.widget)
            section.provider->refresh(section.widget, currentItem_, QStringLiteral("fieldGroups"));
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
        if (group != FieldGroupId::Output && group != FieldGroupId::Send && group != FieldGroupId::Sidechain) continue;
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
        if ((binding.field != QLatin1String("outputBusVolume") && binding.field != QLatin1String("sends")
             && binding.field != QLatin1String("sidechains"))
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
            entry.resolved.payload = *group == FieldGroupId::Output ? FieldGroupPayload(effective->second.output)
                : *group == FieldGroupId::Send ? FieldGroupPayload(effective->second.send)
                : FieldGroupPayload(effective->second.sidechain);
        }
        const auto value = groupFieldValue(binding.field, &view);
        if (binding.drawer) binding.drawer->bind(binding.widget, value, currentItem_);
        else if (value.isValid()) setGroupFieldWidgetValue(binding, value);
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
    if (*group == FieldGroupId::Conversion && conversionViews_.size() > 1)
        for (const auto& selected : conversionViews_) {
            const auto other = selected.field(field);
            if (!selected.valid || !other.valid || other.value != value.value) return {};
        }
    if (!view.valid || !value.valid) return {};
    if (value.value.metaType().id() == QMetaType::QUuid)
        return referenceEditorValue(value.value);
    if (*group != FieldGroupId::Send && *group != FieldGroupId::Sidechain) return value.value;
    return *convertRouteTargets(value.value.toList(), editorTarget);
}

bool PropertyEditorEngine::groupReferenceSelectable(const QString& field, uint32_t candidate, SendType type, bool duckTarget) const
{
    const auto group = fieldGroup(field);
    const auto* target = ProjectManager::instance().findItemByObjectID(candidate);
    if (!group || !target) return false;
    if (*group == FieldGroupId::SpatialRating)
        return target->type == ProjectItem::Type::Attenuation;
    if (*group == FieldGroupId::Conversion)
        return target->type == ProjectItem::Type::Conversion;
    if (target->type != ProjectItem::Type::Bus) return false;
    const auto peerField = [this](FieldGroupId peer, const QString& name) -> std::optional<FieldGroupInspectorFieldView> {
        const auto found = groupBindings_.find(peer);
        if (found == groupBindings_.end()) return FieldGroupInspectorFieldView{true, {}, {}, {}};
        if (!found->second.view.valid) return {};
        return found->second.view.field(name);
    };
    if (*group != FieldGroupId::Output) {
        if (type == SendType::Sidechain && !duckTarget) return true;
        const auto output = peerField(FieldGroupId::Output, QStringLiteral("outputBusRef"));
        if (!output) return false;
        return std::none_of(output->perScope.begin(), output->perScope.end(),
            [&](const auto& scope) { return scope.second.toUuid() == target->guid; });
    }
    for (const auto& [peer, name] : {std::pair{FieldGroupId::Send, QStringLiteral("sends")},
                                     std::pair{FieldGroupId::Sidechain, QStringLiteral("sidechains")}}) {
        const auto rows = peerField(peer, name);
        if (!rows) return false;
        for (const auto& scope : rows->perScope)
            for (const auto& entry : scope.second.toList()) {
                const auto row = entry.toMap();
                if ((peer == FieldGroupId::Send || row.value(QStringLiteral("duckTarget")).toBool())
                    && row.value(QStringLiteral("targetBusID")).toUuid() == target->guid)
                    return false;
            }
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
        if (*group == FieldGroupId::Send || *group == FieldGroupId::Sidechain) {
            const auto self = QPointer<PropertyEditorEngine>(this);
            ListFieldDrawer::setWidgetPreviewCallbacks(binding.widget,
                [self, widget = binding.widget](const QVariant& preview) {
                    if (!self) return;
                    for (auto& entry : self->bindings_)
                        if (entry.widget == widget) { self->previewGroupRoutes(entry, preview); return; }
                },
                [self, widget = binding.widget] {
                    if (!self) return;
                    for (auto& entry : self->bindings_)
                        if (entry.widget == widget) { self->finishGroupPreview(entry); return; }
                });
        }
        binding.drawer->bind(binding.widget, value, currentItem_);
    } else if (value.isValid()) {
        setGroupFieldWidgetValue(binding, value);
    }
    if (const auto* project = ProjectManager::instance().currentProject()) {
        QString tooltip = view.sourceTooltip(*project, binding.field);
        if (!value.isValid() && conversionViews_.size() > 1)
            tooltip.prepend(tr("Mixed values across selected objects.\n"));
        binding.groupTooltip = tooltip;
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
            || (binding.type != PropType::ObjectRef
                && (binding.type != PropType::List || itemType_ == ProjectItem::Type::Bus));
    }
    const auto found = groupBindings_.find(*group);
    if (found == groupBindings_.end()) return false;
    const auto& view = found->second.view;
    return groupContextMatches(view, groupEditRevision_) && view.fieldsEditable;
}

void PropertyEditorEngine::commitFieldChange(
    const QString& field, const QVariant& value, const std::optional<QJsonObject>& groupArgs, QWidget* source)
{
    if (updating_ || !currentItem_) return;
    const auto found = std::find_if(bindings_.begin(), bindings_.end(),
        [&](const FieldBinding& binding) { return binding.field == field && (!source || binding.widget == source); });
    if (found == bindings_.end() || !canEditField(*found)) return;
    const auto group = fieldGroup(field);
    if (!group) {
        if (currentItem_->type == ProjectItem::Type::Bus
            && (field == QLatin1String("sends") || field == QLatin1String("sidechains"))) {
            const auto rows = mergedRouteRows(field, value);
            auto session = found->editSession ? std::move(found->editSession)
                : std::make_unique<PropertyEditSession>(
                      currentItem_, field, currentItem_->comp<VMixNode>()->getField(field), this);
            session->preview(rows);
            session->commit();
            return;
        }
        emit fieldChanged(field, readField(field), value);
        return;
    }
    if (*group == FieldGroupId::Send || *group == FieldGroupId::Sidechain) {
        const auto rows = convertRouteTargets(mergedRouteRows(field, value), actionTarget);
        if (!rows) return;
        auto args = groupBindings_.at(*group).view.actionArgs(true);
        args.insert(field, QJsonArray::fromVariantList(*rows));
        executeGroupAction(*group, *group == FieldGroupId::Send
            ? QStringLiteral("replace_field_group_sends") : QStringLiteral("replace_field_group_sidechains"), std::move(args));
        return;
    }
    auto args = groupArgs.value_or(groupBindings_.at(*group).view.actionArgs(!supportsPlatformLocal(field)));
    if (*group == FieldGroupId::Conversion) {
        const auto ref = referenceActionValue(value);
        if (!ref) return;
        args.insert(QStringLiteral("field"), QStringLiteral("settingsRef"));
        args.insert(QStringLiteral("value"), *ref);
        executeGroupAction(*group, QStringLiteral("set_field_group_field"), std::move(args));
        return;
    }
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
    if (!property) return;
    args.insert(QStringLiteral("field"), QString::fromLatin1(property->jsonKey));
    if (property->referenceRole == harkenplayableproperties::PlayablePropertyReferenceRoleV1::OutputBus
        || property->referenceRole == harkenplayableproperties::PlayablePropertyReferenceRoleV1::Attenuation) {
        const auto ref = referenceActionValue(value);
        if (!ref) return;
        args.insert(QStringLiteral("value"), *ref);
    } else {
        args.insert(QStringLiteral("value"), QJsonValue::fromVariant(value));
    }
    executeGroupAction(*group, QStringLiteral("set_field_group_field"), std::move(args));
}

void PropertyEditorEngine::executeGroupAction(FieldGroupId group, const QString& action, QJsonObject args)
{
    if (group == FieldGroupId::Conversion && conversionViews_.size() > 1) {
        const auto& view = groupBindings_.at(group).view;
        if (!groupContextMatches(view, groupEditRevision_)) return;
        QVector<QUuid> definitions;
        QVector<FieldGroupDefinitionSnapshot> expected;
        for (const auto& selected : conversionViews_) {
            definitions.append(selected.definition);
            expected.append({selected.definition, selected.expectedBefore});
        }
        const auto inherit = action == QLatin1String("set_field_group_mode")
            ? std::optional<bool>(args.value(QStringLiteral("mode")) == QJsonValue(QStringLiteral("Inherit")))
            : std::nullopt;
        const auto settings = action == QLatin1String("set_field_group_field")
            ? std::optional<QUuid>(QUuid(args.value(QStringLiteral("value")).toString())) : std::nullopt;
        const auto platform = inherit || view.editScopes.shared ? PlatformScope{}
            : PlatformScope{fromFieldGroupGuid(view.editScopes.platforms.front())};
        cancelGroupEdits();
        QString error;
        const bool changed = ConversionEditing::setObjects(ProjectManager::instance(),
            definitions, inherit, settings, platform, expected, this, &error);
        groupBindings_.at(group).error = error;
        refreshFieldGroups();
        if (changed) emit fieldGroupChanged();
        return;
    }
    cancelGroupEdits(action == QStringLiteral("set_field_group_field")
        || action == QStringLiteral("replace_field_group_sends")
        || action == QStringLiteral("replace_field_group_sidechains"));
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

void PropertyEditorEngine::showGroupMenu(FieldGroupId group, QWidget* parent, const QPoint& globalPosition)
{
    if (group == FieldGroupId::Conversion && conversionViews_.size() > 1) return;
    refreshFieldGroups();
    auto& binding = groupBindings_.at(group);
    if (!binding.view.valid) return;
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("fieldGroupMenu_") + groupName(group));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto* rebuild = menu->addAction(tr("Rebuild Local from Current Effective Values"));
    rebuild->setObjectName(QStringLiteral("rebuildLocal"));
    rebuild->setEnabled(binding.view.canInherit);
    connect(rebuild, &QAction::triggered, this,
        [this, group, view = binding.view, revision = groupEditRevision_] {
            if (!groupContextMatches(view, revision) || !view.canInherit) return;
            executeGroupAction(group, QStringLiteral("rebuild_field_group_local"), view.actionArgs(true));
        });
    menu->popup(globalPosition);
}

}
