#include "PropertyDescriptor.h"
#include "ProjectTypes.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/Drawers/ListFieldDrawer.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
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

VNX::PropertyDescriptor enumListDescriptor()
{
    auto descriptor = ListHarnessComp::schema()[0];
    VNX::PropertyDescriptor choice;
    choice.field = QStringLiteral("choice");
    choice.label = QStringLiteral("Choice");
    choice.type = VNX::PropType::Enum;
    choice.def = 0;
    choice.options = {{"First", 0}, {"Second", 1}, {"Third", 2}};
    descriptor.elementSchema = {choice, descriptor.elementSchema.front()};
    descriptor.validateElement = [](const QVariantList& rows, int row, const QString&) {
        const auto candidate = rows[row].toMap();
        for (int index = 0; index < rows.size(); ++index) {
            const auto other = rows[index].toMap();
            if (index != row && other.value(QStringLiteral("target")) == candidate.value(QStringLiteral("target"))
                && other.value(QStringLiteral("choice")) == candidate.value(QStringLiteral("choice")))
                return QStringLiteral("The target and choice must be unique");
        }
        return QString{};
    };
    return descriptor;
}

QVariantMap choiceEntry(uint target, int choice)
{
    return {{QStringLiteral("target"), target}, {QStringLiteral("choice"), choice}};
}

bool optionEnabled(const QComboBox* combo, int value)
{
    const auto index = combo->model()->index(combo->findData(value), combo->modelColumn());
    return index.flags().testFlag(Qt::ItemIsEnabled)
        && index.flags().testFlag(Qt::ItemIsSelectable);
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

TEST(ListProperty, EnumOptionsFollowExternalRowConstraintsOnUnchangedBinding)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    auto descriptor = enumListDescriptor();
    bool restricted = true;
    descriptor.elementSchema[1].refSelectableForElement =
        [&restricted](uint32_t, const QVariantMap& row, uint32_t target) {
            return !restricted || target != 7u || row.value(QStringLiteral("choice")).toInt() == 2;
        };
    VNX::ProjectItem owner(QStringLiteral("Owner"), VNX::ProjectItem::Type::Bus);
    owner.objectID = 42;
    std::unique_ptr<QWidget> widget(drawer->build(descriptor, nullptr));
    const QVariantList rows{choiceEntry(7u, 2)};
    int changes = 0;
    drawer->connectChanged(widget.get(), [&](const QVariant&) { ++changes; });
    drawer->bind(widget.get(), rows, &owner);
    auto* combo = widget->findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(optionEnabled(combo, 0));
    EXPECT_FALSE(optionEnabled(combo, 1));
    EXPECT_TRUE(optionEnabled(combo, 2));
    EXPECT_FALSE(combo->itemData(0, Qt::ToolTipRole).toString().isEmpty());
    combo->setCurrentIndex(combo->findData(0));
    EXPECT_EQ(combo->currentData().toInt(), 2);
    EXPECT_EQ(changes, 0);

    restricted = false;
    drawer->bind(widget.get(), rows, &owner);
    EXPECT_TRUE(optionEnabled(combo, 0));
    EXPECT_TRUE(optionEnabled(combo, 1));
    EXPECT_TRUE(combo->itemData(0, Qt::ToolTipRole).toString().isEmpty());
    restricted = true;
    drawer->bind(widget.get(), rows, &owner);
    EXPECT_FALSE(optionEnabled(combo, 0));
    EXPECT_EQ(changes, 0);

    auto* reference = widget->findChild<VNX::ObjectRefWidget*>();
    ASSERT_NE(reference, nullptr);
    emit reference->objectChanged(9u);
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(optionEnabled(combo, 0));
    EXPECT_TRUE(optionEnabled(combo, 1));
}

