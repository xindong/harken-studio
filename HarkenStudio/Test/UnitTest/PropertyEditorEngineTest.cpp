#include "Support/FieldGroupTestUtils.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "Widgets/Inspector/ProjectItemInspectorRows.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterPresenter.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/ValueSegmentedControl.h"
#include "ActionManager.h"
#include "ISettingsStorage.h"
#include "MemorySettingsStorage.h"
#include "PlatformContext.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "PlayablePropertyAuthoringV1.h"
#include "FieldGroupJson.h"
#include "UndoCommands.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VBus.h"
#include "Components/VSpatial.h"
#include "QtControlSkin.h"
#include "ThemeManager.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QRect>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUuid>

#include <cmath>
#include <memory>
#include <tuple>
#include <vector>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

VNX::PropertySchema filterSchema(const VNX::PropertySchema& src, const QString& field)
{
    VNX::PropertySchema out;
    for (const auto& d : src)
        if (d.field == field)
            out.push_back(d);
    return out;
}

struct SignalCounters
{
    int itemPropertyChanged = 0;
    int busChanged = 0;
    QMetaObject::Connection itemConn;
    QMetaObject::Connection busConn;

    SignalCounters()
    {
        auto& pm = VNX::ProjectManager::instance();
        itemConn = QObject::connect(&pm, &VNX::ProjectManager::itemPropertyChanged,
            [this](VNX::ProjectItem*, const QString&) { ++itemPropertyChanged; });
        busConn = QObject::connect(&pm, &VNX::ProjectManager::busChanged,
            [this](uint32_t, const QString&) { ++busChanged; });
    }
    ~SignalCounters()
    {
        QObject::disconnect(itemConn);
        QObject::disconnect(busConn);
    }
};

struct FieldChangeRecorder
{
    std::vector<std::tuple<QString, QVariant, QVariant>> changes;

    explicit FieldChangeRecorder(VNX::PropertyEditorEngine& engine)
    {
        QObject::connect(&engine, &VNX::PropertyEditorEngine::fieldChanged,
            [this](const QString& f, const QVariant& oldV, const QVariant& newV) {
                changes.emplace_back(f, oldV, newV);
            });
    }
};

int undoStackCount()
{
    return VNX::UndoRedoManager::instance().undoStack()->count();
}

void typeDigits(QWidget* target, const QString& digits)
{
    for (const QChar character : digits)
    {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_0 + character.digitValue(),
                        Qt::NoModifier, QString(character));
        QApplication::sendEvent(target, &press);
    }
}

struct EngineFixture
{
    VNX::PropertyEditorEngine engine;
    std::unique_ptr<QWidget> page;
    VNX::ParameterSlider* slider = nullptr;
    QSlider* rawSlider = nullptr;

    EngineFixture(const VNX::PropertySchema& schema, VNX::ComponentType componentType,
                  VNX::ProjectItem::Type itemType)
    {
        engine.setSchema(schema, componentType);
        engine.setItemType(itemType);
        page.reset(engine.buildPage());
        slider = page->findChild<VNX::ParameterSlider*>();
        if (slider)
            rawSlider = slider->findChild<QSlider*>();
    }
};

std::shared_ptr<VNX::ProjectItem> makeAudioItem()
{
    auto item = std::make_shared<VNX::ProjectItem>(QStringLiteral("TestSound"),
                                                   VNX::ProjectItem::Type::Sound);
    item->compOrCreate<VNX::VAudio>();
    item->compOrCreate<VNX::VMixNode>();
    return item;
}

std::shared_ptr<VNX::ProjectItem> makeBusItem()
{
    auto item = std::make_shared<VNX::ProjectItem>(QStringLiteral("TestBus"),
                                                   VNX::ProjectItem::Type::Bus);
    item->objectID = 42;
    item->guid = QUuid::createUuid();
    item->compOrCreate<VNX::VBus>();
    item->compOrCreate<VNX::VMixNode>();
    return item;
}

struct NotesFixture
{
    VNX::PropertyEditorEngine engine;
    std::unique_ptr<QWidget> page;
    QPlainTextEdit* edit = nullptr;
    QWidget* grip = nullptr;

    NotesFixture()
    {
        engine.setSchema({VNX::notesInspectorDescriptor()}, VNX::ComponentType::_Count);
        engine.setItemType(VNX::ProjectItem::Type::Sound);
        page.reset(engine.buildPage());
        edit = page->findChild<QPlainTextEdit*>(QStringLiteral("prop_notes"));
        if (edit)
            grip = edit->findChild<QWidget*>(QStringLiteral("plainTextResizeGrip"));
    }
};

void sendGripMouseEvent(QWidget* grip, QEvent::Type type,
                        Qt::MouseButton button, Qt::MouseButtons buttons,
                        const QPointF& globalPosition)
{
    QMouseEvent event(type, grip->rect().center(), globalPosition,
                      button, buttons, Qt::NoModifier);
    QApplication::sendEvent(grip, &event);
}

