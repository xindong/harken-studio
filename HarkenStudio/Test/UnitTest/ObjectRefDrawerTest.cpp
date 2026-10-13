#include "Components/VBus.h"
#include "Components/VMixNode.h"
#include "PropertyDescriptor.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QToolButton>

#include <memory>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

const VNX::PropertyDescriptor& outputBusDescriptor()
{
    for (const auto& d : VNX::VMixNode::schema())
        if (d.field == QLatin1String("outputBusId")) return d;
    ADD_FAILURE() << "outputBusId descriptor missing";
    static VNX::PropertyDescriptor dummy;
    return dummy;
}

VNX::IFieldDrawer* objectRefDrawer()
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    return VNX::InspectorCustomizationRegistry::instance().fieldDrawer(VNX::PropType::ObjectRef);
}

} // namespace

TEST(ObjectRefDrawer, BuildsRefWidgetWithBusFilter)
{
    auto* drawer = objectRefDrawer();
    ASSERT_NE(drawer, nullptr);

    std::unique_ptr<QWidget> w(drawer->build(outputBusDescriptor(), nullptr));
    auto* ref = qobject_cast<VNX::ObjectRefWidget*>(w.get());
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->filterType(), VNX::ProjectItem::Type::Bus);
    EXPECT_EQ(ref->objectName(), QStringLiteral("prop_outputBusId"));
    EXPECT_NE(ref->findChild<QToolButton*>(QStringLiteral("refPick")), nullptr);
}

TEST(ObjectRefDrawer, BusHostHidesPickButton)
{
    auto* drawer = objectRefDrawer();
    ASSERT_NE(drawer, nullptr);

    std::unique_ptr<QWidget> w(drawer->build(outputBusDescriptor(), nullptr));
    auto* pick = w->findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(pick, nullptr);

    auto bus = std::make_shared<VNX::ProjectItem>(QStringLiteral("SFX"),
                                                  VNX::ProjectItem::Type::Bus);
    bus->compOrCreate<VNX::VBus>();
    drawer->bind(w.get(), QVariant(0u), bus.get());
    EXPECT_TRUE(pick->isHidden());

    auto sound = std::make_shared<VNX::ProjectItem>(QStringLiteral("Shot"),
                                                    VNX::ProjectItem::Type::Sound);
    drawer->bind(w.get(), QVariant(5u), sound.get());
    EXPECT_FALSE(pick->isHidden());
}

TEST(ObjectRefWidget, UnresolvedReferenceShowsMissingWithoutGuid)
{
    ensureApp();
    VNX::ObjectRefWidget widget;
    widget.setUnresolvedReference(QStringLiteral("dead-guid-value"));
    auto* name = widget.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    EXPECT_FALSE(name->text().contains(QStringLiteral("dead-guid-value")));
    EXPECT_TRUE(widget.toolTip().contains(QStringLiteral("dead-guid-value")));
}

TEST(ObjectRefDrawer, EmptyRoutingIsDisplayedWithoutPerFieldInheritance)
{
    auto* drawer = objectRefDrawer();
    ASSERT_NE(drawer, nullptr);

    std::unique_ptr<QWidget> w(drawer->build(outputBusDescriptor(), nullptr));
    auto* ref = qobject_cast<VNX::ObjectRefWidget*>(w.get());
    ASSERT_NE(ref, nullptr);

    auto sound = std::make_shared<VNX::ProjectItem>(QStringLiteral("Shot"),
                                                    VNX::ProjectItem::Type::Sound);
    drawer->bind(w.get(), QVariant(0u), sound.get());
    EXPECT_FALSE(ref->isInherited());
    EXPECT_EQ(ref->currentObjectID(), 0u);
    EXPECT_FALSE(ref->property("inputInvalid").toBool());

    drawer->bind(w.get(), QVariant(5u), sound.get());
    EXPECT_FALSE(ref->isInherited());
    EXPECT_EQ(ref->currentObjectID(), 5u);
    EXPECT_TRUE(ref->property("inputInvalid").toBool());
}

TEST(ObjectRefDrawer, SuppliedEffectiveRoutingIsDisplayedWithoutReadingParentComponents)
{
    auto* drawer = objectRefDrawer();
    ASSERT_NE(drawer, nullptr);

    std::unique_ptr<QWidget> w(drawer->build(outputBusDescriptor(), nullptr));
    auto* ref = qobject_cast<VNX::ObjectRefWidget*>(w.get());
    ASSERT_NE(ref, nullptr);

    auto container = std::make_shared<VNX::ProjectItem>(QStringLiteral("Weapons"),
                                                        VNX::ProjectItem::Type::Layer);
    container->compOrCreate<VNX::VMixNode>()->outputBusId = 77u;
    auto sound = std::make_shared<VNX::ProjectItem>(QStringLiteral("Shot"),
                                                    VNX::ProjectItem::Type::Sound);
    sound->parent = container.get();

    drawer->bind(w.get(), QVariant(28u), sound.get());
    EXPECT_FALSE(ref->isInherited());
    EXPECT_EQ(ref->currentObjectID(), 28u);
}

TEST(ObjectRefDrawer, ObjectChangedForwardsToOnChange)
{
    auto* drawer = objectRefDrawer();
    ASSERT_NE(drawer, nullptr);

    std::unique_ptr<QWidget> w(drawer->build(outputBusDescriptor(), nullptr));
    auto* ref = qobject_cast<VNX::ObjectRefWidget*>(w.get());
    ASSERT_NE(ref, nullptr);

    QVariant captured;
    drawer->connectChanged(w.get(), [&captured](const QVariant& v) { captured = v; });
    emit ref->objectChanged(42u);

    ASSERT_TRUE(captured.isValid());
    EXPECT_EQ(captured.toUInt(), 42u);
}