TEST(ListProperty, EnumOptionsTrackSiblingEditsAndRemovalWithKeyboardSelection)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(enumListDescriptor(), nullptr));
    QVariantList captured;
    int changes = 0;
    drawer->connectChanged(widget.get(), [&](const QVariant& value) {
        captured = value.toList();
        ++changes;
    });
    drawer->bind(widget.get(), QVariantList{choiceEntry(7u, 0), choiceEntry(7u, 1)}, nullptr);
    const auto combos = widget->findChildren<QComboBox*>();
    ASSERT_EQ(combos.size(), 2);
    EXPECT_FALSE(optionEnabled(combos[0], 1));
    EXPECT_FALSE(optionEnabled(combos[1], 0));
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(combos[0], &down);
    EXPECT_EQ(combos[0]->currentData().toInt(), 2);
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(optionEnabled(combos[1], 0));
    EXPECT_FALSE(optionEnabled(combos[1], 2));
    combos[1]->setCurrentIndex(combos[1]->findData(2));
    EXPECT_EQ(combos[1]->currentData().toInt(), 1);
    EXPECT_EQ(changes, 1);

    auto* remove = widget->findChild<QToolButton*>(QStringLiteral("listRemoveSelected_entries"));
    ASSERT_NE(remove, nullptr);
    ASSERT_TRUE(remove->isEnabled());
    remove->click();
    ASSERT_EQ(changes, 2);
    ASSERT_EQ(captured.size(), 1);
    EXPECT_EQ(captured[0].toMap().value(QStringLiteral("choice")).toInt(), 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* remaining = widget->findChild<QComboBox*>();
    ASSERT_NE(remaining, nullptr);
    EXPECT_TRUE(optionEnabled(remaining, 0));
    EXPECT_TRUE(optionEnabled(remaining, 1));
    EXPECT_TRUE(optionEnabled(remaining, 2));
}

TEST(ListProperty, ReadOnlyEnumsRejectProgrammaticChanges)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    for (const bool wholeList : {false, true}) {
        auto descriptor = enumListDescriptor();
        descriptor.flags.readOnly = wholeList;
        descriptor.elementSchema[0].flags.readOnly = !wholeList;
        std::unique_ptr<QWidget> widget(drawer->build(descriptor, nullptr));
        int changes = 0;
        drawer->connectChanged(widget.get(), [&](const QVariant&) { ++changes; });
        drawer->bind(widget.get(), QVariantList{choiceEntry(7u, 0)}, nullptr);
        auto* combo = widget->findChild<QComboBox*>();
        ASSERT_NE(combo, nullptr);
        EXPECT_FALSE(combo->isEnabled());
        combo->setCurrentIndex(combo->findData(1));
        EXPECT_EQ(changes, 0);
    }
}

TEST(ListProperty, EnumCommitCanSynchronouslyRebindWithoutRetiredCallbacks)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(enumListDescriptor(), nullptr));
    drawer->bind(widget.get(), QVariantList{choiceEntry(7u, 0)}, nullptr);
    int changes = 0;
    drawer->connectChanged(widget.get(), [&](const QVariant&) {
        ++changes;
        drawer->bind(widget.get(), QVariantList{choiceEntry(9u, 2)}, nullptr);
    });
    auto* retired = widget->findChild<QComboBox*>();
    ASSERT_NE(retired, nullptr);
    retired->setCurrentIndex(retired->findData(1));
    ASSERT_EQ(changes, 1);
    retired->setCurrentIndex(retired->findData(0));
    EXPECT_EQ(changes, 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto* current = widget->findChild<QComboBox*>();
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->currentData().toInt(), 2);
    EXPECT_TRUE(optionEnabled(current, 0));
}