void clickLeft(QWidget* widget)
{
    const QPointF local = widget->rect().center();
    QMouseEvent event(QEvent::MouseButtonPress, local,
                      widget->mapToGlobal(local.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

VNX::PropertySchema busVolumeAndPanSchema()
{
    VNX::PropertySchema out;
    for (const auto& d : VNX::busInspectorSchema())
        if (d.field == QLatin1String("volume") || d.field == QLatin1String("pan"))
            out.push_back(d);
    return out;
}

} // namespace

TEST(PropertyEditorEngineLayout, FloatColumnsScaleAcrossResizeAndPresenterRebuild)
{
    ensureApp();
    EngineFixture fx(busVolumeAndPanSchema(), VNX::ComponentType::MixNode,
                     VNX::ProjectItem::Type::Bus);
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    const auto controls = fx.page->findChildren<VNX::ParameterSlider*>();
    ASSERT_EQ(controls.size(), 2);
    fx.page->show();

    for (const bool rebuild : {false, true}) {
        if (rebuild)
            Q_EMIT VNX::ThemeManager::instance().themeChanged(
                VNX::ThemeManager::instance().snapshot());
        int previousValueWidth = 0;
        for (const int width : {360, 500, 800}) {
            SCOPED_TRACE(width);
            fx.page->resize(width, 720);
            QApplication::processEvents();
            int valueWidth = 0;
            for (auto* control : controls) {
                const QString field = control->objectName().mid(5);
                auto* label = fx.page->findChild<QLabel*>(
                    QStringLiteral("label_") + field);
                auto* gutter = fx.page->findChild<QWidget*>(
                    QStringLiteral("inspectorFieldGutter_") + field);
                auto* slider = control->presenter()->slider();
                auto* editor = control->presenter()->valueEditor();
                ASSERT_NE(label, nullptr);
                ASSERT_NE(gutter, nullptr);
                ASSERT_NE(slider, nullptr);
                ASSERT_NE(editor, nullptr);
                ASSERT_GT(slider->width(), 0);
                ASSERT_GT(editor->width(), 0);
                EXPECT_EQ(gutter->width(), inspector.slotWidthFor(2));
                EXPECT_NEAR(label->width()
                                / double(gutter->parentWidget()->width() - gutter->width()),
                            inspector.labelColumnShare
                                / double(inspector.labelColumnShare + inspector.valueColumnShare),
                            0.01);
                EXPECT_NEAR(editor->width() / double(slider->width() + editor->width()),
                            0.3, 0.01);
                const QRect sliderRect(slider->mapTo(control->presenter(), QPoint()), slider->size());
                const QRect editorRect(editor->mapTo(control->presenter(), QPoint()), editor->size());
                EXPECT_LT(sliderRect.right(), editorRect.left());
                EXPECT_LE(editorRect.right(), control->presenter()->rect().right());
                if (valueWidth > 0)
                    EXPECT_EQ(editor->width(), valueWidth);
                valueWidth = editor->width();
            }
            EXPECT_GT(valueWidth, previousValueWidth);
            previousValueWidth = valueWidth;
        }
    }
}

TEST(PropertyEditorEngineNotes, NotesEditorVisualsStayWithTheApplicationSkin)
{
    ensureApp();
    NotesFixture fx;
    ASSERT_NE(fx.edit, nullptr);
    ASSERT_NE(fx.grip, nullptr);

    EXPECT_EQ(fx.edit->property("fieldRole").toString(),
              QStringLiteral("inspectorText"));
    EXPECT_TRUE(fx.edit->styleSheet().isEmpty());

    Q_EMIT VNX::ThemeManager::instance().themeChanged(
        VNX::ThemeManager::instance().snapshot());

    EXPECT_EQ(fx.edit->property("fieldRole").toString(),
              QStringLiteral("inspectorText"));
    EXPECT_TRUE(fx.edit->styleSheet().isEmpty());
}

// Inspector pages are built once and never rebuilt, so any theme value baked
// into a widget's own stylesheet would survive a theme switch forever. Field
// labels, checkboxes and value fields therefore carry a semantic property and
// leave the pixels to the application skin, which is re-expanded on every
// theme change.
TEST(PropertyEditorEngineTheme, FieldVisualsStayWithTheApplicationSkin)
{
    ensureApp();
    EngineFixture fx(VNX::layerInspectorSchema(), VNX::ComponentType::MixNode,
                     VNX::ProjectItem::Type::Layer);

    auto* checkBox = fx.page->findChild<QCheckBox*>(
        QStringLiteral("prop_spatialIs3DEnabled"));
    ASSERT_NE(checkBox, nullptr);
    EXPECT_EQ(checkBox->property("fieldRole").toString(),
              QStringLiteral("inspector"));
    EXPECT_TRUE(checkBox->styleSheet().isEmpty());

    auto* label = fx.page->findChild<QLabel*>(
        QStringLiteral("label_spatialIs3DEnabled"));
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->property("textRole").toString(),
              QStringLiteral("formLabel"));
    EXPECT_TRUE(label->styleSheet().isEmpty());

    Q_EMIT VNX::ThemeManager::instance().themeChanged(
        VNX::ThemeManager::instance().snapshot());

    EXPECT_TRUE(checkBox->styleSheet().isEmpty());
    EXPECT_TRUE(label->styleSheet().isEmpty());
}

TEST(PropertyEditorEngineNotes, GripStaysClearOfVisibleScrollBars)
{
    ensureApp();
    NotesFixture fx;
    ASSERT_NE(fx.edit, nullptr);
    ASSERT_NE(fx.grip, nullptr);

    fx.edit->setFixedHeight(96);
    fx.page->resize(360, 200);
    fx.page->show();
    for (const QString text : {QString(), QString(4096, QLatin1Char('x'))}) {
        fx.edit->setPlainText(text);
        QApplication::processEvents();
        const auto* bar = fx.edit->verticalScrollBar();
        const QRect gutter(bar->mapTo(fx.edit, QPoint()), bar->size());
        EXPECT_LT(fx.grip->geometry().right(), gutter.left());
        EXPECT_LE(fx.grip->geometry().bottom(), fx.edit->contentsRect().bottom());
    }
}

TEST(PropertyEditorEngineNotes, NarrowLayoutsSettleWithoutChangingText)
{
    ensureApp();
    const QString style = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(style); });
    qApp->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    NotesFixture fx;
    ASSERT_NE(fx.edit, nullptr);
    auto* section = fx.page->findChild<VNX::CollapsibleSection*>();
    ASSERT_NE(section, nullptr);
    section->setDynamicContent(true);
    fx.page->resize(480, 240);
    fx.page->show();

    struct LayoutActivity : QObject {
        QWidget* root = nullptr;
        int events = 0;
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest
                || event->type() == QEvent::MetaCall) {
                const auto* widget = qobject_cast<QWidget*>(watched);
                if (widget && (widget == root || root->isAncestorOf(widget))) ++events;
            }
            return false;
        }
    } activity;
    activity.root = fx.page.get();
    qApp->installEventFilter(&activity);
    const auto removeFilter = qScopeGuard([&] { qApp->removeEventFilter(&activity); });
    const QStringList samples{QString(), QStringLiteral("x"), QStringLiteral("notes"),
        QString::fromUtf16(u"\u97f3\u6548\u5907\u6ce8"), QString(4096, QLatin1Char('x')),
        QStringLiteral("First line.\nSecond line.\nThird line.")};
    for (bool expanded : {true, false}) {
        SCOPED_TRACE(expanded);
        section->setExpanded(expanded);
        for (int height : {VNX::QtControlSkin::current().fieldHeight(), 96}) {
            SCOPED_TRACE(height);
            fx.edit->setFixedHeight(height);
            for (const auto& text : samples) {
                SCOPED_TRACE(text.size());
                fx.edit->setPlainText(text);
                for (int width : {320, 128, 64, 48, 32, 24, 16, 8, 4, 1, 0,
                                  1, 4, 8, 16, 24, 32, 48, 64, 128, 320}) {
                    SCOPED_TRACE(width);
                    fx.edit->setFixedWidth(width);
                    int quietPasses = 0;
                    for (int pass = 0; pass < 32 && quietPasses < 2; ++pass) {
                        activity.events = 0;
                        QApplication::processEvents();
                        quietPasses = activity.events == 0 ? quietPasses + 1 : 0;
                    }
                    ASSERT_EQ(quietPasses, 2);
                    EXPECT_EQ(fx.edit->toPlainText(), text);
                }
            }
        }
    }
}

