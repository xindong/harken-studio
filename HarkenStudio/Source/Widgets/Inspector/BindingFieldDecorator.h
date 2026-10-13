
#pragma once

#include "InspectorCustomizationRegistry.h"
#include "InspectorSlotButton.h"
#include "Core/BindingSemantics.h"

namespace VNX {

class BindingAddMenu;

class BindingFieldDecorator : public InspectorSlotButton, public IFieldDecorator
{
    Q_OBJECT

public:
    explicit BindingFieldDecorator(uint8_t slotId, QWidget* parent = nullptr);
    ~BindingFieldDecorator() override;

    InspectorSlotKind slotKind() const override { return InspectorSlotKind::Binding; }
    void bind(ProjectItem* item) override;
    QWidget* detailWidget() const override { return nullptr; }
    QString hoverSummary() const override;
    bool isReadOnly() const override;

private:
    void activate();
    void refreshPresentation();
    BindingAddress addressFor(const ProjectItem* item) const;
    void showEditor(harkenstudio::ObjectId bindingId = {});

    uint8_t slotId_ = 0;
    BindingAddress address_;
    ProjectItem* item_ = nullptr;
    BindingAddMenu* addMenu_ = nullptr;
};

void registerBindingFieldDecorator();

}
