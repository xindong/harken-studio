#pragma once

#include "InspectorCustomizationRegistry.h"
#include "InspectorSlotButton.h"
#include "Core/ProjectTypes.h"

namespace harken::studio {

class AttenuationFieldDecorator : public InspectorSlotButton, public IFieldDecorator
{
    Q_OBJECT

public:
    explicit AttenuationFieldDecorator(AttenuationDimension dimension,
                                      QWidget* parent = nullptr);

    InspectorSlotKind slotKind() const override { return InspectorSlotKind::Attenuation; }
    void bind(ProjectItem* item) override;
    void bindGroups(ProjectItem* item, const InspectorGroupLookup& groups) override;
    void refreshGroups(ProjectItem* item, const InspectorGroupLookup& groups) override;
    QString hoverSummary() const override { return toolTip(); }
    bool isReadOnly() const override { return !isEnabled(); }

private:
    void refreshPresentation(const FieldGroupInspectorView* view = nullptr);
    void activate();
    void openEditor();

    AttenuationDimension dimension_;
    ProjectItem* item_ = nullptr;
    bool pageProvidesGroups_ = false;
    QUuid attenuationGuid_;
};

void registerAttenuationFieldDecorator();

}
