#pragma once

#include "Core/BindingSemantics.h"
#include "Core/VariableSemantics.h"

#include <QObject>

class QMenu;
#include "Core/UndoHistory.h"
class QWidget;

namespace harken::studio {

struct ProjectItem;

class BindingAddMenu final : public QObject
{
    Q_OBJECT

public:
    explicit BindingAddMenu(QWidget* anchor);
    ~BindingAddMenu() override;

    void bind(ProjectItem* item, const BindingAddress& address);
    void show();

signals:
    void bindingAdded(harken::studio::types::ObjectId id);

private:
    void openVariableSelector();
    QString addBinding(uint32_t objectId, harken::studio::UndoCommand* undoParent = nullptr);
    void openVariableCreation(VariableKind kind);

    QWidget* anchor_ = nullptr;
    QMenu* menu_ = nullptr;
    QUuid ownerGuid_;
    BindingAddress address_;
    uint64_t generation_ = 0;
};

}
