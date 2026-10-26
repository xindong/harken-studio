#include "PropertyDescriptor.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/Drawers/ListFieldDrawer.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>

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

class ListHarnessComp : public VNX::ReflectedComponent<ListHarnessComp>
{
public:
    VNX::ComponentType getType() const override { return VNX::ComponentType::MixNode; }

    static const VNX::PropertySchema& schema();

    QVariantList entries;
};

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
const VNX::PropertySchema& ListHarnessComp::schema()
{
    static const VNX::PropertySchema s = {
        { .field="entries", .type=VNX::PropType::List, .label="Entries", .section=VNX::PropertySection::MainOutput,
          .def=QVariant(QVariantList{}),
          .elementSchema={
              { .field="target", .jsonKey="targetBusId", .type=VNX::PropType::ObjectRef,
                .def=QVariant(0u) },
              { .field="level", .type=VNX::PropType::Float, .unit=" dB",
                .min=-96.0, .max=12.0, .def=QVariant(-6.0), .decimals=1 },
          },
          .get=[](const VNX::IComponent* c) {
              return QVariant(static_cast<const ListHarnessComp*>(c)->entries);
          },
          .set=[](VNX::IComponent* c, const QVariant& v) {
              static_cast<ListHarnessComp*>(c)->entries = v.toList();
              return true;
          } },
    };
    return s;
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

QVariantMap makeEntry(uint target, double level)
{
    return QVariantMap{{QStringLiteral("target"), target},
                       {QStringLiteral("level"), level}};
}

VNX::IFieldDrawer* listDrawer()
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    return VNX::InspectorCustomizationRegistry::instance().fieldDrawer(VNX::PropType::List);
}

}

TEST(ListProperty, CancelledEditorsCannotWriteToReboundRows)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(ListHarnessComp::schema()[0], nullptr));
    QVariant captured;
    drawer->connectChanged(widget.get(), [&](const QVariant& value) { captured = value; });
    drawer->bind(widget.get(), QVariantList{makeEntry(7u, -6.0)}, nullptr);
    auto* oldSlider = widget->findChild<VNX::ParameterSlider*>();
    auto* oldReference = widget->findChild<VNX::ObjectRefWidget*>();
    ASSERT_NE(oldSlider, nullptr);
    ASSERT_NE(oldReference, nullptr);

    VNX::ListFieldDrawer::cancelWidgetEdits(widget.get());
    drawer->bind(widget.get(), QVariantList{makeEntry(9u, -3.0)}, nullptr);
    oldSlider->simulateValueChange(-13.0);
    emit oldReference->objectChanged(8u);
    EXPECT_FALSE(captured.isValid());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* currentSlider = widget->findChild<VNX::ParameterSlider*>();
    ASSERT_NE(currentSlider, nullptr);
    currentSlider->simulateValueChange(-10.0);
    ASSERT_TRUE(captured.isValid());
    ASSERT_EQ(captured.toList().size(), 1);
    EXPECT_EQ(captured.toList()[0].toMap().value(QStringLiteral("target")).toUInt(), 9u);
    EXPECT_DOUBLE_EQ(captured.toList()[0].toMap().value(QStringLiteral("level")).toDouble(), -10.0);
}

TEST(ListProperty, CommitCanSynchronouslyRebindTheRowsWithoutRetiredCallbacks)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(ListHarnessComp::schema()[0], nullptr));
    int changes = 0;
    QVariant captured;
    drawer->bind(widget.get(), QVariantList{makeEntry(7u, -6.0)}, nullptr);
    drawer->connectChanged(widget.get(), [&](const QVariant& value) {
        ++changes;
        captured = value;
        drawer->bind(widget.get(), QVariantList{makeEntry(9u, -2.0)}, nullptr);
    });
    auto* slider = widget->findChild<VNX::ParameterSlider*>();
    ASSERT_NE(slider, nullptr);
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-4.0);
    slider->binding()->commitEdit();
    ASSERT_EQ(changes, 1);
    EXPECT_DOUBLE_EQ(captured.toList()[0].toMap().value(QStringLiteral("level")).toDouble(), -4.0);
    slider->simulateValueChange(-3.0);
    EXPECT_EQ(changes, 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* replacement = widget->findChild<VNX::ParameterSlider*>();
    ASSERT_NE(replacement, nullptr);
    EXPECT_DOUBLE_EQ(replacement->value(), -2.0);
}
