#include "BindingAddMenu.h"

#include "Core/ActionManager.h"
#include "Core/BindingEditing.h"
#include "Core/Components/VVariable.h"
#include "Core/ProjectManager.h"
#include "Core/UndoCommands.h"
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
                openVariableCreation(kind);
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

QString BindingAddMenu::addBinding(uint32_t objectId, QUndoCommand* undoParent)
{
    auto& manager = ProjectManager::instance();
    auto* item = manager.findItemByGuid(ownerGuid_);
    const auto* variable = manager.findItemByObjectID(objectId);
    if (!item || !anchor_->isEnabled() || !variable || !variable->comp<VVariable>())
        return tr("The binding target is no longer available.");
    auto binding = BindingSemantics::defaultBinding(address_, variable->guid);
    binding.guid = QUuid::createUuid();
    QString label = BindingSemantics::slotName(address_.slotId);
    if (!label.isEmpty())
        label[0] = label[0].toUpper();
    const auto generation = generation_;
    QPointer<BindingAddMenu> self(this);
    const auto result = ActionManager::instance().execute(QStringLiteral("set_binding"),
        BindingEditing::upsertArgs(item, binding, tr("Add %1 Binding").arg(label)),
        ActionSource::GUI, anchor_, undoParent);
    if (self && generation == self->generation_ && result.success)
        emit self->bindingAdded(binding.runtimeId());
    return result.success ? QString{} : result.errorMessage;
}

void BindingAddMenu::openVariableCreation(VariableKind kind)
{
    auto& manager = ProjectManager::instance();
    if (!manager.findItemByGuid(ownerGuid_) || !anchor_->isEnabled()) return;
    const auto generation = generation_;
    QPointer<BindingAddMenu> self(this);
    SelectorRequest request;
    request.title = tr("Create Variable");
    request.creation = SelectorCreationRequest{
        ProjectItem::Type::Variable,
        manager.suggestUniqueName(ProjectItem::Type::Variable, QStringLiteral("Variable")),
        [self, generation, kind](const QString& folderPath, const QString& name) -> QString {
            if (!self || generation != self->generation_
                || !self->anchor_->isEnabled()
                || !ProjectManager::instance().findItemByGuid(self->ownerGuid_))
                return tr("The binding target is no longer available.");
            auto command = std::make_unique<QUndoCommand>(tr("Create Variable and Bind"));
            auto* created = ProjectManager::instance().createItem(
                nullptr, name, ProjectItem::Type::Variable, folderPath, {}, command.get(), kind);
            if (!created) return tr("Failed to create the Variable.");
            const QString error = self && generation == self->generation_
                ? self->addBinding(created->objectID, command.get())
                : tr("The binding target is no longer available.");
            if (!error.isEmpty()) {
                command->undo();
                return error;
            }
            UndoRedoManager::instance().push(command.release());
            return {};
        }};
    SelectorWindow::open(request, anchor_);
}

}
