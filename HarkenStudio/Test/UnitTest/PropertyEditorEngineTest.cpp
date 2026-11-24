#include "Core/ActionManager.h"
#include "Core/ConversionEditing.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include <QCheckBox>
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/ProjectItemInspectorRows.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/TableWidget.h"
#include "Core/Components/VMixNode.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "ProjectTypes.h"
#include "QtControlSkin.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QToolButton>

#include <algorithm>
#include <memory>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_studio_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

struct NotesFixture
{
    harken::studio::PropertyEditorEngine engine;
    std::unique_ptr<QWidget> page;
    QPlainTextEdit* edit = nullptr;

    NotesFixture()
    {
        engine.setSchema({harken::studio::notesInspectorDescriptor()}, harken::studio::ComponentType::_Count);
        engine.setItemType(harken::studio::ProjectItem::Type::Sound);
        page.reset(engine.buildPage());
        edit = page->findChild<QPlainTextEdit*>(QStringLiteral("prop_notes"));
    }
};

}

TEST(PropertyEditorEngineNotes, NarrowLayoutsSettleWithoutChangingText)
{
    ensureApp();
    const QString style = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(style); });
    qApp->setStyleSheet(harken::studio::QtControlSkin::current().applicationStyleSheet());
    NotesFixture fx;
    ASSERT_NE(fx.edit, nullptr);
    auto* section = fx.page->findChild<harken::studio::CollapsibleSection*>();
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
        for (int height : {harken::studio::QtControlSkin::current().fieldHeight(), 96}) {
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

TEST(PropertyEditorEnginePlatformContext, SwitchingMatchesFreshBindingWithoutAuthoringChanges)
{
    using namespace harken::studio;
    ensureApp();
    auto& manager = ProjectManager::instance();
    auto& platforms = PlatformContextService::instance();
    auto& undo = UndoRedoManager::instance();
    if (manager.isProjectOpen()) manager.closeProject();
    platforms.setStorageForTesting(std::make_unique<MemorySettingsStorage>());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto cleanup = qScopeGuard([&] {
        undo.clear();
        if (manager.isProjectOpen()) manager.closeProject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    ProjectConfig config;
    config.name = QStringLiteral("InspectorPlatformRefresh");
    config.location = directory.path();
    config.platforms = HarkenTest::platforms({"Windows", "Android"});
    ASSERT_TRUE(manager.createProject(config));
    auto* item = manager.createItem(nullptr, "Sound", ProjectItem::Type::Sound);
    ASSERT_NE(item, nullptr);
    const auto windows = HarkenTest::PlatformIds::Windows;
    const auto android = HarkenTest::PlatformIds::Android;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
    const auto* pitch = PlayablePropertyAuthoringV1::fieldByName("pitch");
    ASSERT_TRUE(locator);
    ASSERT_NE(pitch, nullptr);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*item, *locator, pitch->id, 120.0, windows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*item, *locator, pitch->id, 240.0, android));
    undo.clear();
    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(*manager.currentProject()));
    const auto before = serializer.projectItemToJson(*item);
    PropertySchema schema;
    for (const auto& descriptor : VMixNode::schema()) {
        if (descriptor.field == "pitch")
            schema.push_back(descriptor);
    }
    auto bindPage = [&](PropertyEditorEngine& engine) {
        engine.setItemType(item->type);
        engine.setSchema(schema, ComponentType::MixNode);
        std::unique_ptr<QWidget> page(engine.buildPage());
        engine.bindItem(item);
        return page;
    };
    platforms.setSelected({windows}, windows, false);
    PropertyEditorEngine engine;
    auto page = bindPage(engine);
    int changes = 0;
    QObject::connect(&engine, &PropertyEditorEngine::fieldChanged,
                     &engine, [&] { ++changes; });
    const QList<PlatformContext> contexts{
        {{android}, android, false},
        {{windows, android}, android, true},
        {{windows}, windows, false},
        {{windows, android}, windows, true},
        {{android}, android, false}
    };
    for (const auto& context : contexts) {
        SCOPED_TRACE(context.primary.toString().toStdString());
        SCOPED_TRACE(context.all);
        platforms.setSelected(context.selected, context.primary, context.all);
        PropertyEditorEngine reference;
        auto referencePage = bindPage(reference);
        const auto* actual = page->findChild<ParameterSlider*>("prop_pitch");
        const auto* expected = referencePage->findChild<ParameterSlider*>("prop_pitch");
        ASSERT_NE(actual, nullptr);
        ASSERT_NE(expected, nullptr);
        EXPECT_EQ(actual->mixedValue(), expected->mixedValue());
        if (!expected->mixedValue()) EXPECT_EQ(actual->value(), expected->value());
        EXPECT_EQ(actual->toolTip(), expected->toolTip());
        EXPECT_EQ(serializer.projectItemToJson(*item), before);
        EXPECT_FALSE(undo.canUndo());
        EXPECT_EQ(changes, 0);
    }
}

