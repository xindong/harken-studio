#include "Components/VVariable.h"
#include "PropertyDescriptor.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/Drawers/ListFieldDrawer.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ParameterValueEditor.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QSlider>
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

// The anchored-table case rides on the product schema it was built for: a
// Variable's labels, windowed by the declared range.
const VNX::PropertyDescriptor& labelsRow()
{
    for (const auto& d : VNX::VVariable::schema())
        if (d.field == QLatin1String("labels")) return d;
    ADD_FAILURE() << "missing labels row";
    static const VNX::PropertyDescriptor kEmpty;
    return kEmpty;
}

QVariantMap makeLabel(int anchor, const QString& name)
{
    return QVariantMap{{QStringLiteral("anchor"), anchor},
                       {QStringLiteral("name"), name}};
}

VNX::IFieldDrawer* listDrawer()
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    return VNX::InspectorCustomizationRegistry::instance().fieldDrawer(VNX::PropType::List);
}

// A rebuild retires its rows through deleteLater, so a test that inspects the
// table after one has to let those deletions land first.
int rowCount(QWidget* w)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return static_cast<int>(w->findChildren<QWidget*>(QStringLiteral("listRow")).size());
}

void windowTo(VNX::IFieldDrawer* drawer, QWidget* w, double low, double high)
{
    VNX::VVariable variable;
    variable.rangeMin = static_cast<float>(low);
    variable.rangeMax = static_cast<float>(high);
    const auto domain = labelsRow().domain(&variable);
    ASSERT_TRUE(domain.has_value());
    drawer->applyDomain(w, *domain);
}

} // namespace

TEST(ListProperty, JsonRoundTrip)
{
    ListHarnessComp comp;
    comp.entries = QVariantList{makeEntry(7u, -6.0), makeEntry(3u, 0.0)};

    const QJsonObject j = comp.toJson();
    ASSERT_TRUE(j.value(QStringLiteral("entries")).isArray());
    const QJsonArray arr = j.value(QStringLiteral("entries")).toArray();
    ASSERT_EQ(arr.size(), 2);
    const QJsonObject e0 = arr.at(0).toObject();
    EXPECT_EQ(e0.value(QStringLiteral("targetBusId")).toInt(), 7);
    EXPECT_DOUBLE_EQ(e0.value(QStringLiteral("level")).toDouble(), -6.0);
    EXPECT_FALSE(e0.contains(QStringLiteral("target")));

    ListHarnessComp reloaded;
    reloaded.fromJson(j);
    ASSERT_EQ(reloaded.entries.size(), 2);
    const QVariantMap r0 = reloaded.entries[0].toMap();
    const QVariantMap r1 = reloaded.entries[1].toMap();
    EXPECT_EQ(r0.value(QStringLiteral("target")).toUInt(), 7u);
    EXPECT_DOUBLE_EQ(r0.value(QStringLiteral("level")).toDouble(), -6.0);
    EXPECT_EQ(r1.value(QStringLiteral("target")).toUInt(), 3u);
    EXPECT_DOUBLE_EQ(r1.value(QStringLiteral("level")).toDouble(), 0.0);
}

TEST(ListProperty, MissingKeyYieldsEmptyList)
{
    ListHarnessComp comp;
    comp.entries = QVariantList{makeEntry(7u, -6.0)};
    comp.fromJson(QJsonObject{});
    EXPECT_TRUE(comp.entries.isEmpty());
}

TEST(ListProperty, ElementFieldFallsBackToDefault)
{
    QJsonObject elem;
    elem[QStringLiteral("targetBusId")] = 9;
    QJsonArray arr;
    arr.append(elem);
    QJsonObject j;
    j[QStringLiteral("entries")] = arr;

    ListHarnessComp comp;
    comp.fromJson(j);
    ASSERT_EQ(comp.entries.size(), 1);
    const QVariantMap e = comp.entries[0].toMap();
    EXPECT_EQ(e.value(QStringLiteral("target")).toUInt(), 9u);
    EXPECT_DOUBLE_EQ(e.value(QStringLiteral("level")).toDouble(), -6.0);
}

