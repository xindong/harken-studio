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
    static char arg0[] = "harken_studio_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class AdvancedActionResultTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        harken::studio::AudioEngine::instance().shutdown();
        QCoreApplication::processEvents();
        harken::studio::ActionManager::instance().clearAllForTesting();
        harken::studio::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        harken::studio::UndoRedoManager::instance().clear();
        harken::studio::ActionManager::instance().clearAllForTesting();
        harken::studio::AudioEngine::instance().shutdown();
        QCoreApplication::processEvents();
    }
};

TEST_F(AdvancedActionResultTest, DecimalStringPreservesFullWidthRuntimeIdentity)
{
    harken::studio::registerAdvancedActions(
        harken::studio::ActionManager::instance(), &harken::studio::AudioEngine::instance());
    const QJsonObject args{
        {QStringLiteral("action_handle"),
         QStringLiteral("18446744073709551615")},
        {QStringLiteral("position"), 0.5},
    };

    const auto result = harken::studio::ActionManager::instance().execute(
        QStringLiteral("seek_action"), args, harken::studio::ActionSource::IPC);

    EXPECT_EQ(result.data.value(QStringLiteral("action_handle")).toString(),
              QStringLiteral("18446744073709551615"));
}

TEST_F(AdvancedActionResultTest, AuthoringWriteScopeSurvivesReplayAndKeepsUnknownTransactionsConservative)
{
    auto& actions = harken::studio::ActionManager::instance();
    auto& undo = harken::studio::UndoRedoManager::instance();
    const QSet<QUuid> writes{QUuid::createUuid()};
    int value = 0;
    harken::studio::ActionDef def;
    def.id = QStringLiteral("write_scope_contract");
    def.displayName = def.id;
    def.handler = [&](const QJsonObject&) {
        const int before = value;
        ++value;
        harken::studio::ActionResult result;
        result.success = true;
        result.undoFn = [&value, before] { value = before; };
        result.redoFn = [&value, after = value] { value = after; };
        result.authoringWriteSet = writes;
        return result;
    };
    actions.registerAction(def);
    QObject observer;
    QList<std::optional<QSet<QUuid>>> observed;
    QObject::connect(&undo, &harken::studio::UndoRedoManager::indexChanged, &observer, [&] {
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
    undo.push(new harken::studio::UndoCommand(QStringLiteral("Unknown authoring changes")));
    undo.endMacro();
    check(false);
    EXPECT_TRUE(undo.undo());
    check(false);
    EXPECT_TRUE(undo.redo());
    check(false);
    undo.clear();
    check(false);
}

TEST_F(AdvancedActionResultTest, AuthoringWriteScopeComposesAcrossMergingAndNestedMacros)
{
    auto& undo = harken::studio::UndoRedoManager::instance();
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();
    const QSet<QUuid> combined{first, second};
    QObject observer;
    std::optional<QSet<QUuid>> observed;
    QObject::connect(&undo, &harken::studio::UndoRedoManager::indexChanged, &observer, [&] {
        const auto* current = undo.currentAuthoringWriteSet();
        observed = current ? std::optional<QSet<QUuid>>(*current) : std::nullopt;
    });
    const auto push = [&](const QUuid& guid, int mergeId = 0) {
        undo.push(new harken::studio::RuntimeActionCommand(
            QStringLiteral("Tracked write"), [] {}, [] {}, mergeId, nullptr, {}, QSet<QUuid>{guid}));
    };
    push(first, 17);
    push(second, 17);
    EXPECT_EQ(undo.undoStack()->count(), 1);
    ASSERT_TRUE(undo.undo());
    ASSERT_TRUE(observed);
    EXPECT_EQ(*observed, combined);
    ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(observed);
    EXPECT_EQ(*observed, combined);

    undo.beginMacro(QStringLiteral("Outer"));
    push(first);
    undo.beginMacro(QStringLiteral("Inner"));
    push(second);
    undo.endMacro();
    undo.endMacro();
    ASSERT_TRUE(observed);
    EXPECT_EQ(*observed, combined);
    ASSERT_TRUE(undo.undo());
    ASSERT_TRUE(observed);
    EXPECT_EQ(*observed, combined);
    ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(observed);
    EXPECT_EQ(*observed, combined);
    EXPECT_EQ(undo.currentAuthoringWriteSet(), nullptr);
    undo.clearHistory();
    ASSERT_TRUE(observed);
    EXPECT_TRUE(observed->isEmpty());
    EXPECT_EQ(undo.currentAuthoringWriteSet(), nullptr);
}

}
