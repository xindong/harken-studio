#include "Core/UndoHistory.h"
#include "Core/ActionManager.h"
#include "Core/Components/VMixNode.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyEditSession.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QStringList>
#include <QTemporaryDir>

namespace {

class ValueChange : public harken::studio::UndoCommand
{
public:
    ValueChange(int& value, int next, int mergeId = -1, harken::studio::UndoCommand* parent = nullptr)
        : UndoCommand(QString::number(next), parent), value_(value), before_(value),
          after_(next), mergeId_(mergeId) {}

    void undo() override { value_ = before_; }
    void redo() override { value_ = after_; }
    int id() const override { return mergeId_; }
    bool mergeWith(const harken::studio::UndoCommand* other) override
    {
        const auto* change = dynamic_cast<const ValueChange*>(other);
        if (!change || &change->value_ != &value_) return false;
        after_ = change->after_;
        setText(change->text());
        return true;
    }

private:
    int& value_;
    int before_;
    int after_;
    int mergeId_;
};

TEST(UndoHistoryTest, MergingPreservesSavedBoundaryAndReplaysCompleteChange)
{
    harken::studio::UndoStack history;
    int value = 0;
    history.push(new ValueChange(value, 1, 10));
    history.push(new ValueChange(value, 2, 10));
    EXPECT_EQ(history.count(), 1);
    history.setClean();
    history.push(new ValueChange(value, 3, 10));
    history.push(new ValueChange(value, 4, 10));
    EXPECT_EQ(history.count(), 2);
    EXPECT_EQ(value, 4);
    EXPECT_FALSE(history.isClean());
    history.undo();
    EXPECT_EQ(value, 2);
    EXPECT_TRUE(history.isClean());
    history.undo();
    EXPECT_EQ(value, 0);
    history.redo();
    history.redo();
    EXPECT_EQ(value, 4);
}

TEST(UndoHistoryTest, NestedMacrosReplayChildrenInDependencyOrder)
{
    harken::studio::UndoStack history;
    int value = 0;
    history.beginMacro(QStringLiteral("Outer"));
    history.push(new ValueChange(value, 1));
    history.beginMacro(QStringLiteral("Inner"));
    history.push(new ValueChange(value, 2));
    history.push(new ValueChange(value, 3));
    history.endMacro();
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.canRedo());
    history.push(new ValueChange(value, 4));
    history.endMacro();
    EXPECT_EQ(history.count(), 1);
    EXPECT_EQ(history.command(0)->childCount(), 3);
    EXPECT_EQ(value, 4);
    history.undo();
    EXPECT_EQ(value, 0);
    history.redo();
    EXPECT_EQ(value, 4);
}

TEST(UndoHistoryTest, BranchingDiscardsRedoAndInvalidatesUnreachableSave)
{
    harken::studio::UndoStack history;
    int value = 0;
    history.push(new ValueChange(value, 1));
    history.push(new ValueChange(value, 2));
    history.setClean();
    history.undo();
    history.push(new ValueChange(value, 3));
    EXPECT_FALSE(history.canRedo());
    EXPECT_EQ(history.cleanIndex(), -1);
    EXPECT_EQ(history.count(), 2);
    history.undo();
    EXPECT_EQ(value, 1);
    history.redo();
    EXPECT_EQ(value, 3);
}

TEST(UndoHistoryTest, HistoryLimitRetainsWholeMacroAndReachableSave)
{
    harken::studio::UndoStack history;
    history.setUndoLimit(2);
    int value = 0;
    history.push(new ValueChange(value, 1));
    history.setClean();
    history.beginMacro(QStringLiteral("Combined"));
    history.push(new ValueChange(value, 2));
    history.push(new ValueChange(value, 3));
    history.endMacro();
    history.push(new ValueChange(value, 4));
    EXPECT_EQ(history.count(), 2);
    history.undo();
    history.undo();
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(history.isClean());
    EXPECT_FALSE(history.canUndo());
    history.push(new ValueChange(value, 5));
    EXPECT_EQ(value, 5);
}

TEST(UndoHistoryTest, ParentOwnsChildrenAndSupportsIndependentRemoval)
{
    int destroyed = 0;
    struct Tracked : harken::studio::UndoCommand {
        Tracked(int& count, harken::studio::UndoCommand* parent) : UndoCommand(parent), count_(count) {}
        ~Tracked() override { ++count_; }
        int& count_;
    };
    auto parent = std::make_unique<harken::studio::UndoCommand>();
    auto* removed = new Tracked(destroyed, parent.get());
    new Tracked(destroyed, parent.get());
    delete removed;
    EXPECT_EQ(parent->childCount(), 1);
    parent.reset();
    EXPECT_EQ(destroyed, 2);
}

TEST(UndoHistoryTest, MergedOperationsNotifyObserversEvenAtSameHistoryIndex)
{
    harken::studio::UndoStack history;
    int value = 0;
    QStringList observed;
    QObject::connect(&history, &harken::studio::UndoStack::indexChanged, &history,
        [&] { observed.append(history.undoText()); });
    history.push(new ValueChange(value, 1, 10));
    history.push(new ValueChange(value, 2, 10));
    EXPECT_EQ(observed, (QStringList{QStringLiteral("1"), QStringLiteral("2")}));
}

