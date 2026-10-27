#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Widgets/Timeline/TimelineCanvas.h"
#include "Widgets/Timeline/TimelineRuler.h"

#include "ActionManager.h"
#include "Components/VTrack.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "LanguageContextService.h"
#include "UndoCommands.h"
#include "Core/MemorySettingsStorage.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QTemporaryDir>

#include <memory>
#include <vector>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void drainEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

}

class TimelineEditorPanelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::LanguageContextService::instance().setStorageForTesting(
            std::make_unique<VNX::MemorySettingsStorage>());
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &manager, nullptr);

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("TimelinePanelProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));

        auto* owner = manager.createItem(nullptr, QStringLiteral("TrackPiece"), VNX::ProjectItem::Type::Piece);
        track_ = manager.createItem(owner, QStringLiteral("Track"),
                                    VNX::ProjectItem::Type::Track);
        piece_ = manager.createItem(nullptr, QStringLiteral("Piece"),
                                    VNX::ProjectItem::Type::Piece);
        sound_ = manager.createItem(track_, QStringLiteral("Sound"),
                                    VNX::ProjectItem::Type::Sound);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(piece_, nullptr);
        ASSERT_NE(sound_, nullptr);
        track_->comp<VNX::VTrack>()->clips = {};
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::LanguageContextService::instance().setStorageForTesting(
            VNX::SettingsStorageFactory::create(
                VNX::SettingsStorageFactory::StorageType::QSettings,
                QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* track_ = nullptr;
    VNX::ProjectItem* piece_ = nullptr;
    VNX::ProjectItem* sound_ = nullptr;
};

class TimelineTrackHostTest : public TimelineEditorPanelTest,
                              public ::testing::WithParamInterface<bool>
{
protected:
    void SetUp() override
    {
        TimelineEditorPanelTest::SetUp();
        if (HasFatalFailure() || GetParam()) return;
        auto& manager = VNX::ProjectManager::instance();
        auto* layer = manager.createItem(nullptr, QStringLiteral("StandaloneTracks"),
            VNX::ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        ASSERT_TRUE(manager.moveItem(track_, layer));
        VNX::UndoRedoManager::instance().clear();
    }
};

INSTANTIATE_TEST_SUITE_P(TimelineHosts, TimelineTrackHostTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Piece" : "Standalone"; });

TEST_F(TimelineEditorPanelTest, DeletingBoundItemClearsBinding)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    ASSERT_EQ(panel.boundItem(), piece_);

    ASSERT_TRUE(manager.deleteItem(piece_));
    EXPECT_EQ(panel.boundItem(), nullptr);
}

TEST_P(TimelineTrackHostTest, TrackCursorDragEndsBeforePanelDestruction)
{
    for (const bool handle : {false, true}) {
        auto panel = std::make_unique<VNX::TimelineEditorPanel>();
        panel->setEditCursorVisible(true);
        panel->resize(900, 360);
        panel->bindItem(track_);
        panel->show();
        drainEvents();
        panel->setZoomPercent(200);
        panel->setEditCursor(5000000000LL, true);
        auto* canvas = panel->findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
        auto* ruler = panel->findChild<VNX::TimelineRuler*>("timelineRuler");
        ASSERT_NE(canvas, nullptr);
        ASSERT_NE(ruler, nullptr);
        QPointer<QLabel> value = panel->findChild<QLabel*>("timelineCursorValue");
        QWidget* widget = handle ? static_cast<QWidget*>(ruler) : canvas;
        const int offset = handle ? panel->laneHeaderWidth() : 0;
        const int y = handle ? ruler->cursorHandleRect().center().y() : canvas->height() / 2;
        const QPoint from(offset + panel->pixelAtCoordinate(0, panel->editCursor()), y);
        const QPoint to(offset + canvas->width() + 50, y);
        QMouseEvent press(QEvent::MouseButtonPress, from, widget->mapToGlobal(from),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        QMouseEvent move(QEvent::MouseMove, to, widget->mapToGlobal(to),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &move);
        ASSERT_TRUE(panel->isCursorDragActive());
        ASSERT_FALSE(value.isNull());
        panel.reset();
        drainEvents();
        EXPECT_TRUE(value.isNull());
    }
}

TEST_F(TimelineEditorPanelTest, BatchCommitFailureRollsBackWithoutUndoStep)
{
    VNX::TimelineEditorPanel panel;
    panel.bindItem(track_);
    ASSERT_TRUE(panel.placeItem(sound_, 0, 0, true));
    const int64_t originalStart = panel.clipStart(0, 0);
    const int64_t originalDuration = panel.clipDuration(0, 0);
    VNX::UndoRedoManager::instance().clear();
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();

    bool firstUndone = false;
    std::vector<VNX::ActionResult> results(2);
    results[0].success = true;
    results[0].undoFn = [&firstUndone, track = track_, originalStart]() {
        firstUndone = true;
        auto* timeline = track->comp<VNX::VTrack>();
        ASSERT_NE(timeline, nullptr);
        ASSERT_FALSE(timeline->clips.items.empty());
        timeline->clips.items.front().start = originalStart;
    };
    results[1].success = false;
    results[1].errorMessage = QStringLiteral("forced batch failure");
    track_->comp<VNX::VTrack>()->clips.items.front().start = originalStart + 123;

    EXPECT_FALSE(panel.commitTimelineActions(std::move(results), QStringLiteral("Move Clips")));
    EXPECT_TRUE(firstUndone);
    EXPECT_EQ(panel.clipStart(0, 0), originalStart);
    EXPECT_EQ(panel.clipDuration(0, 0), originalDuration);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}
