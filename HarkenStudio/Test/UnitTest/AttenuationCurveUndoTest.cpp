#include "ActionManager.h"
#include "AttenuationStore.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/CurveEditor/CurveEditorWidget.h"
#include "Widgets/Inspector/Providers/DimensionsSectionProvider.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QToolButton>

#include <initializer_list>
#include <memory>
#include <utility>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QJsonArray makePoints(std::initializer_list<std::pair<double, double>> pts)
{
    QJsonArray arr;
    for (const auto& p : pts)
        arr.append(QJsonObject{{"distance", p.first}, {"value", p.second}});
    return arr;
}

class AttenuationCurveUndoTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(),
                                    &VNX::ProjectManager::instance(), nullptr, {});
        VNX::registerSpatialActions(VNX::ActionManager::instance(), nullptr,
                                    &VNX::ProjectManager::instance());

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("CurveUndoProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(VNX::ProjectManager::instance().createProject(config));

        const auto seeded = VNX::ActionManager::instance().execute(
            QStringLiteral("set_attenuation_curve"),
            QJsonObject{
                {"name", QStringLiteral("TestAtt")},
                {"curve", QStringLiteral("volume")},
                {"points", makePoints({{0.0, 0.0}, {100.0, -96.0}})},
                {"create_if_missing", true},
                {"max_distance", 100.0}
            });
        ASSERT_TRUE(seeded.success);
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ProjectManager::instance().closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    const VNX::AttenuationDimensionDef* volumeDimension()
    {
        auto* project = VNX::ProjectManager::instance().currentProject();
        const auto* def = VNX::AttenuationStore::find(project, QStringLiteral("TestAtt"));
        return def ? def->dimension(VNX::AttenuationDimension::Volume) : nullptr;
    }

    QUndoStack* stack() { return VNX::UndoRedoManager::instance().undoStack(); }

    QTemporaryDir tempDir_;
};

}

TEST_F(AttenuationCurveUndoTest, SetCurveCommandUndoRedoRoundtrip)
{
    EXPECT_TRUE(stack()->isClean());

    const auto edited = VNX::ActionManager::instance().execute(
        QStringLiteral("set_attenuation_curve"),
        QJsonObject{
            {"name", QStringLiteral("TestAtt")},
            {"curve", QStringLiteral("volume")},
            {"points", makePoints({{0.0, 0.0}, {50.0, -30.0}, {100.0, -96.0}})},
            {"max_distance", 100.0}
        });
    ASSERT_TRUE(edited.success);

    const auto* dim = volumeDimension();
    ASSERT_NE(dim, nullptr);
    ASSERT_EQ(dim->curve.size(), 3);
    EXPECT_FALSE(stack()->isClean());

    stack()->undo();
    dim = volumeDimension();
    ASSERT_NE(dim, nullptr);
    EXPECT_EQ(dim->curve.size(), 2);
    EXPECT_TRUE(stack()->isClean());

    stack()->redo();
    dim = volumeDimension();
    ASSERT_NE(dim, nullptr);
    ASSERT_EQ(dim->curve.size(), 3);
    EXPECT_FLOAT_EQ(dim->curve.at(1).x, 50.0f);
    EXPECT_FLOAT_EQ(dim->curve.at(1).value, -30.0f);
    EXPECT_FALSE(stack()->isClean());
}

TEST_F(AttenuationCurveUndoTest, EachCurveCommitIsOneUndoStep)
{
    const int base = stack()->count();

    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_attenuation_curve"),
        QJsonObject{
            {"name", QStringLiteral("TestAtt")},
            {"curve", QStringLiteral("volume")},
            {"points", makePoints({{0.0, -3.0}, {100.0, -96.0}})}
        }).success);
    EXPECT_EQ(stack()->count(), base + 1);

    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_attenuation_curve"),
        QJsonObject{
            {"name", QStringLiteral("TestAtt")},
            {"curve", QStringLiteral("volume")},
            {"points", makePoints({{0.0, -6.0}, {100.0, -96.0}})}
        }).success);
    EXPECT_EQ(stack()->count(), base + 2);

    stack()->undo();
    const auto* dim = volumeDimension();
    ASSERT_NE(dim, nullptr);
    EXPECT_FLOAT_EQ(dim->curve.front().value, -3.0f);

    stack()->undo();
    dim = volumeDimension();
    ASSERT_NE(dim, nullptr);
    EXPECT_FLOAT_EQ(dim->curve.front().value, 0.0f);
    EXPECT_TRUE(stack()->isClean());
}

