
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"

#include "Core/PropertyDescriptor.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"

#include "Widgets/Inspector/Drawers/ColorFieldDrawer.h"
#include "Widgets/Inspector/Drawers/ListFieldDrawer.h"
#include "Widgets/Inspector/Drawers/ObjectRefFieldDrawer.h"
#include "Widgets/Inspector/Drawers/RangeFieldDrawer.h"
#include "Widgets/Inspector/Providers/DimensionsSectionProvider.h"
#include "Widgets/Inspector/Providers/SelectionSectionProvider.h"
#include "Widgets/Inspector/Providers/SourcesSectionProvider.h"
#include "Widgets/Inspector/Providers/StepsSectionProvider.h"
#include "Widgets/Inspector/Providers/ClipsSectionProvider.h"
#include "Widgets/Inspector/BindingFieldDecorator.h"
#include "Widgets/Inspector/AttenuationFieldDecorator.h"

#include <memory>

namespace VNX {

void registerBuiltinInspectorCustomizations()
{
    static bool installed = false;
    if (installed) return;
    installed = true;

    InspectorCustomizationRegistry::instance().registerFieldDrawer(
        PropType::ObjectRef, std::make_unique<ObjectRefFieldDrawer>());
    InspectorCustomizationRegistry::instance().registerFieldDrawer(
        PropType::Color, std::make_unique<ColorFieldDrawer>());
    InspectorCustomizationRegistry::instance().registerFieldDrawer(
        PropType::List, std::make_unique<ListFieldDrawer>());
    InspectorCustomizationRegistry::instance().registerFieldDrawer(
        PropType::Range, std::make_unique<RangeFieldDrawer>());

    registerDimensionsSectionProvider();
    registerStepsSectionProvider();
    registerSelectionSectionProvider();
    registerSourcesSectionProvider();
    registerClipsSectionProvider();

    registerBindingFieldDecorator();
    registerAttenuationFieldDecorator();
}

}
