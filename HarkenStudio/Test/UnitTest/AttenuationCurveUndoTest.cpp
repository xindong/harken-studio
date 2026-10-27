#include "ActionManager.h"
#include "AttenuationStore.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <initializer_list>
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
                                    &VNX::ProjectManager::instance(), nullptr);
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

    QTemporaryDir tempDir_;
};

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
    EXPECT_EQ(component->toJson(), data);
    ASSERT_TRUE(component->setDimensions({}));
    EXPECT_EQ(original->toJson(), data);
}