TEST(ListProperty, DrawerRendersRowsAndEditsElements)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* drawer = VNX::InspectorCustomizationRegistry::instance().fieldDrawer(VNX::PropType::List);
    ASSERT_NE(drawer, nullptr);

    const VNX::PropertyDescriptor& d = ListHarnessComp::schema()[0];
    std::unique_ptr<QWidget> w(drawer->build(d, nullptr));
    QVariant captured;
    drawer->connectChanged(w.get(), [&captured](const QVariant& v) { captured = v; });
    drawer->bind(w.get(), QVariantList{makeEntry(7u, -6.0), makeEntry(3u, 0.0)}, nullptr);

    const auto spins = w->findChildren<QDoubleSpinBox*>();
    ASSERT_EQ(spins.size(), 2);
    EXPECT_DOUBLE_EQ(spins[0]->value(), -6.0);

    spins[0]->setValue(-12.0);
    ASSERT_TRUE(captured.isValid());
    QVariantList edited = captured.toList();
    ASSERT_EQ(edited.size(), 2);
    EXPECT_DOUBLE_EQ(edited[0].toMap().value(QStringLiteral("level")).toDouble(), -12.0);
    EXPECT_EQ(edited[1].toMap().value(QStringLiteral("target")).toUInt(), 3u);
}

TEST(ListProperty, SeparateValueEditorSynchronizesAndCancelsDraftWithoutCommitting)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(ListHarnessComp::schema()[0], nullptr));
    QVariant captured;
    int changes = 0;
    drawer->connectChanged(widget.get(), [&](const QVariant& value) {
        captured = value;
        ++changes;
    });
    drawer->bind(widget.get(), QVariantList{makeEntry(7u, -6.0)}, nullptr);
    auto* slider = widget->findChild<VNX::ParameterSlider*>();
    auto* value = widget->findChild<VNX::ParameterValueEditor*>();
    ASSERT_NE(slider, nullptr);
    ASSERT_NE(value, nullptr);
    auto* track = slider->findChild<QSlider*>();
    auto* spin = value->valueEditor();
    ASSERT_NE(track, nullptr);
    ASSERT_NE(spin, nullptr);
    auto* input = spin->findChild<QLineEdit*>();
    ASSERT_NE(input, nullptr);
    widget->resize(720, 160);
    widget->show();
    QCoreApplication::processEvents();

    QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
    QApplication::sendEvent(track, &end);
    EXPECT_DOUBLE_EQ(slider->value(), 12.0);
    EXPECT_DOUBLE_EQ(spin->value(), 12.0);
    ASSERT_EQ(changes, 1);
    EXPECT_DOUBLE_EQ(captured.toList()[0].toMap().value(QStringLiteral("level")).toDouble(), 12.0);

    const auto typeValue = [input](const QString& text) {
        input->setFocus();
        input->selectAll();
        for (const QChar character : text) {
            QKeyEvent key(QEvent::KeyPress, character.unicode(), Qt::NoModifier, QString(character));
            QApplication::sendEvent(input, &key);
        }
    };
    typeValue(QStringLiteral("-20"));
    EXPECT_EQ(changes, 1);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    QApplication::sendEvent(spin, &enter);
    ASSERT_EQ(changes, 2);
    EXPECT_DOUBLE_EQ(slider->value(), -20.0);
    EXPECT_DOUBLE_EQ(spin->value(), -20.0);
    EXPECT_DOUBLE_EQ(captured.toList()[0].toMap().value(QStringLiteral("level")).toDouble(), -20.0);
    const QString committedText = input->text();

    typeValue(QStringLiteral("-30"));
    ASSERT_NE(input->text(), committedText);
    EXPECT_EQ(changes, 2);
    VNX::ListFieldDrawer::cancelWidgetEdits(widget.get(), true);
    QCoreApplication::processEvents();
    EXPECT_EQ(changes, 2);
    EXPECT_EQ(input->text(), committedText);
    EXPECT_DOUBLE_EQ(slider->value(), -20.0);
}

TEST(ListProperty, DrawerAddAppendsElementWithDefaults)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* drawer = VNX::InspectorCustomizationRegistry::instance().fieldDrawer(VNX::PropType::List);
    ASSERT_NE(drawer, nullptr);

    const VNX::PropertyDescriptor& d = ListHarnessComp::schema()[0];
    std::unique_ptr<QWidget> w(drawer->build(d, nullptr));
    QVariant captured;
    drawer->connectChanged(w.get(), [&captured](const QVariant& v) { captured = v; });
    drawer->bind(w.get(), QVariantList{}, nullptr);

    auto* addBtn = w->findChild<QToolButton*>(QStringLiteral("listAdd_entries"));
    ASSERT_NE(addBtn, nullptr);
    addBtn->click();

    const QVariantList added = captured.toList();
    ASSERT_EQ(added.size(), 1);
    EXPECT_EQ(added[0].toMap().value(QStringLiteral("target")).toUInt(), 0u);
    EXPECT_DOUBLE_EQ(added[0].toMap().value(QStringLiteral("level")).toDouble(), -6.0);
}

