#pragma once

#include "Core/BindingSemantics.h"
#include "Core/VariableSemantics.h"

#include <QObject>

class QMenu;
class QWidget;

namespace VNX {

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
    void bindingAdded(harkenstudio::ObjectId id);

private:
    void openVariableSelector();
    void addBinding(uint32_t objectId);
    void createVariable(VariableKind kind);

    QWidget* anchor_ = nullptr;
    QMenu* menu_ = nullptr;
    QUuid ownerGuid_;
    BindingAddress address_;
    uint64_t generation_ = 0;
};

}