TEST(PropertyEditorEngineNotes, SharedFieldHeightPreservesManualExpansion)
{
    ensureApp();
    NotesFixture fx;
    ASSERT_NE(fx.edit, nullptr);
    ASSERT_NE(fx.grip, nullptr);
    fx.page->resize(480, 300);
    fx.page->show();
    QApplication::processEvents();
    const int fieldHeight = VNX::QtControlSkin::current().fieldHeight();
    EXPECT_EQ(fx.edit->height(), fieldHeight);
    EXPECT_GE(fx.edit->viewport()->height(), fx.edit->fontMetrics().height());

    const auto dragGrip = [&](int delta) {
        const QPointF start = fx.grip->mapToGlobal(fx.grip->rect().center());
        sendGripMouseEvent(fx.grip, QEvent::MouseButtonPress,
                           Qt::LeftButton, Qt::LeftButton, start);
        sendGripMouseEvent(fx.grip, QEvent::MouseMove,
                           Qt::NoButton, Qt::LeftButton, start + QPointF(0, delta));
        sendGripMouseEvent(fx.grip, QEvent::MouseButtonRelease,
                           Qt::LeftButton, Qt::NoButton, start + QPointF(0, delta));
        QApplication::processEvents();
    };
    dragGrip(48);
    EXPECT_EQ(fx.edit->height(), fieldHeight + 48);
    Q_EMIT VNX::ThemeManager::instance().themeChanged(
        VNX::ThemeManager::instance().snapshot());
    QApplication::processEvents();
    EXPECT_EQ(fx.edit->height(), fieldHeight + 48);
    dragGrip(-200);
    EXPECT_EQ(fx.edit->height(), fieldHeight);
}

TEST(PropertyEditorEngineNotes, UngrabAndDeactivateCancelResize)
{
    ensureApp();
    NotesFixture fx;
    ASSERT_NE(fx.edit, nullptr);
    ASSERT_NE(fx.grip, nullptr);
    fx.edit->resize(240, 96);
    fx.edit->show();
    QApplication::processEvents();

    const QPointF startGlobal = fx.grip->mapToGlobal(fx.grip->rect().center());
    sendGripMouseEvent(fx.grip, QEvent::MouseButtonPress,
                       Qt::LeftButton, Qt::LeftButton, startGlobal);
    QEvent ungrab(QEvent::UngrabMouse);
    QApplication::sendEvent(fx.grip, &ungrab);
    const int heightAfterUngrab = fx.edit->height();
    sendGripMouseEvent(fx.grip, QEvent::MouseMove,
                       Qt::NoButton, Qt::LeftButton,
                       startGlobal + QPointF(0, 40));
    EXPECT_EQ(fx.edit->height(), heightAfterUngrab);

    sendGripMouseEvent(fx.grip, QEvent::MouseButtonPress,
                       Qt::LeftButton, Qt::LeftButton, startGlobal);
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(fx.grip, &deactivate);
    const int heightAfterDeactivate = fx.edit->height();
    sendGripMouseEvent(fx.grip, QEvent::MouseMove,
                       Qt::NoButton, Qt::LeftButton,
                       startGlobal + QPointF(0, 40));
    EXPECT_EQ(fx.edit->height(), heightAfterDeactivate);
}

TEST(PropertyEditorEngineNotes, MoveWithoutLeftButtonCancelsResize)
{
    ensureApp();
    NotesFixture fx;
    ASSERT_NE(fx.edit, nullptr);
    ASSERT_NE(fx.grip, nullptr);
    fx.edit->resize(240, 96);
    fx.edit->show();
    QApplication::processEvents();

    const QPointF startGlobal = fx.grip->mapToGlobal(fx.grip->rect().center());
    sendGripMouseEvent(fx.grip, QEvent::MouseButtonPress,
                       Qt::LeftButton, Qt::LeftButton, startGlobal);
    const int startHeight = fx.edit->height();
    sendGripMouseEvent(fx.grip, QEvent::MouseMove,
                       Qt::NoButton, Qt::RightButton,
                       startGlobal + QPointF(0, 40));
    EXPECT_EQ(fx.edit->height(), startHeight);

    sendGripMouseEvent(fx.grip, QEvent::MouseMove,
                       Qt::NoButton, Qt::LeftButton,
                       startGlobal + QPointF(0, 40));
    EXPECT_EQ(fx.edit->height(), startHeight);
}
TEST(PropertyEditorEngineSession, DragPreviewsModelWithoutCommitSignals)
{
    ensureApp();
    auto item = makeAudioItem();
    EngineFixture fx(filterSchema(VNX::VMixNode::schema(), QStringLiteral("volume")),
                     VNX::ComponentType::MixNode, VNX::ProjectItem::Type::Sound);
    ASSERT_NE(fx.rawSlider, nullptr);
    fx.engine.bindItem(item.get());

    FieldChangeRecorder rec(fx.engine);
    SignalCounters counters;
    const int undoBefore = undoStackCount();
    const float startVolume = item->comp<VNX::VMixNode>()->volume;

    fx.rawSlider->setSliderDown(true);
    fx.rawSlider->setValue(500);
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume,
                    static_cast<float>(fx.slider->value()));
    fx.rawSlider->setValue(750);
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume,
                    static_cast<float>(fx.slider->value()));

    EXPECT_TRUE(rec.changes.empty());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);
    EXPECT_FALSE(item->isDirtyObject());

    fx.rawSlider->setSliderDown(false);

    EXPECT_TRUE(rec.changes.empty());
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, startVolume);
}

