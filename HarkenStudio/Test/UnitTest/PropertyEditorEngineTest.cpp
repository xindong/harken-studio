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
#include "Widgets/FilterEditor/FilterPreviewField.h"
#include "Core/Components/VMixNode.h"
#include "Core/FilterAuthoring.h"
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
    ASSERT_TRUE(FilterAuthoring::setOverride(*item, "filterLowPassFrequency", 6000.0, windows));
    ASSERT_TRUE(FilterAuthoring::setOverride(*item, "filterLowPassFrequency", 1200.0, android));
    undo.clear();
    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(*manager.currentProject()));
    const auto before = serializer.projectItemToJson(*item);
    PropertySchema schema;
    for (const auto& descriptor : VMixNode::schema()) {
        if (descriptor.field == "pitch" || descriptor.field == "filter"
            || descriptor.field == "filterLowPassFrequency")
            schema.push_back(descriptor);
    }
    auto bindPage = [&](PropertyEditorEngine& engine) {
        engine.setItemType(item->type);
        engine.setSchema(schema, ComponentType::MixNode);
        std::unique_ptr<QWidget> page(engine.buildPage());
        engine.bindItem(item);
        return page;
    };
    auto renderPreview = [](FilterPreviewField* preview) {
        preview->setFixedSize(240, 64);
        QImage image(preview->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        preview->render(&image);
        return image;
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
        for (const auto* field : {"prop_pitch", "prop_filterLowPassFrequency"}) {
            SCOPED_TRACE(field);
            const auto* actual = page->findChild<ParameterSlider*>(field);
            const auto* expected = referencePage->findChild<ParameterSlider*>(field);
            ASSERT_NE(actual, nullptr);
            ASSERT_NE(expected, nullptr);
            EXPECT_EQ(actual->mixedValue(), expected->mixedValue());
            if (!expected->mixedValue()) EXPECT_EQ(actual->value(), expected->value());
            EXPECT_EQ(actual->toolTip(), expected->toolTip());
        }
        auto* actualPreview = page->findChild<FilterPreviewField*>("prop_filter");
        auto* expectedPreview = referencePage->findChild<FilterPreviewField*>("prop_filter");
        ASSERT_NE(actualPreview, nullptr);
        ASSERT_NE(expectedPreview, nullptr);
        EXPECT_EQ(renderPreview(actualPreview), renderPreview(expectedPreview));
        EXPECT_EQ(actualPreview->toolTip(), expectedPreview->toolTip());
        EXPECT_EQ(serializer.projectItemToJson(*item), before);
        EXPECT_FALSE(undo.canUndo());
        EXPECT_EQ(changes, 0);
    }
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
