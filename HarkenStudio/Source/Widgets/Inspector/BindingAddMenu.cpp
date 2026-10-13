#include "BindingAddMenu.h"

#include "Core/ActionManager.h"
#include "Core/BindingEditing.h"
#include "Core/Components/VVariable.h"
#include "Core/ProjectManager.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/ProjectItemMenuController.h"

#include <QMenu>
#include <QPointer>

namespace VNX {

BindingAddMenu::BindingAddMenu(QWidget* anchor)
    : QObject(anchor)
    , anchor_(anchor)
    , menu_(new QMenu(anchor))
{
    menu_->setObjectName(QStringLiteral("bindingAddMenu"));
    auto* select = menu_->addAction(tr("Select Variable"));
    select->setObjectName(QStringLiteral("bindingSelectVariable"));
    connect(select, &QAction::triggered, this, [this] {
        QMetaObject::invokeMethod(this, [this, generation = generation_] {
            if (generation == generation_)
                openVariableSelector();
        }, Qt::QueuedConnection);
    });
    auto* create = menu_->addMenu(tr("New Variable"));
    create->setObjectName(QStringLiteral("bindingNewVariable"));
    ProjectItemMenuController::addVariableKindActions(create, [this](VariableKind kind) {
        QMetaObject::invokeMethod(this, [this, kind, generation = generation_] {
            if (generation == generation_)
                createVariable(kind);
        }, Qt::QueuedConnection);
    });
}

BindingAddMenu::~BindingAddMenu()
{
    SelectorWindow::dismiss(anchor_);
}

void BindingAddMenu::bind(ProjectItem* item, const BindingAddress& address)
{
    const QUuid guid = item ? item->guid : QUuid{};
    if (guid != ownerGuid_ || address != address_) {
        ++generation_;
        menu_->close();
        SelectorWindow::dismiss(anchor_);
    }
    ownerGuid_ = guid;
    address_ = address;
}

void BindingAddMenu::show()
{
    if (ownerGuid_.isNull() || !anchor_->isEnabled()) return;
    menu_->popup(anchor_->mapToGlobal(QPoint(0, anchor_->height())));
}

void BindingAddMenu::openVariableSelector()
{
    if (!ProjectManager::instance().findItemByGuid(ownerGuid_) || !anchor_->isEnabled()) return;
    SelectorRequest request;
    request.title = tr("Variable");
    request.filterType = ProjectItem::Type::Variable;
    QPointer<BindingAddMenu> self(this);
    request.onSelected = [self, guid = ownerGuid_, address = address_](uint32_t id) {
        if (self && self->ownerGuid_ == guid && self->address_ == address)
            self->addBinding(id);
    };
    SelectorWindow::open(request, anchor_);
}

void BindingAddMenu::addBinding(uint32_t objectId)
{
    auto& manager = ProjectManager::instance();
    auto* item = manager.findItemByGuid(ownerGuid_);
    const auto* variable = manager.findItemByObjectID(objectId);
    if (!item || !anchor_->isEnabled() || !variable || !variable->comp<VVariable>()) return;
    auto binding = BindingSemantics::defaultBinding(address_, variable->guid);
    binding.guid = QUuid::createUuid();
    QString label = BindingSemantics::slotName(address_.slotId);
    if (!label.isEmpty())
        label[0] = label[0].toUpper();
    const auto generation = generation_;
    QPointer<BindingAddMenu> self(this);
    const auto result = ActionManager::instance().execute(QStringLiteral("set_binding"),
        BindingEditing::upsertArgs(item, binding, tr("Add %1 Binding").arg(label)),
        ActionSource::GUI, anchor_);
    if (self && generation == self->generation_ && result.success)
        emit self->bindingAdded(binding.runtimeId());
}

void BindingAddMenu::createVariable(VariableKind kind)
{
    auto& manager = ProjectManager::instance();
    if (!manager.findItemByGuid(ownerGuid_) || !anchor_->isEnabled()) return;
    const QString name = manager.suggestUniqueName(ProjectItem::Type::Variable, QStringLiteral("Variable"));
    const auto generation = generation_;
    QPointer<BindingAddMenu> self(this);
    auto* created = manager.createItem(nullptr, name, ProjectItem::Type::Variable, {}, {}, nullptr, kind);
    if (self && generation == self->generation_ && created)
        self->addBinding(created->objectID);
}

}