TEST(PropertyEditorEngineSession, EmptyDragIsSilentlyDropped)
{
    ensureApp();
    auto item = makeAudioItem();
    EngineFixture fx(filterSchema(VNX::VMixNode::schema(), QStringLiteral("volume")),
                     VNX::ComponentType::MixNode, VNX::ProjectItem::Type::Sound);
    ASSERT_NE(fx.rawSlider, nullptr);
    fx.engine.bindItem(item.get());

    FieldChangeRecorder rec(fx.engine);
    SignalCounters counters;
    const int undoBefore = undoStackCount();

    fx.rawSlider->setSliderDown(true);
    fx.rawSlider->setSliderDown(false);

    EXPECT_TRUE(rec.changes.empty());
    EXPECT_EQ(counters.itemPropertyChanged, 0);
    EXPECT_EQ(undoStackCount(), undoBefore);
    EXPECT_FALSE(item->isDirtyObject());
}

TEST(PropertyEditorEngineSession, SettingTheSpinBoxValueReportsOneFieldChange)
{
    ensureApp();
    auto item = makeAudioItem();
    item->comp<VNX::VMixNode>()->volume = -12.0f;
    EngineFixture fx(filterSchema(VNX::VMixNode::schema(), QStringLiteral("volume")),
                     VNX::ComponentType::MixNode, VNX::ProjectItem::Type::Sound);
    ASSERT_NE(fx.slider, nullptr);
    fx.engine.bindItem(item.get());

    FieldChangeRecorder rec(fx.engine);
    auto* spinBox = fx.slider->findChild<QDoubleSpinBox*>();
    ASSERT_NE(spinBox, nullptr);

    spinBox->setValue(-6.0);

    ASSERT_EQ(rec.changes.size(), 1u);
    EXPECT_EQ(std::get<0>(rec.changes[0]), QStringLiteral("volume"));
    EXPECT_FLOAT_EQ(std::get<1>(rec.changes[0]).toFloat(), -12.0f);
    EXPECT_DOUBLE_EQ(std::get<2>(rec.changes[0]).toDouble(), -6.0);
}

TEST(PropertyEditorEngineSession, TypedIntDigitsReportOneChangeWhenAccepted)
{
    ensureApp();
    auto item = makeAudioItem();
    item->comp<VNX::VAudio>()->priority = 50;
    EngineFixture fx(filterSchema(VNX::VAudio::schema(), QStringLiteral("priority")),
                     VNX::ComponentType::Audio, VNX::ProjectItem::Type::Sound);
    fx.engine.bindItem(item.get());
    auto* spin = fx.page->findChild<QSpinBox*>();
    ASSERT_NE(spin, nullptr);
    ASSERT_FALSE(spin->keyboardTracking());

    FieldChangeRecorder rec(fx.engine);
    fx.page->show();
    spin->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    spin->selectAll();
    typeDigits(spin, QStringLiteral("72"));

    EXPECT_TRUE(rec.changes.empty());

    QKeyEvent accept(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                     QStringLiteral("\r"));
    QApplication::sendEvent(spin, &accept);

    ASSERT_EQ(rec.changes.size(), 1u);
    EXPECT_EQ(std::get<0>(rec.changes[0]), QStringLiteral("priority"));
    EXPECT_EQ(std::get<2>(rec.changes[0]).toInt(), 72);
}

TEST(PropertyEditorEngineSession, BindItemDuringSessionCancelsPreview)
{
    ensureApp();
    auto item = makeAudioItem();
    item->comp<VNX::VMixNode>()->volume = -12.0f;
    EngineFixture fx(filterSchema(VNX::VMixNode::schema(), QStringLiteral("volume")),
                     VNX::ComponentType::MixNode, VNX::ProjectItem::Type::Sound);
    ASSERT_NE(fx.rawSlider, nullptr);
    fx.engine.bindItem(item.get());

    FieldChangeRecorder rec(fx.engine);

    fx.rawSlider->setSliderDown(true);
    fx.rawSlider->setValue(750);
    EXPECT_NE(item->comp<VNX::VMixNode>()->volume, -12.0f);

    fx.engine.bindItem(item.get());

    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, -12.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(fx.slider->value()), -12.0f);

    fx.rawSlider->setSliderDown(false);

    EXPECT_TRUE(rec.changes.empty());
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, -12.0f);
}

TEST(PropertyEditorEngineSession, BusVolumeSessionPreviewsDb)
{
    ensureApp();
    auto bus = makeBusItem();
    bus->comp<VNX::VMixNode>()->volume = 0.0f;
    EngineFixture fx(filterSchema(VNX::VMixNode::schema(), QStringLiteral("volume")),
                     VNX::ComponentType::Bus, VNX::ProjectItem::Type::Bus);
    ASSERT_NE(fx.rawSlider, nullptr);
    fx.engine.bindItem(bus.get());

    FieldChangeRecorder rec(fx.engine);
    SignalCounters counters;

    fx.rawSlider->setSliderDown(true);
    fx.rawSlider->setValue(700);

    const float db = static_cast<float>(fx.slider->value());
    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->volume, db);
    EXPECT_TRUE(rec.changes.empty());
    EXPECT_EQ(counters.busChanged, 0);
    EXPECT_FALSE(bus->isDirtyObject());

    fx.rawSlider->setSliderDown(false);

    EXPECT_TRUE(rec.changes.empty());
    EXPECT_FLOAT_EQ(bus->comp<VNX::VMixNode>()->volume, 0.0f);
}

TEST(PropertyEditorEngineSession, RefreshFieldDuringDragKeepsSessionAlive)
{
    ensureApp();
    auto item = makeAudioItem();
    item->comp<VNX::VMixNode>()->volume = -12.0f;
    EngineFixture fx(filterSchema(VNX::VMixNode::schema(), QStringLiteral("volume")),
                     VNX::ComponentType::MixNode, VNX::ProjectItem::Type::Sound);
    ASSERT_NE(fx.rawSlider, nullptr);
    fx.engine.bindItem(item.get());

    FieldChangeRecorder rec(fx.engine);

    fx.rawSlider->setSliderDown(true);
    fx.rawSlider->setValue(750);
    const double draggedValue = fx.slider->value();
    const float previewVolume = item->comp<VNX::VMixNode>()->volume;

    EXPECT_TRUE(fx.engine.refreshField(QStringLiteral("volume")));

    EXPECT_DOUBLE_EQ(fx.slider->value(), draggedValue);
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume, previewVolume);

    fx.rawSlider->setValue(400);
    EXPECT_FLOAT_EQ(item->comp<VNX::VMixNode>()->volume,
                    static_cast<float>(fx.slider->value()));
    EXPECT_TRUE(rec.changes.empty());

    fx.rawSlider->setSliderDown(false);
    EXPECT_TRUE(rec.changes.empty());
}