TEST(ListProperty, AnchoredAddFillsTheLowestFreeAnchorAndStopsAtAFullWindow)
{
    VNX::IFieldDrawer* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);

    std::unique_ptr<QWidget> w(drawer->build(labelsRow(), nullptr));
    QVariant captured;
    drawer->connectChanged(w.get(), [&captured](const QVariant& v) { captured = v; });
    windowTo(drawer, w.get(), 0.0, 3.0);
    drawer->bind(w.get(), QVariantList{}, nullptr);

    auto* addBtn = w->findChild<QToolButton*>(QStringLiteral("listAdd_labels"));
    ASSERT_NE(addBtn, nullptr);
    for (int expected = 0; expected <= 3; ++expected)
    {
        ASSERT_TRUE(addBtn->isEnabled()) << expected;
        addBtn->click();
        const QVariantList added = captured.toList();
        ASSERT_EQ(added.size(), expected + 1);
        EXPECT_EQ(added.back().toMap().value(QStringLiteral("anchor")).toInt(), expected);
    }

    // Four integers, four anchors: the window has no free anchor left.
    EXPECT_FALSE(addBtn->isEnabled());

    // A hole reopens it, and the new row takes that hole rather than the tail.
    drawer->bind(w.get(), QVariantList{makeLabel(0, QStringLiteral("Calm")),
                                       makeLabel(2, QStringLiteral("Loud")),
                                       makeLabel(3, QStringLiteral("Boss"))},
                 nullptr);
    ASSERT_TRUE(addBtn->isEnabled());
    addBtn->click();
    EXPECT_EQ(captured.toList()[1].toMap().value(QStringLiteral("anchor")).toInt(), 1);
}

TEST(ListProperty, InvalidLabelNamesShowErrorAndCannotCommitIncludingHiddenDuplicates)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(labelsRow(), nullptr));
    QVariant captured;
    drawer->connectChanged(widget.get(), [&captured](const QVariant& value) { captured = value; });
    windowTo(drawer, widget.get(), 0, 3);
    drawer->bind(widget.get(), QVariantList{makeLabel(0, QStringLiteral("None")),
        makeLabel(1, QString()), makeLabel(10, QStringLiteral("Hidden"))}, nullptr);
    const auto editors = widget->findChildren<QLineEdit*>();
    ASSERT_EQ(editors.size(), 2);
    auto* edit = editors[1];
    for (const QString& invalid : {QStringLiteral("None"), QStringLiteral("Hidden"),
             QStringLiteral("2"), QStringLiteral(" Ready"), QStringLiteral("Ready "),
             QStringLiteral(" \t ")}) {
        edit->setText(invalid);
        EXPECT_TRUE(edit->property("inputInvalid").toBool()) << invalid.toStdString();
        EXPECT_FALSE(edit->hasAcceptableInput());
        QMetaObject::invokeMethod(edit, "editingFinished");
        EXPECT_FALSE(captured.isValid());
        EXPECT_EQ(edit->text(), invalid);
    }
    for (const QString& valid : {QStringLiteral("none"), QStringLiteral("Stage 2"), QString()}) {
        edit->setText(valid);
        EXPECT_FALSE(edit->property("inputInvalid").toBool());
        QMetaObject::invokeMethod(edit, "editingFinished");
        EXPECT_EQ(captured.toList()[1].toMap().value(QStringLiteral("name")).toString(), valid);
    }
}