TEST(PropertyEditorEngineRouting, FilteredTablesPreservePeerRoutesAndUndo)
{
    using namespace harken::studio;
    ensureApp();
    registerBuiltinInspectorCustomizations();
    auto& manager = ProjectManager::instance();
    auto& history = UndoRedoManager::instance();
    ActionManager::instance().clearAllForTesting();
    registerProjectActions(ActionManager::instance(), &manager, nullptr);
    QTemporaryDir directory;
    ProjectConfig config;
    config.name = QStringLiteral("FilteredRoutes");
    config.location = directory.path();
    ASSERT_TRUE(manager.createProject(config));
    const auto close = qScopeGuard([&] {
        history.clear();
        manager.closeProject();
        ActionManager::instance().clearAllForTesting();
    });
    auto* sound = manager.createItem(nullptr, QStringLiteral("Source"), ProjectItem::Type::Sound);
    auto* target = manager.createItem(manager.getMasterBusItem(), QStringLiteral("A"), ProjectItem::Type::Bus);
    auto* other = manager.createItem(manager.getMasterBusItem(), QStringLiteral("B"), ProjectItem::Type::Bus);
    ASSERT_TRUE(sound && target && other);
    const SendFieldGroupRow send{toFieldGroupGuid(target->guid), -1.0f, SendType::Send, false};
    const SendFieldGroupRow sidechain{toFieldGroupGuid(target->guid), -6.0f, SendType::Sidechain, false};
    const SendFieldGroupRow otherSend{toFieldGroupGuid(other->guid), -12.0f, SendType::Send, false};
    const SendFieldGroup initial{{send, sidechain, otherSend}};
    sound->fieldGroups.send.shared = {FieldGroupMode::Local, initial};
    history.clear();
    InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* sends = panel.findChild<QWidget*>(QStringLiteral("prop_sends"));
    auto* sidechains = panel.findChild<QWidget*>(QStringLiteral("prop_sidechains"));
    ASSERT_TRUE(sends && sidechains);
    const auto numbers = sends->findChildren<QLabel*>(QStringLiteral("tableRowNumber"));
    ASSERT_EQ(numbers.size(), 2);
    EXPECT_EQ(numbers[1]->text(), QStringLiteral("2"));
    const auto sideNumbers = sidechains->findChildren<QLabel*>(QStringLiteral("tableRowNumber"));
    ASSERT_EQ(sideNumbers.size(), 1);
    EXPECT_EQ(sideNumbers.front()->text(), QStringLiteral("1"));
    auto* add = sidechains->findChild<QToolButton*>(QStringLiteral("listAdd_sidechains"));
    ASSERT_NE(add, nullptr);
    add->click();
    auto* menu = sidechains->findChild<QMenu*>(QStringLiteral("listAddMenu_sidechains"));
    ASSERT_NE(menu, nullptr);
    ASSERT_EQ(menu->actions().size(), 2);
    EXPECT_EQ(menu->actions()[0]->text(), QStringLiteral("Sidechain to..."));
    EXPECT_EQ(menu->actions()[1]->text(), QStringLiteral("Sidechain to... (Ducking Enabled)"));
    menu->close();
    auto* check = sidechains->findChild<QCheckBox*>();
    ASSERT_NE(check, nullptr);
    ASSERT_TRUE(check->isEnabled());
    check->click();
    auto enabled = sidechain;
    enabled.duckTarget = true;
    EXPECT_EQ(sound->fieldGroups.send.shared.localHistory, (SendFieldGroup{{send, enabled, otherSend}}));
    auto* table = sends->findChild<TableWidget*>();
    ASSERT_NE(table, nullptr);
    table->moveRow(0, 2);
    EXPECT_EQ(sound->fieldGroups.send.shared.localHistory, (SendFieldGroup{{otherSend, enabled, send}}));
    auto* remove = sends->findChild<QToolButton*>(QStringLiteral("listRemoveSelected_sends"));
    ASSERT_TRUE(remove && remove->isEnabled());
    remove->click();
    EXPECT_EQ(sound->fieldGroups.send.shared.localHistory, (SendFieldGroup{{otherSend, enabled}}));
    ASSERT_EQ(history.undoStack()->count(), 3);
    history.undo();
    history.undo();
    history.undo();
    EXPECT_EQ(sound->fieldGroups.send.shared.localHistory, initial);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*target);
    const auto* property = PlayablePropertyAuthoringV1::fieldByName(QStringLiteral("sends"));
    ASSERT_TRUE(locator && property);
    QJsonArray busRows{QJsonObject{{"targetBusID", static_cast<qint64>(other->objectID)}, {"sendLevel", -9.0}}};
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*target, *locator, property->id, busRows, std::nullopt));
    InspectorPanel busPanel;
    busPanel.showItemInspector(target);
    auto* receives = busPanel.findChild<QWidget*>(QStringLiteral("prop_receives"));
    ASSERT_NE(receives, nullptr);
    auto* busSends = busPanel.findChild<QWidget*>(QStringLiteral("prop_sends"));
    ASSERT_NE(busSends, nullptr);
    EXPECT_EQ(busSends->findChildren<QLabel*>(QStringLiteral("tableRowNumber")).size(), 1);
    const auto incoming = receives->findChildren<QWidget*>(QStringLiteral("listRow"));
    ASSERT_EQ(incoming.size(), 2);
    auto* sendRow = incoming[0]->layout();
    auto* sendDuckingCell = sendRow->itemAt(sendRow->count() - 1)->widget();
    ASSERT_NE(sendDuckingCell, nullptr);
    EXPECT_EQ(sendDuckingCell->findChild<QCheckBox*>(), nullptr);
    const auto labels = sendDuckingCell->findChildren<QLabel*>();
    EXPECT_TRUE(std::all_of(labels.begin(), labels.end(), [](const QLabel* label) {
        return label->text().isEmpty();
    }));
    auto* ducking = incoming[1]->findChild<QCheckBox*>();
    ASSERT_NE(ducking, nullptr);
    EXPECT_FALSE(ducking->isEnabled());
    EXPECT_FALSE(ducking->isChecked());
}