TEST(PropertyEditorEngineSession, RefreshFieldReloadsWidgetFromModel)
{
    ensureApp();
    auto item = makeAudioItem();
    item->comp<VNX::VMixNode>()->volume = -12.0f;
    EngineFixture fx(filterSchema(VNX::VMixNode::schema(), QStringLiteral("volume")),
                     VNX::ComponentType::MixNode, VNX::ProjectItem::Type::Sound);
    ASSERT_NE(fx.slider, nullptr);
    fx.engine.bindItem(item.get());

    FieldChangeRecorder rec(fx.engine);
    item->comp<VNX::VMixNode>()->volume = -3.0f;
    fx.engine.refreshField(QStringLiteral("volume"));

    EXPECT_FLOAT_EQ(static_cast<float>(fx.slider->value()), -3.0f);
    EXPECT_TRUE(rec.changes.empty());
}

class PropertyEditorEngineCommitTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &projectManager, nullptr, {});
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("ParameterBindingProject");
        config.location = tempDir.path();
        ASSERT_TRUE(projectManager.createProject(config));
        item = projectManager.createItem(nullptr, QStringLiteral("ParameterBindingSound"),
                                         VNX::ProjectItem::Type::Sound);
        ASSERT_NE(item, nullptr);
        VNX::UndoRedoManager::instance().clear();
        VNX::PlatformContextService::instance().setAll(true);
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir;
    VNX::ProjectItem* item = nullptr;
};

TEST_F(PropertyEditorEngineCommitTest, VolumeDragCommitsOneUndoStepAndRoundtrips)
{
    auto* mix = item->comp<VNX::VMixNode>();
    ASSERT_NE(mix, nullptr);
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*item);
    const auto* volume = VNX::PlayablePropertyAuthoringV1::fieldByName(
        QStringLiteral("volume"));
    ASSERT_TRUE(locator.has_value());
    ASSERT_NE(volume, nullptr);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *item, *locator, volume->id, -12.0, std::nullopt));
    EngineFixture fx(filterSchema(VNX::VMixNode::schema(), QStringLiteral("volume")),
                     VNX::ComponentType::MixNode, VNX::ProjectItem::Type::Sound);
    ASSERT_NE(fx.rawSlider, nullptr);
    fx.engine.bindItem(item);
    const int undoBefore = undoStackCount();

    fx.rawSlider->setSliderDown(true);
    fx.rawSlider->setValue(500);
    fx.rawSlider->setValue(750);
    const float committedValue = static_cast<float>(fx.slider->value());
    fx.rawSlider->setSliderDown(false);

    EXPECT_FLOAT_EQ(mix->volume, committedValue);
    EXPECT_EQ(undoStackCount(), undoBefore + 1);

    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FLOAT_EQ(mix->volume, -12.0f);
}

TEST_F(PropertyEditorEngineCommitTest, DistanceScalePercentEditingPreservesRatioAndUndo)
{
    auto* spatial = item->comp<VNX::VSpatial>();
    ASSERT_NE(spatial, nullptr);
    auto groups = item->fieldGroups;
    groups.spatialRating.shared = {VNX::FieldGroupMode::Local, VNX::SpatialRatingFieldGroup{}};
    ASSERT_TRUE(VNX::ProjectManager::instance().restoreFieldGroupsInternal(item, groups));
    const QString field = QStringLiteral("spatialAttenuationDistanceScaling");
    EngineFixture fx(filterSchema(
        VNX::contentObjectInspectorRows(VNX::ProjectItem::Type::Sound), field),
        VNX::ComponentType::Audio, VNX::ProjectItem::Type::Sound);
    ASSERT_NE(fx.slider, nullptr);
    fx.engine.bindItem(item);
    EXPECT_TRUE(fx.slider->isEnabled());

    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    for (const auto presenter : {VNX::ParameterPresenterId::SliderWithValue,
                                 VNX::ParameterPresenterId::VerticalFader,
                                 VNX::ParameterPresenterId::RotaryKnob}) {
        fx.slider->setPresenterOverride(presenter);
        auto* editor = fx.slider->presenter()->valueEditor();
        ASSERT_NE(editor, nullptr);
        EXPECT_EQ(editor->text(), QStringLiteral("100"));
        EXPECT_TRUE(editor->toolTip().endsWith(QLatin1Char('%')));
        editor->setValue(250.0);
        EXPECT_FLOAT_EQ(spatial->attenuationDistanceScaling, 2.5f);
        EXPECT_EQ(undo->index(), 1);
        std::string serialized;
        std::string error;
        ASSERT_TRUE(VNX::WriteFieldGroupsJson(item->fieldGroups, serialized, error)) << error;
        const QJsonObject saved = QJsonDocument::fromJson(
            QByteArray::fromStdString(serialized)).object();
        EXPECT_DOUBLE_EQ(saved.value(QStringLiteral("shared")).toObject()
            .value(QStringLiteral("spatialRating")).toObject()
            .value(QStringLiteral("values")).toObject()
            .value(QStringLiteral("spatialAttenuationDistanceScaling")).toDouble(), 2.5);
        undo->undo();
        EXPECT_FLOAT_EQ(spatial->attenuationDistanceScaling, 1.0f);
        fx.engine.refreshField(field);
        EXPECT_EQ(editor->text(), QStringLiteral("100"));
        undo->redo();
        EXPECT_FLOAT_EQ(spatial->attenuationDistanceScaling, 2.5f);
        undo->undo();
        fx.engine.refreshField(field);
    }

    auto* binding = fx.slider->binding();
    binding->beginEdit();
    binding->previewEdit(1.5);
    EXPECT_FLOAT_EQ(spatial->attenuationDistanceScaling, 1.0f);
    ASSERT_TRUE(item->fieldGroups.spatialRating.shared.localHistory.has_value());
    EXPECT_FLOAT_EQ(item->fieldGroups.spatialRating.shared.localHistory
        ->spatialAttenuationDistanceScaling, 1.0f);
    EXPECT_EQ(fx.slider->presenter()->valueEditor()->text(), QStringLiteral("150"));
    binding->cancelEdit();
    EXPECT_FLOAT_EQ(spatial->attenuationDistanceScaling, 1.0f);
    EXPECT_EQ(fx.slider->presenter()->valueEditor()->text(), QStringLiteral("100"));
    EXPECT_EQ(undo->index(), 0);
}

