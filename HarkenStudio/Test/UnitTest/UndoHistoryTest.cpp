#include "Core/UndoHistory.h"

#include <gtest/gtest.h>

#include <QStringList>

namespace {

class ValueChange : public VNX::UndoCommand
{
public:
    ValueChange(int& value, int next, int mergeId = -1, VNX::UndoCommand* parent = nullptr)
        : UndoCommand(QString::number(next), parent), value_(value), before_(value),
          after_(next), mergeId_(mergeId) {}

    void undo() override { value_ = before_; }
    void redo() override { value_ = after_; }
    int id() const override { return mergeId_; }
    bool mergeWith(const VNX::UndoCommand* other) override
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
    VNX::UndoStack history;
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
    VNX::UndoStack history;
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
    VNX::UndoStack history;
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
    VNX::UndoStack history;
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
    struct Tracked : VNX::UndoCommand {
        Tracked(int& count, VNX::UndoCommand* parent) : UndoCommand(parent), count_(count) {}
        ~Tracked() override { ++count_; }
        int& count_;
    };
    auto parent = std::make_unique<VNX::UndoCommand>();
    auto* removed = new Tracked(destroyed, parent.get());
    new Tracked(destroyed, parent.get());
    delete removed;
    EXPECT_EQ(parent->childCount(), 1);
    parent.reset();
    EXPECT_EQ(destroyed, 2);
}

TEST(UndoHistoryTest, MergedOperationsNotifyObserversEvenAtSameHistoryIndex)
{
    VNX::UndoStack history;
    int value = 0;
    QStringList observed;
    QObject::connect(&history, &VNX::UndoStack::indexChanged, &history,
        [&] { observed.append(history.undoText()); });
    history.push(new ValueChange(value, 1, 10));
    history.push(new ValueChange(value, 2, 10));
    EXPECT_EQ(observed, (QStringList{QStringLiteral("1"), QStringLiteral("2")}));
}

TEST(UndoHistoryTest, ObsoleteHistoryEntryDoesNotReplayAndReleasesItsSlot)
{
    VNX::UndoStack history;
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
    VNX::UndoStack history;
    history.resetClean();
    history.clear();
    EXPECT_EQ(history.cleanIndex(), -1);
    EXPECT_FALSE(history.isClean());
}

}