TEST(ListProperty, AnchorsOutsideTheWindowLeaveTheTableButNotTheValue)
{
    VNX::IFieldDrawer* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);

    std::unique_ptr<QWidget> w(drawer->build(labelsRow(), nullptr));
    QVariant captured;
    drawer->connectChanged(w.get(), [&captured](const QVariant& v) { captured = v; });
    windowTo(drawer, w.get(), 0.0, 10.0);
    drawer->bind(w.get(), QVariantList{makeLabel(0, QStringLiteral("Calm")),
                                       makeLabel(10, QStringLiteral("Boss"))},
                 nullptr);
    EXPECT_EQ(rowCount(w.get()), 2);

    windowTo(drawer, w.get(), 0.0, 5.0);
    ASSERT_EQ(rowCount(w.get()), 1);

    EXPECT_TRUE(w->findChildren<QSpinBox*>().empty());
    const auto names = w->findChildren<QLineEdit*>();
    ASSERT_EQ(names.size(), 1);
    names.front()->setText(QStringLiteral("Quiet"));
    QMetaObject::invokeMethod(names.front(), "editingFinished");
    const QVariantList edited = captured.toList();
    ASSERT_EQ(edited.size(), 2);
    EXPECT_EQ(edited[0].toMap().value(QStringLiteral("anchor")).toInt(), 0);
    EXPECT_EQ(edited[1].toMap().value(QStringLiteral("anchor")).toInt(), 10);
    EXPECT_EQ(edited[1].toMap().value(QStringLiteral("name")).toString(),
              QStringLiteral("Boss"));

    windowTo(drawer, w.get(), 0.0, 10.0);
    EXPECT_EQ(rowCount(w.get()), 2);
}

TEST(ListProperty, DrawerRemoveDeletesRow)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* drawer = VNX::InspectorCustomizationRegistry::instance().fieldDrawer(VNX::PropType::List);
    ASSERT_NE(drawer, nullptr);

    const VNX::PropertyDescriptor& d = ListHarnessComp::schema()[0];
    std::unique_ptr<QWidget> w(drawer->build(d, nullptr));
    QVariant captured;
    drawer->connectChanged(w.get(), [&captured](const QVariant& v) { captured = v; });
    drawer->bind(w.get(), QVariantList{makeEntry(7u, -6.0), makeEntry(3u, 0.0),
                                      makeEntry(9u, -12.0)}, nullptr);

    auto rows = w->findChildren<QWidget*>(QStringLiteral("listRow"));
    ASSERT_EQ(rows.size(), 3);
    auto* removeBtn = w->findChild<QToolButton*>(QStringLiteral("listRemoveSelected_entries"));
    ASSERT_NE(removeBtn, nullptr);
    EXPECT_FALSE(removeBtn->isEnabled());

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(5.0, 5.0),
                      rows[1]->mapToGlobal(QPointF(5.0, 5.0)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(rows[1], &press);
    ASSERT_TRUE(removeBtn->isEnabled());
    removeBtn->click();
    QCoreApplication::processEvents();

    QVariantList remaining = captured.toList();
    ASSERT_EQ(remaining.size(), 2);
    EXPECT_EQ(remaining[0].toMap().value(QStringLiteral("target")).toUInt(), 7u);
    EXPECT_EQ(remaining[1].toMap().value(QStringLiteral("target")).toUInt(), 9u);
    ASSERT_EQ(rowCount(w.get()), 2);
    rows = w->findChildren<QWidget*>(QStringLiteral("listRow"));
    EXPECT_FALSE(rows[0]->property("selected").toBool());
    EXPECT_TRUE(rows[1]->property("selected").toBool());
    ASSERT_TRUE(removeBtn->isEnabled());

    removeBtn->click();
    QCoreApplication::processEvents();

    remaining = captured.toList();
    ASSERT_EQ(remaining.size(), 1);
    EXPECT_EQ(remaining[0].toMap().value(QStringLiteral("target")).toUInt(), 7u);
    ASSERT_EQ(rowCount(w.get()), 1);
    rows = w->findChildren<QWidget*>(QStringLiteral("listRow"));
    EXPECT_TRUE(rows[0]->property("selected").toBool());
    ASSERT_TRUE(removeBtn->isEnabled());

    removeBtn->click();
    QCoreApplication::processEvents();

    EXPECT_TRUE(captured.toList().isEmpty());
    EXPECT_EQ(rowCount(w.get()), 0);
    EXPECT_FALSE(removeBtn->isEnabled());
}