namespace VNX {

class PropertyEditorEnginePlatformContextSessionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        PlatformContextService::instance().setStorage(
            std::make_unique<MemorySettingsStorage>());
        auto& projectManager = ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &projectManager, nullptr, {});
        ASSERT_TRUE(tempDir.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("PlatformContextSession");
        config.location = tempDir.path();
        config.platforms = HarkenTest::platforms(QStringList{
            QStringLiteral("Windows"), QStringLiteral("Android")});
        ASSERT_TRUE(projectManager.createProject(config));
        item = projectManager.createItem(nullptr, QStringLiteral("PlatformContextSound"),
                                         ProjectItem::Type::Sound);
        ASSERT_NE(item, nullptr);
        UndoRedoManager::instance().clear();
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Windows, true);
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        auto& projectManager = ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        ActionManager::instance().clearAllForTesting();
        PlatformContextService::instance().setStorage(
            SettingsStorageFactory::create(
                SettingsStorageFactory::StorageType::QSettings,
                QStringLiteral("HarkenSound"),
                QStringLiteral("HarkenStudio")));
    }

    QTemporaryDir tempDir;
    ProjectItem* item = nullptr;
};

TEST_F(PropertyEditorEnginePlatformContextSessionTest,
       SpatializationMixShowsInheritedValueAndEditsSharedInPlatformView)
{
    auto& pm = ProjectManager::instance();
    auto* parent = pm.createItem(nullptr, QStringLiteral("MixParent"), ProjectItem::Type::Layer);
    ASSERT_NE(parent, nullptr);
    ASSERT_TRUE(pm.moveItem(item, parent));
    parent->comp<VSpatial>()->spatializationMix = 0.5f;
    item->comp<VSpatial>()->spatializationMixPresent = false;
    const auto groups = item->fieldGroups;
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EngineFixture fx(filterSchema(contentObjectInspectorRows(ProjectItem::Type::Sound),
                                  QStringLiteral("spatializationMix")),
                     ComponentType::Spatial, ProjectItem::Type::Sound);
    ASSERT_NE(fx.slider, nullptr);
    ASSERT_NE(fx.rawSlider, nullptr);
    fx.engine.bindItem(item);
    EXPECT_DOUBLE_EQ(fx.slider->value(), 0.5);
    EXPECT_TRUE(fx.slider->isEnabled());
    EXPECT_TRUE(fx.engine.playablePropertyEventRelated(parent, QStringLiteral("spatializationMix")));
    parent->comp<VSpatial>()->spatializationMix = 0.25f;
    ASSERT_TRUE(fx.engine.refreshField(QStringLiteral("spatializationMix")));
    EXPECT_DOUBLE_EQ(fx.slider->value(), 0.25);
    parent->comp<VSpatial>()->spatializationMix = 0.5f;
    ASSERT_TRUE(fx.engine.refreshField(QStringLiteral("spatializationMix")));
    UndoRedoManager::instance().clear();
    fx.rawSlider->setSliderDown(true);
    fx.rawSlider->setValue(750);
    const auto value = static_cast<float>(fx.slider->value());
    fx.rawSlider->setSliderDown(false);
    EXPECT_TRUE(item->comp<VSpatial>()->spatializationMixPresent);
    EXPECT_FLOAT_EQ(item->comp<VSpatial>()->spatializationMix, value);
    EXPECT_TRUE(item->platformOverrides.rows.empty());
    EXPECT_EQ(undoStackCount(), 1);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FALSE(item->comp<VSpatial>()->spatializationMixPresent);
    ASSERT_TRUE(fx.engine.refreshField(QStringLiteral("spatializationMix")));
    EXPECT_DOUBLE_EQ(fx.slider->value(), 0.5);
    EXPECT_EQ(item->fieldGroups, groups);
}

TEST_F(PropertyEditorEnginePlatformContextSessionTest,
       DistanceScaleShowsMixedOrInstanceValuesWithoutAFieldOverrideIndicator)
{
    registerBuiltinInspectorCustomizations();
    const QString field = QStringLiteral("spatialAttenuationDistanceScaling");
    auto groups = item->fieldGroups;
    SpatialRatingFieldGroup windows;
    windows.spatialAttenuationDistanceScaling = 2.5f;
    groups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {FieldGroupMode::Local, windows};
    groups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, SpatialRatingFieldGroup{}};
    ASSERT_TRUE(ProjectManager::instance().restoreFieldGroupsInternal(item, groups));

    EngineFixture fx(filterSchema(
        contentObjectInspectorRows(ProjectItem::Type::Sound), field),
        ComponentType::Audio, ProjectItem::Type::Sound);
    ASSERT_NE(fx.slider, nullptr);
    fx.engine.bindItem(item);
    EXPECT_TRUE(fx.slider->mixedValue());
    EXPECT_TRUE(fx.slider->isEnabled());
    EXPECT_TRUE(fx.slider->presenter()->valueEditor()->text().isEmpty());
    EXPECT_EQ(fx.page->findChild<QWidget*>(QStringLiteral("platform_") + field), nullptr);

    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    EXPECT_FALSE(fx.slider->mixedValue());
    EXPECT_DOUBLE_EQ(fx.slider->value(), 2.5);
    EXPECT_EQ(fx.slider->presenter()->valueEditor()->text(), QStringLiteral("250"));
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    EXPECT_DOUBLE_EQ(fx.slider->value(), 1.0);
    EXPECT_EQ(fx.slider->presenter()->valueEditor()->text(), QStringLiteral("100"));
}