TEST(PropertyEditorEngineSelection, SharedReferencesKeepMixedInheritanceAndAtomicHistoryAcrossPlatforms)
{
    using namespace harken::studio;
    ensureApp();
    registerBuiltinInspectorCustomizations();
    auto& manager = ProjectManager::instance();
    auto& history = UndoRedoManager::instance();
    ActionManager::instance().clearAllForTesting();
    registerProjectActions(ActionManager::instance(), &manager, nullptr);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ProjectConfig config;
    config.name = QStringLiteral("SharedReferenceSelection");
    config.location = directory.path();
    ASSERT_TRUE(manager.createProject(config));
    const auto close = qScopeGuard([&] {
        history.clear();
        manager.closeProject();
        ActionManager::instance().clearAllForTesting();
    });
    auto* parent = manager.createItem(nullptr, QStringLiteral("Parent"), ProjectItem::Type::Layer);
    ASSERT_NE(parent, nullptr);
    auto* first = manager.createItem(parent, QStringLiteral("First"), ProjectItem::Type::Sound);
    auto* second = manager.createItem(parent, QStringLiteral("Second"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    second->fieldGroups.conversion.shared = {FieldGroupMode::Local, ConversionFieldGroup{}};
    const auto firstBefore = first->fieldGroups;
    const auto secondBefore = second->fieldGroups;
    const auto parentBefore = parent->fieldGroups;
    ASSERT_FALSE(manager.currentProject()->conversionItems.empty());
    auto* preset = manager.currentProject()->conversionItems.front().rootItem.get();
    ASSERT_NE(preset, nullptr);
    history.clear();
    InspectorPanel panel;
    panel.showItemInspector(first);
    panel.setConversionSelection({first, second});
    auto* inherit = panel.findChild<QCheckBox*>(QStringLiteral("inherit_conversion"));
    auto* settings = panel.findChild<ObjectRefWidget*>(QStringLiteral("prop_settingsRef"));
    ASSERT_NE(inherit, nullptr);
    ASSERT_NE(settings, nullptr);
    EXPECT_EQ(inherit->checkState(), Qt::PartiallyChecked);
    EXPECT_TRUE(settings->isReadOnly());
    inherit->click();
    EXPECT_EQ(inherit->checkState(), Qt::Checked);
    EXPECT_EQ(history.undoStack()->count(), 1);
    history.undo();
    EXPECT_EQ(first->fieldGroups, firstBefore);
    EXPECT_EQ(second->fieldGroups, secondBefore);
    EXPECT_EQ(inherit->checkState(), Qt::PartiallyChecked);
    history.redo();
    inherit->click();
    EXPECT_EQ(inherit->checkState(), Qt::Unchecked);
    EXPECT_FALSE(settings->isReadOnly());
    const auto firstLocal = first->fieldGroups;
    const auto secondLocal = second->fieldGroups;
    emit settings->objectChanged(preset->objectID);
    EXPECT_EQ(history.undoStack()->count(), 3);
    history.undo();
    EXPECT_EQ(first->fieldGroups, firstLocal);
    EXPECT_EQ(second->fieldGroups, secondLocal);
    history.redo();
    for (const auto& platform : manager.currentProject()->platforms) {
        PlatformContextService::instance().setSelected({platform.id}, platform.id);
        EXPECT_EQ(settings->currentObjectID(), preset->objectID);
        EXPECT_FALSE(settings->isReadOnly());
    }
    PlatformContextService::instance().setAll(true);
    emit settings->objectChanged(0);
    EXPECT_EQ(history.undoStack()->count(), 4);
    EXPECT_TRUE(harken::studio::project_schema::IsNull(first->fieldGroups.conversion.shared.localHistory->settingsRef));
    EXPECT_TRUE(harken::studio::project_schema::IsNull(second->fieldGroups.conversion.shared.localHistory->settingsRef));
    history.undo();
    EXPECT_EQ(settings->currentObjectID(), preset->objectID);
    EXPECT_EQ(parent->fieldGroups, parentBefore);
    const auto firstStable = first->fieldGroups;
    const auto secondStable = second->fieldGroups;
    const QVector<FieldGroupDefinitionSnapshot> expected{
        {first->guid, firstStable}, {second->guid, secondStable}};
    second->fieldGroups.conversion.shared.localHistory->settingsRef = {};
    const auto secondChanged = second->fieldGroups;
    const int historyIndex = history.undoStack()->index();
    emit settings->objectChanged(0);
    EXPECT_EQ(first->fieldGroups, firstStable);
    EXPECT_EQ(second->fieldGroups, secondChanged);
    QString error;
    EXPECT_FALSE(ConversionEditing::setObjects(manager, {first->guid, second->guid},
        std::nullopt, QUuid{}, {}, expected, nullptr, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(first->fieldGroups, firstStable);
    EXPECT_EQ(second->fieldGroups, secondChanged);
    EXPECT_EQ(history.undoStack()->index(), historyIndex);
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(second, secondStable, secondChanged));
    panel.showItemInspector(first);
    emit settings->objectChanged(0);
    EXPECT_TRUE(harken::studio::project_schema::IsNull(first->fieldGroups.conversion.shared.localHistory->settingsRef));
    EXPECT_EQ(second->fieldGroups, secondStable);
    EXPECT_EQ(history.undoStack()->index(), historyIndex + 1);
    history.undo();
    EXPECT_EQ(first->fieldGroups, firstStable);
    EXPECT_EQ(second->fieldGroups, secondStable);
}