TEST(ListProperty, LabelRowMoveKeepsSparsePositionsAndHiddenIdentities)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(labelsRow(), nullptr));
    QVariant captured;
    drawer->connectChanged(widget.get(), [&](const QVariant& value) { captured = value; });
    QVariantList original;
    for (int index : {0, 10, 12, 20, 100}) {
        auto label = makeLabel(index, QString::number(index));
        label[QStringLiteral("id")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        original.append(label);
    }
    windowTo(drawer, widget.get(), 10.0, 20.0);
    drawer->bind(widget.get(), original, nullptr);
    auto* table = widget->findChild<VNX::TableWidget*>();
    ASSERT_NE(table, nullptr);
    table->moveRow(1, 3);
    const auto moved = captured.toList();
    ASSERT_EQ(moved.size(), 5);
    EXPECT_EQ(moved.front(), original.front());
    EXPECT_EQ(moved.back(), original.back());
    for (int i = 1; i <= 3; ++i) {
        EXPECT_EQ(moved[i].toMap()[QStringLiteral("anchor")], original[i].toMap()[QStringLiteral("anchor")]);
        const auto source = original[i == 3 ? 1 : i + 1].toMap();
        EXPECT_EQ(moved[i].toMap()[QStringLiteral("id")], source[QStringLiteral("id")]);
        EXPECT_EQ(moved[i].toMap()[QStringLiteral("name")], source[QStringLiteral("name")]);
    }
    EXPECT_TRUE(widget->findChildren<QSpinBox*>().empty());
}

TEST(ListProperty, ReadOnlyCancelsPreviewAndRejectsRetiredEditorsAndFooterActions)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(ListHarnessComp::schema()[0], nullptr));
    int changes = 0;
    drawer->connectChanged(widget.get(), [&](const QVariant&) { ++changes; });
    drawer->bind(widget.get(), QVariantList{makeEntry(7u, -6.0)}, nullptr);
    auto* slider = widget->findChild<VNX::ParameterSlider*>();
    auto* reference = widget->findChild<VNX::ObjectRefWidget*>();
    auto* add = widget->findChild<QToolButton*>(QStringLiteral("listAdd_entries"));
    auto* remove = widget->findChild<QToolButton*>(QStringLiteral("listRemoveSelected_entries"));
    ASSERT_NE(slider, nullptr);
    ASSERT_NE(reference, nullptr);
    ASSERT_NE(add, nullptr);
    ASSERT_NE(remove, nullptr);

    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-20.0);
    EXPECT_EQ(changes, 0);
    VNX::ListFieldDrawer::setWidgetReadOnly(widget.get(), true);
    EXPECT_FALSE(slider->binding()->isEditing());
    slider->simulateValueChange(-13.0);
    emit reference->objectChanged(8u);
    emit add->clicked();
    emit remove->clicked();
    EXPECT_EQ(changes, 0);
    EXPECT_TRUE(add->isHidden() || add->parentWidget()->isHidden());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(widget->findChildren<VNX::ParameterSlider*>().isEmpty());
    auto* currentReference = widget->findChild<VNX::ObjectRefWidget*>();
    ASSERT_NE(currentReference, nullptr);
    EXPECT_TRUE(currentReference->isReadOnly());
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

TEST(ListProperty, MixedTableLocksMutationsAndRestoresTheBoundRows)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(ListHarnessComp::schema()[0], nullptr));
    int changes = 0;
    drawer->connectChanged(widget.get(), [&](const QVariant&) { ++changes; });
    drawer->bind(widget.get(), QVariantList{makeEntry(7u, -6.0)}, nullptr);
    auto* add = widget->findChild<QToolButton*>(QStringLiteral("listAdd_entries"));
    ASSERT_NE(add, nullptr);
    VNX::ListFieldDrawer::setWidgetMixed(widget.get(), true);
    auto* label = widget->findChild<QLabel*>(QStringLiteral("listMixedValue"));
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->isHidden());
    EXPECT_EQ(rowCount(widget.get()), 0);
    emit add->clicked();
    EXPECT_EQ(changes, 0);

    VNX::ListFieldDrawer::setWidgetMixed(widget.get(), false);
    EXPECT_EQ(rowCount(widget.get()), 1);
    auto* slider = widget->findChild<VNX::ParameterSlider*>();
    ASSERT_NE(slider, nullptr);
    EXPECT_DOUBLE_EQ(slider->value(), -6.0);
    slider->simulateValueChange(-4.0);
    EXPECT_EQ(changes, 1);
}

TEST(ListProperty, EditGuardRechecksAtEachMutation)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(ListHarnessComp::schema()[0], nullptr));
    int changes = 0;
    bool editable = true;
    VNX::ListFieldDrawer::setWidgetEditGuard(widget.get(), [&] { return editable; });
    drawer->connectChanged(widget.get(), [&](const QVariant&) { ++changes; });
    drawer->bind(widget.get(), QVariantList{makeEntry(7u, -6.0)}, nullptr);
    auto* slider = widget->findChild<VNX::ParameterSlider*>();
    auto* reference = widget->findChild<VNX::ObjectRefWidget*>();
    auto* add = widget->findChild<QToolButton*>(QStringLiteral("listAdd_entries"));
    ASSERT_NE(slider, nullptr);
    ASSERT_NE(reference, nullptr);
    ASSERT_NE(add, nullptr);
    editable = false;
    slider->simulateValueChange(-13.0);
    emit reference->objectChanged(8u);
    emit add->clicked();
    EXPECT_EQ(changes, 0);
    editable = true;
    slider->simulateValueChange(-12.0);
    EXPECT_EQ(changes, 1);
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