TEST(UndoHistoryTest, ObsoleteHistoryEntryDoesNotReplayAndReleasesItsSlot)
{
    harken::studio::UndoStack history;
    int value = 0;
    auto* change = new ValueChange(value, 1);
    history.push(change);
    change->setObsolete(true);
    history.undo();
    EXPECT_EQ(value, 1);
    EXPECT_EQ(history.count(), 0);
    EXPECT_EQ(history.index(), 0);
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.canRedo());
}

TEST(UndoHistoryTest, ClearingEmptyHistoryDoesNotInventASavedState)
{
    harken::studio::UndoStack history;
    history.resetClean();
    history.clear();
    EXPECT_EQ(history.cleanIndex(), -1);
    EXPECT_FALSE(history.isClean());
}

class AuthoringEditTransactionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_NE(qApp, nullptr);
        ASSERT_TRUE(directory_.isValid());
        auto& project = harken::studio::ProjectManager::instance();
        ASSERT_TRUE(project.closeProject());
        auto& actions = harken::studio::ActionManager::instance();
        actions.clearAllForTesting();
        harken::studio::registerProjectActions(actions, &project);
        harken::studio::ProjectConfig config;
        config.name = QStringLiteral("EditTransaction");
        config.location = directory_.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
        ASSERT_TRUE(project.createProject(config));
        item_ = project.createItem(project.getMasterBusItem(), QStringLiteral("Bus"), harken::studio::ProjectItem::Type::Bus);
        ASSERT_NE(item_, nullptr);
        harken::studio::PlatformContextService::instance().setAll(true);
        harken::studio::UndoRedoManager::instance().clearHistory();
    }

    void TearDown() override
    {
        harken::studio::PropertyEditSession::finishAllActive(false);
        harken::studio::UndoRedoManager::instance().clear();
        harken::studio::ProjectManager::instance().closeProject();
        harken::studio::ActionManager::instance().clearAllForTesting();
    }

    harken::studio::ActionResult setField(const QString& field, double value,
                              harken::studio::PlatformScope platform = std::nullopt)
    {
        QJsonObject args{{QStringLiteral("authoring_handle"), static_cast<qint64>(item_->objectID)},
            {QStringLiteral("property"), field}, {QStringLiteral("value"), value}};
        if (platform)
            args.insert(QStringLiteral("platform_ids"), QJsonArray{harken::studio::ProjectPlatformSemantics::idString(*platform)});
        return harken::studio::ActionManager::instance().execute(QStringLiteral("set_item_property"), args);
    }

    QJsonObject authoredState() const
    {
        return {{QStringLiteral("mixnode"), item_->comp<harken::studio::VMixNode>()->toJson()},
            {QStringLiteral("definition"), harken::studio::PlayablePropertyAuthoringV1::definitionToJson(*item_)},
            {QStringLiteral("platforms"), harken::studio::PlayablePropertyAuthoringV1::platformOverridesToJson(item_->platformOverrides)}};
    }

    QTemporaryDir directory_;
    harken::studio::ProjectItem* item_ = nullptr;
};

TEST_F(AuthoringEditTransactionTest, CancelledPreviewsRestoreValuesAndOverridePresenceWithoutHistory)
{
    const auto windows = HarkenTest::PlatformIds::Windows;
    const auto android = HarkenTest::PlatformIds::Android;
    struct FieldCase { QString field; double base; double overlay; double preview; };
    for (const FieldCase field : {
             FieldCase{QStringLiteral("volume"), -6.0, -12.0, -3.0},
             FieldCase{QStringLiteral("pitch"), 400.0, 800.0, 1200.0}}) {
        SCOPED_TRACE(field.field.toStdString());
        ASSERT_TRUE(setField(field.field, field.base).success);
        ASSERT_TRUE(setField(field.field, field.overlay, windows).success);
        harken::studio::UndoRedoManager::instance().clearHistory();
        const auto before = authoredState();
        for (const harken::studio::PlatformScope scope : {harken::studio::PlatformScope{}, harken::studio::PlatformScope{windows}, harken::studio::PlatformScope{android}}) {
            SCOPED_TRACE(scope ? scope->toString().toStdString() : "shared");
            const auto platforms = scope
                ? std::optional<QVector<harken::studio::ProjectPlatformId>>(QVector<harken::studio::ProjectPlatformId>{*scope})
                : std::nullopt;
            harken::studio::PropertyEditSession edit(item_, field.field,
                scope == harken::studio::PlatformScope{windows} ? field.overlay : field.base, nullptr, platforms);
            edit.preview(field.preview);
            ASSERT_NE(authoredState(), before);
            EXPECT_FALSE(harken::studio::UndoRedoManager::instance().canUndo());
            edit.cancel();
            EXPECT_EQ(authoredState(), before);
            EXPECT_FALSE(harken::studio::UndoRedoManager::instance().canUndo());
            EXPECT_FALSE(harken::studio::UndoRedoManager::instance().canRedo());
        }
    }
}

}