TEST_F(PropertyEditorEnginePlatformContextSessionTest,
       ChangingSelectedPlatformsDuringVolumeDragCommitsOriginalScope)
{
    auto* mix = item->comp<VMixNode>();
    ASSERT_NE(mix, nullptr);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
    const auto* volume = PlayablePropertyAuthoringV1::fieldByName(
        QStringLiteral("volume"));
    ASSERT_TRUE(locator.has_value());
    ASSERT_NE(volume, nullptr);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *item, *locator, volume->id, -12.0, std::nullopt));
    mix->volume = -12.0f;

    EngineFixture fx(filterSchema(VMixNode::schema(), QStringLiteral("volume")),
                     ComponentType::MixNode, ProjectItem::Type::Sound);
    ASSERT_NE(fx.rawSlider, nullptr);
    fx.engine.bindItem(item);
    const int undoBefore = undoStackCount();

    fx.rawSlider->setSliderDown(true);
    fx.rawSlider->setValue(750);
    EXPECT_NE(mix->volume, -12.0f);
    EXPECT_TRUE(fx.slider->binding()->isEditing());
    const float committedVolume = mix->volume;

    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);

    EXPECT_FALSE(fx.slider->binding()->isEditing());
    EXPECT_FLOAT_EQ(mix->volume, committedVolume);
    EXPECT_EQ(undoStackCount(), undoBefore + 1);

    fx.rawSlider->setSliderDown(false);
    EXPECT_FLOAT_EQ(mix->volume, committedVolume);
    EXPECT_EQ(undoStackCount(), undoBefore + 1);
}

TEST_F(PropertyEditorEnginePlatformContextSessionTest,
       LoopCountCanCommitOneAndRoundTripUndo)
{
    const QString field = QStringLiteral("loopCount");
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(field);
    ASSERT_TRUE(locator.has_value());
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *item, *locator, property->id, 2, std::nullopt));
    auto* audio = item->comp<VAudio>();
    ASSERT_NE(audio, nullptr);

    ValueSegmentedControl::clearSessionMemory();
    PropertyEditorEngine engine;
    engine.setSchema(contentObjectInspectorRows(ProjectItem::Type::Sound),
                     ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    auto* loop = page->findChild<ValueSegmentedControl*>(QStringLiteral("prop_loopCount"));
    ASSERT_NE(loop, nullptr);
    engine.bindItem(item);

    QToolButton* finite = nullptr;
    for (auto* button : loop->findChildren<QToolButton*>())
        if (button->property("segmentPosition").toString() == QLatin1String("first"))
            finite = button;
    ASSERT_NE(finite, nullptr);
    EXPECT_EQ(finite->text(), QStringLiteral("2"));
    auto* editor = loop->findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);

    const int undoBefore = undoStackCount();
    QMouseEvent open(QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(finite, &open);
    editor->setText(QString());
    editor->insert(QStringLiteral("1"));
    EXPECT_EQ(audio->loopCount, 1u);
    QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &commit);
    EXPECT_FALSE(loop->isEditing());
    engine.refreshField(field);
    EXPECT_TRUE(finite->isChecked());
    EXPECT_EQ(finite->text(), QStringLiteral("1"));
    EXPECT_EQ(audio->toJson().value(field).toInt(), 1);
    EXPECT_EQ(undoStackCount(), undoBefore + 1);

    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(audio->loopCount, 2u);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(audio->loopCount, 1u);
    engine.refreshField(field);
    EXPECT_TRUE(finite->isChecked());
    EXPECT_EQ(finite->text(), QStringLiteral("1"));
}

TEST_F(PropertyEditorEnginePlatformContextSessionTest,
       ChangingSelectedPlatformsDuringLoopCountEditCommitsAndClosesEditor)
{
    auto* audio = item->comp<VAudio>();
    ASSERT_NE(audio, nullptr);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
    const auto* loopCount = PlayablePropertyAuthoringV1::fieldByName(
        QStringLiteral("loopCount"));
    ASSERT_TRUE(locator.has_value());
    ASSERT_NE(loopCount, nullptr);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(
        *item, *locator, loopCount->id, 7, std::nullopt));
    audio->loopCount = 7;

    ValueSegmentedControl::clearSessionMemory();
    PropertyEditorEngine engine;
    engine.setSchema(contentObjectInspectorRows(ProjectItem::Type::Sound),
                     ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    auto* loop = page->findChild<ValueSegmentedControl*>(
        QStringLiteral("prop_loopCount"));
    ASSERT_NE(loop, nullptr);
    engine.bindItem(item);

    QToolButton* finite = nullptr;
    for (auto* button : loop->findChildren<QToolButton*>()) {
        if (button->property("segmentPosition").toString() == QLatin1String("first"))
            finite = button;
    }
    ASSERT_NE(finite, nullptr);
    auto* editor = loop->findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);

    const int undoBefore = undoStackCount();
    QMouseEvent open(QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(finite, &open);
    ASSERT_TRUE(loop->isEditing());

    editor->setText(QString());
    editor->insert(QStringLiteral("99"));
    EXPECT_EQ(audio->loopCount, 99u);

    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);

    EXPECT_FALSE(loop->isEditing());
    EXPECT_EQ(audio->loopCount, 99u);
    EXPECT_EQ(undoStackCount(), undoBefore + 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(audio->loopCount, 7u);
}

} // namespace VNX