TEST(ListProperty, RowTransformsAreSharedByOptionValidationAndCommit)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    auto descriptor = enumListDescriptor();
    descriptor.editElement = [](QVariantMap row, const QString& field, const QVariant& value) {
        row[field] = value;
        if (field == QLatin1String("choice")) {
            if (value.toInt() == 1) row[QStringLiteral("target")] = 9u;
            else if (value.toInt() == 2) row[QStringLiteral("target")] = 11u;
        }
        return row;
    };
    std::unique_ptr<QWidget> widget(drawer->build(descriptor, nullptr));
    QVariantList captured;
    int changes = 0;
    drawer->connectChanged(widget.get(), [&](const QVariant& value) {
        captured = value.toList();
        ++changes;
    });
    drawer->bind(widget.get(), QVariantList{choiceEntry(7u, 0), choiceEntry(9u, 1)}, nullptr);
    const auto combos = widget->findChildren<QComboBox*>();
    const auto references = widget->findChildren<VNX::ObjectRefWidget*>();
    ASSERT_EQ(combos.size(), 2);
    ASSERT_EQ(references.size(), 2);
    EXPECT_FALSE(optionEnabled(combos[0], 1));
    combos[0]->setCurrentIndex(combos[0]->findData(1));
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(combos[0]->currentData().toInt(), 0);
    combos[0]->setCurrentIndex(combos[0]->findData(2));
    ASSERT_EQ(changes, 1);
    EXPECT_EQ(captured[0].toMap().value(QStringLiteral("target")).toUInt(), 11u);
    EXPECT_EQ(captured[0].toMap().value(QStringLiteral("choice")).toInt(), 2);
    EXPECT_EQ(references[0]->currentObjectID(), 11u);
}

TEST(ListProperty, InitialChoicesDoNotApplyExistingRowEdits)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    auto descriptor = enumListDescriptor();
    descriptor.listAddChoiceField = QStringLiteral("choice");
    descriptor.editElement = [](QVariantMap row, const QString& field, const QVariant& value) {
        row[field] = value;
        row[QStringLiteral("revision")] = row.value(QStringLiteral("revision")).toInt() + 1;
        return row;
    };
    std::unique_ptr<QWidget> widget(drawer->build(descriptor, nullptr));
    QVariantList captured;
    drawer->connectChanged(widget.get(), [&](const QVariant& value) { captured = value.toList(); });
    drawer->bind(widget.get(), QVariantList{}, nullptr);
    auto* add = widget->findChild<QToolButton*>(QStringLiteral("listAdd_entries"));
    ASSERT_NE(add, nullptr);
    add->click();
    auto* menu = widget->findChild<QMenu*>(QStringLiteral("listAddMenu_entries"));
    ASSERT_NE(menu, nullptr);
    ASSERT_EQ(menu->actions().size(), 3);
    menu->actions()[1]->trigger();
    menu->close();
    QCoreApplication::sendPostedEvents(widget.get(), QEvent::MetaCall);
    ASSERT_EQ(captured.size(), 1);
    EXPECT_EQ(captured[0].toMap().value(QStringLiteral("choice")).toInt(), 1);
    EXPECT_FALSE(captured[0].toMap().contains(QStringLiteral("revision")));
    auto* combo = widget->findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(combo->findData(2));
    EXPECT_EQ(captured[0].toMap().value(QStringLiteral("revision")).toInt(), 1);
}

TEST(ListProperty, ReadOnlyNumberedTablesDoNotExposeRowMutation)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    auto descriptor = VNX::withInspectorRowOrder(enumListDescriptor());
    descriptor.flags.readOnly = true;
    descriptor.listPresentation = VNX::ListPresentation::Display;
    std::unique_ptr<QWidget> widget(drawer->build(descriptor, nullptr));
    int changes = 0;
    drawer->connectChanged(widget.get(), [&](const QVariant&) { ++changes; });
    drawer->bind(widget.get(), QVariantList{choiceEntry(7u, 0), choiceEntry(9u, 1)}, nullptr);
    auto* table = widget->findChild<VNX::TableWidget*>();
    ASSERT_NE(table, nullptr);
    const auto numbers = widget->findChildren<QLabel*>(QStringLiteral("tableRowNumber"));
    ASSERT_EQ(numbers.size(), 2);
    EXPECT_EQ(numbers[0]->text(), QStringLiteral("1"));
    EXPECT_EQ(numbers[1]->text(), QStringLiteral("2"));
    EXPECT_FALSE(table->canMoveRow(0, 1));
    table->moveRow(0, 1);
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(widget->findChild<QToolButton*>(QStringLiteral("listAdd_entries")), nullptr);
    EXPECT_EQ(widget->findChild<QToolButton*>(QStringLiteral("listRemoveSelected_entries")), nullptr);
}
