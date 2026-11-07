#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/ProjectItemInspectorRows.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/ParameterSlider.h"
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
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QTemporaryDir>

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

struct NotesFixture
{
    VNX::PropertyEditorEngine engine;
    std::unique_ptr<QWidget> page;
    QPlainTextEdit* edit = nullptr;

    NotesFixture()
    {
        engine.setSchema({VNX::notesInspectorDescriptor()}, VNX::ComponentType::_Count);
        engine.setItemType(VNX::ProjectItem::Type::Sound);
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

TEST(PropertyEditorEnginePlatformContext, SwitchingMatchesFreshBindingWithoutAuthoringChanges)
{
    using namespace VNX;
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