TEST_F(AttenuationCurveUndoTest, AddedDimensionsPersistDefaultCurvesThroughUndoRedo)
{
    auto* project = VNX::ProjectManager::instance().currentProject();
    auto* attenuation = VNX::AttenuationStore::find(project, QStringLiteral("TestAtt"));
    ASSERT_NE(attenuation, nullptr);
    attenuation->dimensions.clear();
    attenuation->maxDistance = 250.0f;
    for (const auto type : {VNX::AttenuationDimension::Volume,
                            VNX::AttenuationDimension::Pitch}) {
        const int base = stack()->count();
        const auto added = VNX::ActionManager::instance().execute(
            QStringLiteral("add_attenuation_dimension"),
            QJsonObject{{"name", QStringLiteral("TestAtt")},
                        {"dimension", QLatin1String(VNX::attenuationDimensionName(type))}});
        ASSERT_TRUE(added.success);
        EXPECT_EQ(stack()->count(), base + 1);
        attenuation = VNX::AttenuationStore::find(project, QStringLiteral("TestAtt"));
        const auto* dimension = attenuation->dimension(type);
        ASSERT_NE(dimension, nullptr);
        ASSERT_EQ(dimension->curve.size(), 2);
        ASSERT_EQ(dimension->handles.size(), 1);
        EXPECT_FLOAT_EQ(dimension->curve.front().x, 0.0f);
        EXPECT_FLOAT_EQ(dimension->curve.back().x, 250.0f);
        EXPECT_FLOAT_EQ(dimension->curve.front().value, 0.0f);
        EXPECT_FLOAT_EQ(dimension->curve.back().value,
                        type == VNX::AttenuationDimension::Volume ? -96.0f : 0.0f);
        const QJsonObject after = attenuation->toJson();

        VNX::CurveEditorWidget editor;
        editor.bindSelection(VNX::CurveSelection::attenuation(
            attenuation->hostItem()->guid, attenuation->hostItem()->name, type));
        VNX::AttenuationDimensionDef displayed;
        displayed.type = type;
        displayed.curve = editor.canvas()->curve();
        displayed.handles = editor.canvas()->curveHandles();
        EXPECT_EQ(displayed.toJson(), dimension->toJson());

        stack()->undo();
        attenuation = VNX::AttenuationStore::find(project, QStringLiteral("TestAtt"));
        EXPECT_EQ(attenuation->dimension(type), nullptr);
        stack()->redo();
        attenuation = VNX::AttenuationStore::find(project, QStringLiteral("TestAtt"));
        EXPECT_EQ(attenuation->toJson(), after);
        VNX::VAttenuation restored;
        ASSERT_TRUE(restored.fromJson(after));
        EXPECT_EQ(restored.toJson(), after);
    }
}

TEST_F(AttenuationCurveUndoTest, CanvasCutPasteRestoresCurveWithOneUndoStepPerEdit)
{
    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_attenuation_curve"),
        QJsonObject{
            {"name", QStringLiteral("TestAtt")},
            {"curve", QStringLiteral("volume")},
            {"points", makePoints({{0.0, 0.0}, {50.0, -40.0}, {100.0, -96.0}})},
            {"handles", QJsonArray{QJsonArray{10.0, -5.0, 40.0, -30.0},
                                    QJsonArray{60.0, -50.0, 90.0, -80.0}}}
        }).success);
    VNX::UndoRedoManager::instance().clear();
    const QJsonObject original = volumeDimension()->toJson();
    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::attenuation(
        {}, QStringLiteral("TestAtt"), VNX::AttenuationDimension::Volume));
    auto* canvas = editor.canvas();
    canvas->setSelectedPointIndices({1}, 1);

    ASSERT_TRUE(canvas->copySelectedPointsToClipboard());
    ASSERT_TRUE(canvas->pasteFromClipboard());
    EXPECT_EQ(stack()->count(), 0);
    EXPECT_TRUE(stack()->isClean());

    ASSERT_TRUE(canvas->cutSelectedPointsToClipboard());
    ASSERT_EQ(stack()->count(), 1);
    const QJsonObject cut = volumeDimension()->toJson();
    ASSERT_TRUE(canvas->pasteFromClipboard());
    ASSERT_EQ(stack()->count(), 2);
    EXPECT_EQ(volumeDimension()->toJson(), original);

    stack()->undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(volumeDimension()->toJson(), cut);
    EXPECT_EQ(canvas->curve().size(), 2);
    stack()->undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(volumeDimension()->toJson(), original);
    EXPECT_EQ(canvas->curve().size(), 3);
    EXPECT_TRUE(stack()->isClean());
    stack()->redo();
    stack()->redo();
    QCoreApplication::processEvents();
    EXPECT_EQ(volumeDimension()->toJson(), original);
    EXPECT_EQ(canvas->curve().size(), 3);
}