TEST(ListProperty, InteractivePreviewKeepsOtherRowsAndFinishesWithoutExtraCommit)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(ListHarnessComp::schema()[0], nullptr));
    QVariant preview;
    QVariant committed;
    int finishes = 0;
    int commits = 0;
    VNX::ListFieldDrawer::setWidgetPreviewCallbacks(widget.get(),
        [&](const QVariant& value) { preview = value; }, [&] { ++finishes; });
    drawer->connectChanged(widget.get(), [&](const QVariant& value) { committed = value; ++commits; });
    const QVariantList original{makeEntry(7u, -6.0), makeEntry(3u, -2.0)};
    drawer->bind(widget.get(), original, nullptr);
    auto* slider = widget->findChild<VNX::ParameterSlider*>();
    ASSERT_NE(slider, nullptr);
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-9.0);
    ASSERT_EQ(preview.toList().size(), 2);
    EXPECT_DOUBLE_EQ(preview.toList()[0].toMap().value(QStringLiteral("level")).toDouble(), -9.0);
    EXPECT_EQ(preview.toList()[1], original[1]);
    EXPECT_EQ(commits, 0);
    EXPECT_TRUE(slider->binding()->isEditing());
    slider->binding()->cancelEdit();
    EXPECT_EQ(finishes, 1);
    EXPECT_EQ(commits, 0);
    EXPECT_DOUBLE_EQ(slider->value(), -6.0);
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-12.0);
    slider->binding()->commitEdit();
    EXPECT_EQ(finishes, 2);
    EXPECT_EQ(commits, 1);
    EXPECT_DOUBLE_EQ(committed.toList()[0].toMap().value(QStringLiteral("level")).toDouble(), -12.0);
    EXPECT_EQ(committed.toList()[1], original[1]);
}

TEST(ListProperty, RetiringPreviewedRowsFinishesPreviewAndRejectsOldEditors)
{
    auto* drawer = listDrawer();
    ASSERT_NE(drawer, nullptr);
    std::unique_ptr<QWidget> widget(drawer->build(ListHarnessComp::schema()[0], nullptr));
    int previews = 0;
    int finishes = 0;
    int commits = 0;
    VNX::ListFieldDrawer::setWidgetPreviewCallbacks(widget.get(),
        [&](const QVariant&) { ++previews; }, [&] { ++finishes; });
    drawer->connectChanged(widget.get(), [&](const QVariant&) { ++commits; });
    drawer->bind(widget.get(), QVariantList{makeEntry(7u, -6.0)}, nullptr);
    auto* slider = widget->findChild<VNX::ParameterSlider*>();
    ASSERT_NE(slider, nullptr);
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-9.0);
    EXPECT_EQ(previews, 1);
    VNX::ListFieldDrawer::cancelWidgetEdits(widget.get());
    EXPECT_EQ(finishes, 1);
    EXPECT_FALSE(slider->binding()->isEditing());
    slider->binding()->beginEdit();
    slider->binding()->previewEdit(-12.0);
    slider->binding()->commitEdit();
    EXPECT_EQ(previews, 1);
    EXPECT_EQ(finishes, 1);
    EXPECT_EQ(commits, 0);
}

TEST(CollapsibleSection, HeaderControlsDoNotToggleExpansionByClickOrSpace)
{
    ensureApp();
    VNX::CollapsibleSection section(QStringLiteral("Section"), 0);
    section.setDynamicContent(true);
    auto* check = new QCheckBox(QStringLiteral("Inherit"));
    section.setHeaderWidget(check);
    EXPECT_EQ(section.headerWidget(), check);
    check->click();
    EXPECT_TRUE(check->isChecked());
    EXPECT_FALSE(section.isExpanded());

    section.setExpanded(true);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(check, &press);
    QApplication::sendEvent(check, &release);
    EXPECT_FALSE(check->isChecked());
    EXPECT_TRUE(section.isExpanded());
}
