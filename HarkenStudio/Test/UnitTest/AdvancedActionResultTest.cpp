#include "ActionManager.h"
#include "AudioEngine.h"
#include "UndoCommands.h"

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
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
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

TEST_F(AdvancedActionResultTest, AuthoringWriteScopeSurvivesReplayAndKeepsUnknownTransactionsConservative)
{
    auto& actions = VNX::ActionManager::instance();
    auto& undo = VNX::UndoRedoManager::instance();
    const QSet<QUuid> writes{QUuid::createUuid()};
    int value = 0;
    VNX::ActionDef def;
    def.id = QStringLiteral("write_scope_contract");
    def.displayName = def.id;
    def.handler = [&](const QJsonObject&) {
        const int before = value;
        ++value;
        VNX::ActionResult result;
        result.success = true;
        result.undoFn = [&value, before] { value = before; };
        result.redoFn = [&value, after = value] { value = after; };
        result.authoringWriteSet = writes;
        return result;
    };
    actions.registerAction(def);
    QObject observer;
    QList<std::optional<QSet<QUuid>>> observed;
    QObject::connect(&undo, &VNX::UndoRedoManager::indexChanged, &observer, [&] {
        const auto* current = undo.currentAuthoringWriteSet();
        observed.append(current ? std::optional<QSet<QUuid>>(*current) : std::nullopt);
    });
    const auto check = [&](bool exact) {
        ASSERT_FALSE(observed.isEmpty());
        EXPECT_EQ(observed.back().has_value(), exact);
        if (exact && observed.back()) EXPECT_EQ(*observed.back(), writes);
        EXPECT_EQ(undo.currentAuthoringWriteSet(), nullptr);
        observed.clear();
    };
    EXPECT_TRUE(actions.execute(def.id, {}).success);
    EXPECT_EQ(value, 1);
    check(true);
    EXPECT_TRUE(undo.undo());
    EXPECT_EQ(value, 0);
    check(true);
    EXPECT_TRUE(undo.redo());
    EXPECT_EQ(value, 1);
    check(true);
    undo.undoStack()->undo();
    check(false);
    undo.undoStack()->redo();
    check(false);
    undo.beginMacro(QStringLiteral("Mixed transaction"));
    EXPECT_TRUE(actions.execute(def.id, {}).success);
    undo.push(new QUndoCommand(QStringLiteral("Unknown authoring changes")));
    undo.endMacro();
    check(false);
    EXPECT_TRUE(undo.undo());
    check(false);
    EXPECT_TRUE(undo.redo());
    check(false);
    undo.clear();
    check(false);
}

}