TEST_F(AttenuationCurveUndoTest, GenericFieldEditsShareValidationAndUndoWithoutRestoringNames)
{
    auto& manager = VNX::ProjectManager::instance();
    auto& actions = VNX::ActionManager::instance();
    auto* component = VNX::AttenuationStore::find(manager.currentProject(), QStringLiteral("TestAtt"));
    ASSERT_NE(component, nullptr);
    auto* item = component->hostItem();
    const auto guid = item->guid;
    const auto before = component->toJson();
    VNX::CurveEditorWidget editor;
    editor.bindSelection(VNX::CurveSelection::attenuation(
        guid, item->name, VNX::AttenuationDimension::Volume));
    VNX::DimensionsSectionProvider provider;
    std::unique_ptr<QWidget> section(provider.build(nullptr));
    provider.bind(section.get(), item);
    auto* add = section->findChild<QToolButton*>(QStringLiteral("listAdd_dimensions"));
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE(add->isEnabled());
    const QJsonArray rows{
        QJsonObject{{"type", "volume"},
            {"points", makePoints({{0.0, 0.0}, {40.0, -30.0}, {100.0, -96.0}})}},
        QJsonObject{{"type", "pitch"},
            {"points", makePoints({{0.0, 0.0}, {100.0, 0.0}})}}};
    auto args = QJsonObject{{"object_id", static_cast<qint64>(item->objectID)},
        {"property", "dimensions"}, {"value", rows}};
    const auto changed = actions.execute(QStringLiteral("set_item_property"), args);
    ASSERT_TRUE(changed.success) << changed.errorMessage.toStdString();
    QCoreApplication::processEvents();
    EXPECT_EQ(editor.canvas()->curve().size(), 3);
    EXPECT_FALSE(add->isEnabled());
    EXPECT_EQ(stack()->count(), 1);
    const auto after = component->toJson();
    args["value"] = QJsonArray{rows.first(), rows.first()};
    EXPECT_FALSE(actions.execute(QStringLiteral("set_item_property"), args).success);
    EXPECT_EQ(component->toJson(), after);
    EXPECT_EQ(stack()->count(), 1);

    ASSERT_TRUE(VNX::AttenuationStore::rename(manager.currentProject(), item->name,
        QStringLiteral("RenamedAtt"), &manager));
    stack()->undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(editor.canvas()->curve().size(), 2);
    EXPECT_TRUE(add->isEnabled());
    EXPECT_EQ(component->toJson(), before);
    EXPECT_EQ(item->name, QStringLiteral("RenamedAtt"));
    EXPECT_EQ(item->guid, guid);
    stack()->redo();
    QCoreApplication::processEvents();
    EXPECT_EQ(editor.canvas()->curve().size(), 3);
    EXPECT_FALSE(add->isEnabled());
    EXPECT_EQ(component->toJson(), after);
    EXPECT_EQ(component->snapshot().name, item->name);

    args["property"] = "maxDistance";
    args["value"] = 175.0;
    ASSERT_TRUE(actions.execute(QStringLiteral("set_item_property"), args).success);
    QCoreApplication::processEvents();
    EXPECT_FLOAT_EQ(component->maxDistance, 175.0f);
    EXPECT_FLOAT_EQ(editor.canvas()->xMax(), 175.0f);
    stack()->undo();
    QCoreApplication::processEvents();
    EXPECT_FLOAT_EQ(component->maxDistance, 100.0f);
    EXPECT_FLOAT_EQ(editor.canvas()->xMax(), 100.0f);
    EXPECT_EQ(component->toJson(), after);
}

TEST_F(AttenuationCurveUndoTest, InvalidCurveCreationLeavesNoObjectOrUndoEntry)
{
    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("set_attenuation_curve"),
        QJsonObject{{"name", "InvalidAtt"}, {"curve", "unknown"},
                    {"points", makePoints({{0.0, 0.0}})}, {"create_if_missing", true}});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(VNX::AttenuationStore::find(VNX::ProjectManager::instance().currentProject(),
        QStringLiteral("InvalidAtt")), nullptr);
    EXPECT_EQ(stack()->count(), 0);
}

TEST_F(AttenuationCurveUndoTest, DuplicatedAttenuationHasIndependentIdentityAndCurves)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* original = VNX::AttenuationStore::find(manager.currentProject(), QStringLiteral("TestAtt"));
    ASSERT_NE(original, nullptr);
    const auto data = original->toJson();
    auto* copy = manager.duplicateItem(original->hostItem());
    ASSERT_NE(copy, nullptr);
    auto* component = copy->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);
    EXPECT_NE(copy->guid, original->hostItem()->guid);
    EXPECT_EQ(component->snapshot().guid, copy->guid);
    EXPECT_EQ(component->snapshot().name, copy->name);
    EXPECT_EQ(component->toJson(), data);
    ASSERT_TRUE(component->setDimensions({}));
    EXPECT_EQ(original->toJson(), data);
}
