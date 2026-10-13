#include "AttenuationFieldDecorator.h"

#include "FieldGroupInspectorView.h"
#include "InspectorPanel.h"
#include "Core/ActionManager.h"
#include "Core/AttenuationStore.h"
#include "Core/BindingContract.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"

#include <QSet>
#include <QMenu>

namespace VNX {

AttenuationFieldDecorator::AttenuationFieldDecorator(
    AttenuationDimension dimension, QWidget* parent)
    : InspectorSlotButton(InspectorSlotKind::Attenuation, parent)
    , dimension_(dimension)
{
    setObjectName(QStringLiteral("attenuationPoint_")
                  + QLatin1String(attenuationDimensionName(dimension)));
    setEnabled(false);
    connect(this, &QToolButton::clicked, this, &AttenuationFieldDecorator::activate);
    auto& project = ProjectManager::instance();
    connect(&project, &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& property, const QObject*) {
        if (item && (item->type == ProjectItem::Type::Attenuation
                     || property == QLatin1String("fieldGroups")))
            refreshPresentation();
    });
    connect(&project, &ProjectManager::itemRenamed, this,
            [this](ProjectItem* item, const QString&) {
        if (item && item->type == ProjectItem::Type::Attenuation)
            refreshPresentation();
    });
    connect(&project, &ProjectManager::itemMoved, this,
            [this] { refreshPresentation(); });
    connect(&project, &ProjectManager::itemDeleted, this,
            [this](ProjectItem* item) {
        if (item == item_) bind(nullptr);
        else if (item && item->guid == attenuationGuid_)
            QMetaObject::invokeMethod(this, [this] { refreshPresentation(); }, Qt::QueuedConnection);
    });
    connect(&project, &ProjectManager::projectClosed, this,
            [this] { bind(nullptr); });
    connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged,
            this, [this] { refreshPresentation(); });
    refreshPresentation();
}

void AttenuationFieldDecorator::bind(ProjectItem* item)
{
    item_ = item;
    refreshPresentation();
}

void AttenuationFieldDecorator::refreshPresentation()
{
    QStringList names;
    bool mixed = false;
    attenuationGuid_ = QUuid{};
    setEnabled(false);
    const auto* project = ProjectManager::instance().currentProject();
    if (item_ && project) {
        const auto view = buildFieldGroupInspectorView(
            *project, item_->guid, FieldGroupId::SpatialRating,
            PlatformContextService::instance().context());
        const auto references = view.field(QStringLiteral("spatialAttenuationRef"));
        QSet<QUuid> bindings;
        for (const auto& entry : references.perScope) {
            QUuid binding;
            const auto* attenuation = AttenuationStore::findByGuid(project, entry.second.toUuid());
            if (attenuation) {
                binding = attenuation->hostItem()->guid;
                if (attenuation->dimension(dimension_)) {
                    names.append(attenuation->hostItem()->name);
                }
            }
            bindings.insert(binding);
        }
        mixed = bindings.size() > 1;
        if (!mixed && bindings.size() == 1)
            attenuationGuid_ = *bindings.begin();
        if (!view.valid) {
            setBound(false);
            setToolTip(tr("Attenuation\nUnavailable"));
            return;
        }
    }
    names.removeDuplicates();
    setEnabled(!attenuationGuid_.isNull());
    setBound(!names.isEmpty());
    QString summary = names.isEmpty() ? tr("Unbound") : names.join(QLatin1Char('\n'));
    if (mixed) summary.prepend(tr("Mixed bindings\n"));
    setToolTip(tr("Attenuation\n%1").arg(summary));
}

void AttenuationFieldDecorator::activate()
{
    refreshPresentation();
    if (isReadOnly()) return;
    const auto* definition = AttenuationStore::findByGuid(
        ProjectManager::instance().currentProject(), attenuationGuid_);
    if (!definition) return;
    if (definition->dimension(dimension_)) {
        openEditor();
        return;
    }
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("attenuationSlotAddMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QUuid target = attenuationGuid_;
    auto* add = menu->addAction(dimension_ == AttenuationDimension::Volume
        ? tr("Add Volume") : tr("Add Pitch"));
    connect(add, &QAction::triggered, this, [this, target] {
        refreshPresentation();
        if (isReadOnly() || attenuationGuid_ != target) return;
        ActionManager::instance().execute(QStringLiteral("add_attenuation_dimension"),
            {{QStringLiteral("attenuation_guid"), target.toString(QUuid::WithoutBraces)},
             {QStringLiteral("dimension"), QLatin1String(attenuationDimensionName(dimension_))}},
            ActionSource::GUI, this);
        refreshPresentation();
    });
    menu->popup(mapToGlobal(QPoint(0, height())));
}

void AttenuationFieldDecorator::openEditor()
{
    const auto* definition = AttenuationStore::findByGuid(
        ProjectManager::instance().currentProject(), attenuationGuid_);
    if (!definition || !item_) return;
    for (QWidget* current = this; current; current = current->parentWidget()) {
        auto* inspector = qobject_cast<InspectorPanel*>(current);
        if (!inspector) continue;
        inspector->requestExternalEditor(ExternalEditorSelection::fromCurve(
            CurveSelection::attenuation(definition->hostItem()->guid, definition->hostItem()->name, dimension_), item_->guid));
        return;
    }
}

void registerAttenuationFieldDecorator()
{
    InspectorCustomizationRegistry::instance().registerSlotDecorator(
        InspectorSlotKind::Attenuation,
        [](const PropertyDescriptor& descriptor) -> QWidget* {
            const auto slotId = BindingContractSemantics::slotIdFromDescriptor(descriptor);
            using Property = harkenplayableproperties::PlayablePropertyIdV1;
            if (slotId == static_cast<uint8_t>(Property::Volume))
                return new AttenuationFieldDecorator(AttenuationDimension::Volume);
            if (slotId == static_cast<uint8_t>(Property::Pitch))
                return new AttenuationFieldDecorator(AttenuationDimension::Pitch);
            return nullptr;
        });
}

}
