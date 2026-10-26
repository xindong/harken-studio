#include "ActionManager.h"
#include "AudioEngine.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QJsonObject>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class AdvancedActionResultTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::AudioEngine::instance().shutdown();
        QCoreApplication::processEvents();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    void TearDown() override
    {
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::AudioEngine::instance().shutdown();
        QCoreApplication::processEvents();
    }
};

TEST_F(AdvancedActionResultTest, DecimalStringPreservesFullWidthGameObjectId)
{
    VNX::registerAdvancedActions(
        VNX::ActionManager::instance(), &VNX::AudioEngine::instance());
    const QJsonObject args{
        {QStringLiteral("game_object_id"),
         QStringLiteral("18446744073709551615")},
        {QStringLiteral("volume"), 0.5},
    };

    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("set_game_object_volume"), args, VNX::ActionSource::IPC);

    EXPECT_EQ(result.data.value(QStringLiteral("game_object_id")).toString(),
              QStringLiteral("18446744073709551615"));
}

}
