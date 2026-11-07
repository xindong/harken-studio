#include "Core/UndoHistory.h"
#include "Core/ActionManager.h"
#include "Core/Components/VMixNode.h"
#include "Core/FilterAuthoring.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/PropertyEditSession.h"
#include "Core/UndoCommands.h"
#include "Widgets/FilterEditor/FilterEditorWidget.h"
#include "Widgets/FilterEditor/FilterResponseCanvas.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStringList>
#include <QTemporaryDir>

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

class AuthoringEditTransactionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_NE(qApp, nullptr);
        ASSERT_TRUE(directory_.isValid());
        auto& project = VNX::ProjectManager::instance();
        ASSERT_TRUE(project.closeProject());
        auto& actions = VNX::ActionManager::instance();
        actions.clearAllForTesting();
        VNX::registerProjectActions(actions, &project);
        VNX::ProjectConfig config;
        config.name = QStringLiteral("EditTransaction");
        config.location = directory_.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
        ASSERT_TRUE(project.createProject(config));
        item_ = project.createItem(project.getMasterBusItem(), QStringLiteral("Bus"), VNX::ProjectItem::Type::Bus);
        ASSERT_NE(item_, nullptr);
        VNX::PlatformContextService::instance().setAll(true);
        VNX::UndoRedoManager::instance().clearHistory();
    }

    void TearDown() override
    {
        VNX::PropertyEditSession::finishAllActive(false);
        VNX::UndoRedoManager::instance().clear();
        VNX::ProjectManager::instance().closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    VNX::ActionResult setField(const QString& field, double value,
                              VNX::PlatformScope platform = std::nullopt)
    {
        QJsonObject args{{QStringLiteral("authoring_handle"), static_cast<qint64>(item_->objectID)},
            {QStringLiteral("property"), field}, {QStringLiteral("value"), value}};
        if (platform)
            args.insert(QStringLiteral("platform_ids"), QJsonArray{VNX::ProjectPlatformSemantics::idString(*platform)});
        return VNX::ActionManager::instance().execute(QStringLiteral("set_item_property"), args);
    }

    QJsonObject authoredState() const
    {
        return {{QStringLiteral("mixnode"), item_->comp<VNX::VMixNode>()->toJson()},
            {QStringLiteral("definition"), VNX::PlayablePropertyAuthoringV1::definitionToJson(*item_)},
            {QStringLiteral("platforms"), VNX::PlayablePropertyAuthoringV1::platformOverridesToJson(item_->platformOverrides)}};
    }

    QTemporaryDir directory_;
    VNX::ProjectItem* item_ = nullptr;
};

TEST_F(AuthoringEditTransactionTest, CancelledPreviewsRestoreValuesAndOverridePresenceWithoutHistory)
{
    const auto windows = HarkenTest::PlatformIds::Windows;
    const auto android = HarkenTest::PlatformIds::Android;
    struct FieldCase { QString field; double base; double overlay; double preview; };
    for (const FieldCase field : {
             FieldCase{QStringLiteral("volume"), -6.0, -12.0, -3.0},
             FieldCase{QStringLiteral("filterLowShelfFrequency"), 400.0, 800.0, 1200.0}}) {
        SCOPED_TRACE(field.field.toStdString());
        ASSERT_TRUE(setField(field.field, field.base).success);
        ASSERT_TRUE(setField(field.field, field.overlay, windows).success);
        VNX::UndoRedoManager::instance().clearHistory();
        const auto before = authoredState();
        for (const VNX::PlatformScope scope : {VNX::PlatformScope{}, VNX::PlatformScope{windows}, VNX::PlatformScope{android}}) {
            SCOPED_TRACE(scope ? scope->toString().toStdString() : "shared");
            const auto platforms = scope
                ? std::optional<QVector<VNX::ProjectPlatformId>>(QVector<VNX::ProjectPlatformId>{*scope})
                : std::nullopt;
            VNX::PropertyEditSession edit(item_, field.field,
                scope == VNX::PlatformScope{windows} ? field.overlay : field.base, nullptr, platforms);
            edit.preview(field.preview);
            ASSERT_NE(authoredState(), before);
            EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());
            edit.cancel();
            EXPECT_EQ(authoredState(), before);
            EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());
            EXPECT_FALSE(VNX::UndoRedoManager::instance().canRedo());
        }
    }
}

TEST_F(AuthoringEditTransactionTest, GraphGesturesCancelExactlyAndCommitMultipleFieldsAsOneUndo)
{
    const auto windows = HarkenTest::PlatformIds::Windows;
    ASSERT_TRUE(setField(QStringLiteral("filterLowShelfFrequency"), 400.0).success);
    ASSERT_TRUE(setField(QStringLiteral("filterLowShelfGain"), 6.0).success);
    VNX::FilterEditorWidget editor;
    editor.setAttribute(Qt::WA_DontShowOnScreen);
    editor.resize(720, 640);
    editor.bindItem(item_);
    editor.show();
    QApplication::processEvents();
    auto* canvas = editor.findChild<VNX::FilterResponseCanvas*>();
    ASSERT_NE(canvas, nullptr);
    const auto mouse = [canvas](QEvent::Type type, const QPointF& point,
                                Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas->mapToGlobal(point.toPoint()), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    for (bool all : {true, false}) {
        SCOPED_TRACE(all);
        if (all) VNX::PlatformContextService::instance().setAll(true);
        else {
            ASSERT_TRUE(setField(QStringLiteral("filterLowShelfFrequency"), 900.0, windows).success);
            VNX::PlatformContextService::instance().setSelected({windows}, windows, false);
        }
        QApplication::processEvents();
        ASSERT_TRUE(canvas->isVisible());
        VNX::UndoRedoManager::instance().clearHistory();
        const auto before = authoredState();
        const auto scope = all ? VNX::PlatformScope{} : VNX::PlatformScope{windows};
        const auto original = VNX::FilterAuthoring::effectiveState(*item_, scope).sections[2];
        ASSERT_FALSE(canvas->renderData().points.isEmpty());
        const auto point = canvas->renderData().points.front().position;
        const auto moved = point + QPointF(35.0, -20.0);
        mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, moved, Qt::NoButton, Qt::LeftButton);
        const auto preview = VNX::FilterAuthoring::effectiveState(*item_, scope).sections[2];
        ASSERT_NE(preview.frequency, original.frequency);
        ASSERT_NE(preview.gain, original.gain);
        EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(canvas, &escape);
        mouse(QEvent::MouseButtonRelease, moved, Qt::LeftButton, Qt::NoButton);
        EXPECT_EQ(authoredState(), before);
        EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());
        mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, moved, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, moved, Qt::LeftButton, Qt::NoButton);
        const auto after = authoredState();
        ASSERT_NE(after, before);
        EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
        ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
        EXPECT_EQ(authoredState(), before);
        EXPECT_FALSE(VNX::UndoRedoManager::instance().canUndo());
        ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
        EXPECT_EQ(authoredState(), after);
        EXPECT_FALSE(VNX::UndoRedoManager::instance().canRedo());
        ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    }
}

}
