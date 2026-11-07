
#pragma once

#include "InspectorCustomizationRegistry.h"
#include "InspectorSlotButton.h"
#include "Core/BindingSemantics.h"
#include "Core/ExternalEditorSelection.h"

namespace VNX {

class BindingAddMenu;

class BindingFieldDecorator : public InspectorSlotButton, public IFieldDecorator
{
    Q_OBJECT

public:
    explicit BindingFieldDecorator(uint8_t slotId, QWidget* parent = nullptr);
    explicit BindingFieldDecorator(const BindingAddress& address, QWidget* parent = nullptr);
    ~BindingFieldDecorator() override;

    InspectorSlotKind slotKind() const override { return InspectorSlotKind::Binding; }
    void bind(ProjectItem* item) override;
    QWidget* detailWidget() const override { return nullptr; }
    QString hoverSummary() const override;
    bool isReadOnly() const override;

signals:
    void externalEditorRequested(const ExternalEditorSelection& selection);

private:
    void activate();
    void refreshPresentation();
    BindingAddress addressFor(const ProjectItem* item) const;
    void showEditor(harkenstudio::ObjectId bindingId = {});

    BindingAddress address_;
    ProjectItem* item_ = nullptr;
    BindingAddMenu* addMenu_ = nullptr;
};

void registerBindingFieldDecorator();

}