TEST_F(PropertyEditorEngineCommitTest, LayerSpatialRowsReadTheOwnershipGroupAndPreserveDisplayRanges)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* parent = manager.createItem(nullptr, QStringLiteral("ParentLayer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(parent, nullptr);
    auto* child = manager.createItem(parent, QStringLiteral("ChildLayer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(child, nullptr);
    auto groups = parent->fieldGroups;
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialIs3DEnabled = true;
    rating.spatialPanning = 0.25f;
    rating.spatialAttenuationDistanceScaling = 2.5f;
    groups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(parent, groups));

    EngineFixture fx(VNX::layerInspectorSchema(), VNX::ComponentType::MixNode,
                     VNX::ProjectItem::Type::Layer);
    fx.engine.bindItem(child);

    auto* is3d = fx.page->findChild<QCheckBox*>(
        QStringLiteral("prop_spatialIs3DEnabled"));
    ASSERT_NE(is3d, nullptr);
    EXPECT_TRUE(is3d->isChecked());
    EXPECT_FALSE(is3d->isEnabled());

    auto* mix = fx.page->findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_spatialPanning"));
    ASSERT_NE(mix, nullptr);
    EXPECT_FLOAT_EQ(static_cast<float>(mix->value()), 0.25f);
    EXPECT_FALSE(mix->isEnabled());
    EXPECT_DOUBLE_EQ(mix->minimum(), -1.0);
    EXPECT_DOUBLE_EQ(mix->maximum(), 1.0);

    auto* dist = fx.page->findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_spatialAttenuationDistanceScaling"));
    ASSERT_NE(dist, nullptr);
    EXPECT_FLOAT_EQ(static_cast<float>(dist->value()), 2.5f);
    EXPECT_FALSE(dist->isEnabled());
    EXPECT_EQ(dist->binding()->name(), QStringLiteral("Distance Scale"));
    auto* editor = dist->presenter()->valueEditor();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->text(), QStringLiteral("250"));
    EXPECT_TRUE(editor->toolTip().endsWith(QLatin1Char('%')));
    EXPECT_DOUBLE_EQ(editor->minimum(), 10.0);
    EXPECT_DOUBLE_EQ(editor->maximum(), 1000.0);
    VNX::CollapsibleSection* section = nullptr;
    for (QWidget* parent = dist->parentWidget(); parent; parent = parent->parentWidget()) {
        section = qobject_cast<VNX::CollapsibleSection*>(parent);
        if (section) break;
    }
    ASSERT_NE(section, nullptr);
    auto* inherit = section->findChild<QCheckBox*>(QStringLiteral("inherit_spatialRating"));
    ASSERT_NE(inherit, nullptr);
    EXPECT_TRUE(inherit->isChecked());
    EXPECT_EQ(fx.page->findChild<QWidget*>(QStringLiteral("prop_spatialSpread")), nullptr);
}

class PropertyEditorEngineBusSourceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::registerBuiltinInspectorCustomizations();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &projectManager, nullptr, {});
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("BusLocalMixProject");
        config.location = tempDir.path();
        ASSERT_TRUE(projectManager.createProject(config));
        VNX::ProjectItem* master = projectManager.getMasterBusItem();
        ASSERT_NE(master, nullptr);
        parentBus = projectManager.createItem(
            master, QStringLiteral("ParentBus"), VNX::ProjectItem::Type::Bus);
        ASSERT_NE(parentBus, nullptr);
        childBus = projectManager.createItem(
            parentBus, QStringLiteral("ChildBus"), VNX::ProjectItem::Type::Bus);
        ASSERT_NE(childBus, nullptr);
        VNX::UndoRedoManager::instance().clear();
        VNX::PlatformContextService::instance().setAll(true);
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& projectManager = VNX::ProjectManager::instance();
        if (projectManager.isProjectOpen())
            projectManager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir;
    VNX::ProjectItem* parentBus = nullptr;
    VNX::ProjectItem* childBus = nullptr;
};

TEST_F(PropertyEditorEngineBusSourceTest, LocalVolumeAndPanStayEditableWithoutSourceControls)
{
    EngineFixture fx(busVolumeAndPanSchema(), VNX::ComponentType::Bus,
                     VNX::ProjectItem::Type::Bus);
    fx.engine.bindItem(childBus);

    EXPECT_EQ(fx.page->findChild<QLabel*>(QStringLiteral("source_volume")), nullptr);
    EXPECT_EQ(fx.page->findChild<QLabel*>(QStringLiteral("source_pan")), nullptr);
    EXPECT_EQ(fx.page->findChild<QCheckBox*>(QStringLiteral("inherit_mainOutput")), nullptr);
    auto* volumeSlider = fx.page->findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_volume"));
    auto* panSlider = fx.page->findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_pan"));
    ASSERT_NE(volumeSlider, nullptr);
    ASSERT_NE(panSlider, nullptr);
    EXPECT_TRUE(volumeSlider->isEnabled());
    EXPECT_TRUE(panSlider->isEnabled());

    const auto parentLocator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*parentBus);
    const auto childLocator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*childBus);
    ASSERT_TRUE(parentLocator.has_value());
    ASSERT_TRUE(childLocator.has_value());
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *parentBus, *parentLocator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Volume),
        -12.0, std::nullopt));
    fx.engine.bindItem(childBus);
    EXPECT_FLOAT_EQ(static_cast<float>(volumeSlider->value()), 0.0f);

    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *childBus, *childLocator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Volume),
        -6.0, std::nullopt));
    fx.engine.bindItem(childBus);
    EXPECT_FLOAT_EQ(static_cast<float>(volumeSlider->value()), -6.0f);

    QObject::connect(&fx.engine, &VNX::PropertyEditorEngine::fieldChanged,
        &fx.engine, [&](const QString& name, const QVariant&, const QVariant& value) {
            const auto result = VNX::ActionManager::instance().execute(
                QStringLiteral("set_item_property"),
                fx.engine.buildSetItemPropertyArgs(name, value),
                VNX::ActionSource::Internal, &fx.engine);
            EXPECT_TRUE(result.success) << result.errorMessage.toStdString();
        });
    volumeSlider->simulateValueChange(-9.0);
    panSlider->simulateValueChange(0.25);
    EXPECT_FLOAT_EQ(childBus->comp<VNX::VMixNode>()->volume, -9.0f);
    EXPECT_FLOAT_EQ(childBus->comp<VNX::VBus>()->pan, 0.25f);
    EXPECT_FLOAT_EQ(parentBus->comp<VNX::VMixNode>()->volume, -12.0f);

    auto* label = fx.page->findChild<QLabel*>(QStringLiteral("label_volume"));
    ASSERT_NE(label, nullptr);
    const int undoBeforeLabelClick = undoStackCount();
    clickLeft(label);

    EXPECT_TRUE(childBus->playablePropertyPresence.Has(
        harkenplayableproperties::PlayablePropertyIdV1::Volume));
    EXPECT_FLOAT_EQ(childBus->comp<VNX::VMixNode>()->volume, -9.0f);
    EXPECT_EQ(undoStackCount(), undoBeforeLabelClick);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->undo();
    EXPECT_FLOAT_EQ(childBus->comp<VNX::VBus>()->pan, 0.0f);
    undo->undo();
    EXPECT_FLOAT_EQ(childBus->comp<VNX::VMixNode>()->volume, -6.0f);
    fx.engine.refreshField(QStringLiteral("volume"));
    EXPECT_FLOAT_EQ(static_cast<float>(volumeSlider->value()), -6.0f);
}
