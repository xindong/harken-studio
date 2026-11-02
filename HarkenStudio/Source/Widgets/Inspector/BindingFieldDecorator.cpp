#include "BindingFieldDecorator.h"

#include "BindingAddMenu.h"
#include "Core/BindingContract.h"
#include "Core/BindingEditing.h"
#include "Core/Components/VMixNode.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"

namespace VNX {

namespace {

QString variableName(const QUuid& guid)
{
    const ProjectItem* item = ProjectManager::instance().findItemByGuid(guid);
    return item ? item->name : QObject::tr("Missing Variable");
}

}

BindingFieldDecorator::BindingFieldDecorator(uint8_t slotId, QWidget* parent)
    : BindingFieldDecorator(BindingContractSemantics::addressOf(slotId, ProjectItemType::Sound), parent)
{
}

BindingFieldDecorator::BindingFieldDecorator(const BindingAddress& address, QWidget* parent)
    : InspectorSlotButton(InspectorSlotKind::Binding, parent)
    , address_(address)
{
    setObjectName(QStringLiteral("bindingPoint_")
        + BindingSemantics::slotName(address));
    setContextMenuPolicy(Qt::PreventContextMenu);
    addMenu_ = new BindingAddMenu(this);
    connect(addMenu_, &BindingAddMenu::bindingAdded, this, [this](harkenstudio::ObjectId id) {
        if (!item_) return;
        QMetaObject::invokeMethod(this, [this, guid = item_->guid, address = address_, id] {
            if (item_ && item_->guid == guid && address_ == address)
                showEditor(id);
        }, Qt::QueuedConnection);
    });
    connect(this, &QToolButton::clicked, this,
            [this] { activate(); });
    connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged, this,
            [this](ProjectItem* item, const QString& change, const QObject*) {
        if (!item || item != item_) return;
        if (change != QLatin1String("bindings")
            && change != BindingEditing::changeKey(address_))
            return;
        refreshPresentation();
    });
    connect(&ProjectManager::instance(), &ProjectManager::itemRenamed, this,
            [this](ProjectItem* renamed, const QString&) {
        if (!item_ || !renamed) return;
        const auto* mix = BindingEditing::ledgerHost(item_, address_);
        if (!mix) return;
        for (const auto* binding : mix->bindings.findAll(address_)) {
            if (binding->variable == renamed->guid) {
                refreshPresentation();
                return;
            }
        }
    });
    connect(&ProjectManager::instance(), &ProjectManager::itemDeleted, this,
            [this](ProjectItem* deleted) {
        if (!deleted) return;
        if (deleted == item_) {
            item_ = nullptr;
            refreshPresentation();
            return;
        }
        if (!item_) return;
        const auto* mix = BindingEditing::ledgerHost(item_, address_);
        if (!mix) return;
        for (const auto* binding : mix->bindings.findAll(address_)) {
            if (binding->variable == deleted->guid) {
                refreshPresentation();
                return;
            }
        }
    });
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this,
            [this] { bind(nullptr); });
    refreshPresentation();
}

BindingFieldDecorator::~BindingFieldDecorator() = default;

BindingAddress BindingFieldDecorator::addressFor(const ProjectItem* item) const
{
    BindingAddress address = address_;
    address.hostClass = BindingContractSemantics::hostClassOf(
        item ? item->type : ProjectItemType::Sound);
    return address;
}

void BindingFieldDecorator::bind(ProjectItem* item)
{
    item_ = item;
    address_ = addressFor(item);
    refreshPresentation();
}

void BindingFieldDecorator::activate()
{
    if (isReadOnly()) return;
    if (isBound())
        showEditor();
    else
        addMenu_->show();
}

void BindingFieldDecorator::showEditor(harkenstudio::ObjectId bindingId)
{
    if (isReadOnly()) return;
    if (bindingId != harkenstudio::ObjectId{}) {
        const auto* mix = BindingEditing::ledgerHost(item_, address_);
        const auto* binding = mix ? mix->bindings.find(bindingId) : nullptr;
        if (!binding || binding->address != address_) return;
    }
    emit externalEditorRequested(ExternalEditorSelection::fromCurve(
        CurveSelection::binding(item_->guid, address_, bindingId)));
}

QString BindingFieldDecorator::hoverSummary() const
{
    return toolTip();
}

bool BindingFieldDecorator::isReadOnly() const
{
    return !item_ || !isEnabled();
}

void BindingFieldDecorator::refreshPresentation()
{
    const BindingAddress address = addressFor(item_);
    address_ = address;
    addMenu_->bind(item_, address);
    setEnabled(item_ && BindingContractSemantics::isBindable(
        BindingContractSemantics::derive(address)));
    QStringList summary;
    if (const auto* mix = BindingEditing::ledgerHost(item_, address))
        for (const auto* binding : mix->bindings.findAll(address))
            summary.append(variableName(binding->variable));
    setToolTip(summary.isEmpty() ? tr("Variable\nUnbound")
                               : tr("Variable\n%1").arg(summary.join(QLatin1Char('\n'))));
    setBound(!summary.isEmpty());
    QtControlSkin::setSemanticProperty(
        this, "bindingState",
        isBound() ? QStringLiteral("own") : QStringLiteral("none"));
}

void registerBindingFieldDecorator()
{
    InspectorCustomizationRegistry::instance().setBindingDecoratorFactory(
        [](const PropertyDescriptor& descriptor) -> QWidget* {
            const auto address = BindingContractSemantics::addressFromDescriptor(
                descriptor, ProjectItemType::Sound);
            if (!address) return nullptr;
            return new BindingFieldDecorator(*address);
        });
}

}
