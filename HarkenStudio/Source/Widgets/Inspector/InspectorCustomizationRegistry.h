
#pragma once

#include "Core/PropertyDescriptor.h"
#include "Core/ProjectTypes.h"
#include "InspectorSlotKind.h"

#include <QString>
#include <QVariant>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <vector>

class QMenu;

namespace VNX {

struct ProjectItem;

class IFieldDrawer
{
public:
    virtual ~IFieldDrawer() = default;

    virtual QWidget* build(const PropertyDescriptor& d, QWidget* parent) = 0;

    virtual void bind(QWidget* widget, const QVariant& value, ProjectItem* item) = 0;

    virtual void connectChanged(QWidget* widget, std::function<void(const QVariant&)> onChange) = 0;

    // Called whenever the engine re-resolves a descriptor's FieldDomain, for
    // drawers whose editor carries a numeric domain of its own.
    virtual void applyDomain(QWidget* widget, const FieldDomain& domain)
    {
        Q_UNUSED(widget)
        Q_UNUSED(domain)
    }
};

class ISectionProvider
{
public:
    virtual ~ISectionProvider() = default;

    virtual QWidget* build(QWidget* parent) = 0;

    virtual QWidget* buildHeader(QWidget*, QWidget*) { return nullptr; }

    virtual void bind(QWidget* widget, ProjectItem* item) = 0;

    virtual bool refresh(QWidget*, ProjectItem*, const QString&) { return false; }

    virtual QString summary(ProjectItem* ) const { return {}; }
};

class IFieldDecorator
{
public:
    virtual ~IFieldDecorator() = default;

    virtual InspectorSlotKind slotKind() const = 0;
    virtual void bind(ProjectItem* item) = 0;
    virtual QWidget* detailWidget() const { return nullptr; }
    virtual QString hoverSummary() const { return {}; }
    virtual bool isReadOnly() const { return false; }
    virtual void populateContextMenu(QMenu*) {}
};

class InspectorCustomizationRegistry
{
public:
    static InspectorCustomizationRegistry& instance();

    void registerFieldDrawer(PropType type, std::unique_ptr<IFieldDrawer> drawer);
    IFieldDrawer* fieldDrawer(PropType type) const;

    void registerSectionProvider(ProjectItem::Type type, const QString& slot,
                                 std::unique_ptr<ISectionProvider> provider);
    ISectionProvider* sectionProvider(ProjectItem::Type type, const QString& slot) const;

    using DecoratorFactory = std::function<QWidget*(const PropertyDescriptor&)>;
    void registerSlotDecorator(InspectorSlotKind kind, DecoratorFactory factory);
    QWidget* makeSlotDecorator(InspectorSlotKind kind, const PropertyDescriptor& d) const;
    const std::vector<InspectorSlotKind>& registeredSlotKinds() const;

    void setBindingDecoratorFactory(DecoratorFactory factory);
    QWidget* makeBindingDecorator(const PropertyDescriptor& d) const;

    void clear();

private:
    InspectorCustomizationRegistry() = default;

    std::map<PropType, std::unique_ptr<IFieldDrawer>> fieldDrawers_;
    std::map<std::pair<ProjectItem::Type, QString>, std::unique_ptr<ISectionProvider>> sectionProviders_;
    std::map<InspectorSlotKind, DecoratorFactory> slotFactories_;
    std::vector<InspectorSlotKind> slotOrder_;
};

}
