#pragma once

#include "InspectorCustomizationRegistry.h"
#include "InspectorSlotButton.h"
#include "Core/ProjectTypes.h"

namespace VNX {

class AttenuationFieldDecorator : public InspectorSlotButton, public IFieldDecorator
{
    Q_OBJECT

public:
    explicit AttenuationFieldDecorator(AttenuationDimension dimension,
                                      QWidget* parent = nullptr);

    InspectorSlotKind slotKind() const override { return InspectorSlotKind::Attenuation; }
    void bind(ProjectItem* item) override;
    QString hoverSummary() const override { return toolTip(); }
    bool isReadOnly() const override { return !isEnabled(); }

private:
    void refreshPresentation();
    void activate();
    void openEditor();

    AttenuationDimension dimension_;
    ProjectItem* item_ = nullptr;
    QUuid attenuationGuid_;
};

void registerAttenuationFieldDecorator();

}
