
#include "InspectorCustomizationRegistry.h"

#include <algorithm>

namespace VNX {

InspectorCustomizationRegistry& InspectorCustomizationRegistry::instance()
{
    static InspectorCustomizationRegistry s;
    return s;
}

void InspectorCustomizationRegistry::registerFieldDrawer(PropType type, std::unique_ptr<IFieldDrawer> drawer)
{
    fieldDrawers_[type] = std::move(drawer);
}

IFieldDrawer* InspectorCustomizationRegistry::fieldDrawer(PropType type) const
{
    auto it = fieldDrawers_.find(type);
    return it != fieldDrawers_.end() ? it->second.get() : nullptr;
}

void InspectorCustomizationRegistry::registerSectionProvider(ProjectItem::Type type, const QString& slot,
                                                             std::unique_ptr<ISectionProvider> provider)
{
    sectionProviders_[{type, slot}] = std::move(provider);
}

ISectionProvider* InspectorCustomizationRegistry::sectionProvider(ProjectItem::Type type, const QString& slot) const
{
    auto it = sectionProviders_.find({type, slot});
    return it != sectionProviders_.end() ? it->second.get() : nullptr;
}

void InspectorCustomizationRegistry::registerSlotDecorator(InspectorSlotKind kind, DecoratorFactory factory)
{
    if (std::find(slotOrder_.begin(), slotOrder_.end(), kind) == slotOrder_.end())
        slotOrder_.push_back(kind);
    slotFactories_[kind] = std::move(factory);
}

QWidget* InspectorCustomizationRegistry::makeSlotDecorator(
    InspectorSlotKind kind, const PropertyDescriptor& d) const
{
    auto it = slotFactories_.find(kind);
    return it != slotFactories_.end() && it->second ? it->second(d) : nullptr;
}

const std::vector<InspectorSlotKind>& InspectorCustomizationRegistry::registeredSlotKinds() const
{
    return slotOrder_;
}

void InspectorCustomizationRegistry::setBindingDecoratorFactory(DecoratorFactory factory)
{
    registerSlotDecorator(InspectorSlotKind::Binding, std::move(factory));
}

QWidget* InspectorCustomizationRegistry::makeBindingDecorator(const PropertyDescriptor& d) const
{
    return makeSlotDecorator(InspectorSlotKind::Binding, d);
}

void InspectorCustomizationRegistry::clear()
{
    fieldDrawers_.clear();
    sectionProviders_.clear();
    slotFactories_.clear();
    slotOrder_.clear();
}

}
