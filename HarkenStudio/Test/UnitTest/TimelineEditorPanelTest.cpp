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

        auto* owner = manager.createItem(nullptr, QStringLiteral("TrackSegment"), VNX::ProjectItem::Type::Segment);
        track_ = manager.createItem(owner, QStringLiteral("Track"),
                                    VNX::ProjectItem::Type::Track);
        segment_ = manager.createItem(nullptr, QStringLiteral("Segment"),
                                    VNX::ProjectItem::Type::Segment);
        sound_ = manager.createItem(track_, QStringLiteral("Sound"),
                                    VNX::ProjectItem::Type::Sound);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(segment_, nullptr);
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
    VNX::ProjectItem* segment_ = nullptr;
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
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Segment" : "Standalone"; });

TEST_F(TimelineEditorPanelTest, DeletingBoundItemClearsBinding)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TimelineEditorPanel panel;
    panel.bindItem(segment_);
    ASSERT_EQ(panel.boundItem(), segment_);

    ASSERT_TRUE(manager.deleteItem(segment_));
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

TEST_P(TimelineTrackHostTest, CoordinateSpaceTracksModelNotificationsAndUndoRedo)
{
    auto& manager = VNX::ProjectManager::instance();
    auto& undo = VNX::UndoRedoManager::instance();
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 0, true));
    drainEvents();
    undo.clear();

    const int right = panel.timelineViewportWidth() - 1;
    const int64_t initialEnd = panel.coordinateAtPixel(0, right);
    const QUuid guid = panel.clipGuid(0, 0);
    const QUuid targetGuid = panel.clipTargetGuid(0, 0);
    EXPECT_EQ(panel.clipTarget(0, 0), manager.findItemByGuid(targetGuid));
    const int64_t duration = panel.clipDuration(0, 0);
    const int64_t distantStart = initialEnd * 2 + 123456789;
    panel.timeTicks();
    track_->comp<VNX::VTrack>()->clips.items.front().start = distantStart;
    emit manager.itemPropertyChanged(track_, QStringLiteral("clips"));

    EXPECT_EQ(panel.coordinateAtPixel(0, right), distantStart + duration);
    EXPECT_EQ(panel.snapCoordinate(0, distantStart + 1, false), distantStart);
    const int64_t negativeStart = -initialEnd / 2;
    ASSERT_TRUE(panel.moveClip(0, guid, 0, negativeStart, true));
    EXPECT_LE(panel.viewOriginCoordinate(), negativeStart);
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.pixelAtCoordinate(0, negativeStart)), negativeStart);

    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(panel.viewOriginCoordinate(), 0);
    EXPECT_EQ(panel.coordinateAtPixel(0, right), distantStart + duration);
    EXPECT_EQ(panel.snapCoordinate(0, distantStart + 1, false), distantStart);
    ASSERT_TRUE(undo.redo());
    EXPECT_LE(panel.viewOriginCoordinate(), negativeStart);
    EXPECT_EQ(panel.clipStart(0, 0), negativeStart);

    ASSERT_TRUE(panel.deleteClip(0, guid));
    EXPECT_EQ(panel.viewOriginCoordinate(), 0);
    EXPECT_EQ(panel.coordinateAtPixel(0, right), initialEnd);
    ASSERT_TRUE(undo.undo());
    EXPECT_LE(panel.viewOriginCoordinate(), negativeStart);
    EXPECT_EQ(panel.snapCoordinate(0, negativeStart + 1, false), negativeStart);
    EXPECT_EQ(panel.clipTarget(0, 0), manager.findItemByGuid(targetGuid));

    ASSERT_TRUE(manager.deleteItem(sound_));
    EXPECT_EQ(manager.findItemByGuid(targetGuid), nullptr);
    EXPECT_EQ(panel.clipCount(0), 0);
    EXPECT_EQ(panel.clipTarget(0, 0), nullptr);
    ASSERT_TRUE(undo.undo());
    const int restoredClip = panel.findClipIndex(0, guid);
    ASSERT_GE(restoredClip, 0);
    ASSERT_NE(manager.findItemByGuid(targetGuid), nullptr);
    EXPECT_EQ(panel.clipTargetGuid(0, restoredClip), targetGuid);
    EXPECT_EQ(panel.clipTarget(0, restoredClip), manager.findItemByGuid(targetGuid));
}

TEST_F(TimelineEditorPanelTest, TrackReorderingAndDeletionPreserveLiveCoordinateMapping)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::registerContainerActions(VNX::ActionManager::instance(), &manager);
    auto* otherTrack = manager.createItem(track_->parent, QStringLiteral("OtherTrack"),
        VNX::ProjectItem::Type::Track);
    ASSERT_NE(otherTrack, nullptr);
    auto* otherSound = manager.createItem(otherTrack, QStringLiteral("OtherSound"),
        VNX::ProjectItem::Type::Sound);
    ASSERT_NE(otherSound, nullptr);
    otherTrack->comp<VNX::VTrack>()->clips = {};
    const QUuid firstTrackGuid = track_->guid;
    const QUuid otherTrackGuid = otherTrack->guid;

    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_->parent);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 0, true));
    drainEvents();
    const int right = panel.timelineViewportWidth() - 1;
    const int64_t initialEnd = panel.coordinateAtPixel(0, right);
    const int64_t distantStart = initialEnd * 2 + 123456789;
    ASSERT_TRUE(panel.placeItem(otherSound, 1, distantStart, true));
    const QUuid distantClip = panel.clipGuid(1, 0);
    const int64_t distantEnd = distantStart + panel.clipDuration(1, 0);
    EXPECT_EQ(panel.coordinateAtPixel(0, right), distantEnd);

    ASSERT_TRUE(panel.reorderLane(1, 0)) << panel.lastInteractionError().toStdString();
    EXPECT_EQ(panel.laneGuid(0), otherTrackGuid);
    EXPECT_EQ(panel.findLaneOfClip(distantClip), 0);
    EXPECT_EQ(panel.clipStart(0, 0), distantStart);
    EXPECT_EQ(panel.coordinateAtPixel(0, right), distantEnd);
    drainEvents();

    ASSERT_TRUE(manager.deleteItem(otherTrack));
    ASSERT_EQ(panel.laneCount(), 1);
    EXPECT_EQ(panel.laneGuid(0), firstTrackGuid);
    EXPECT_EQ(panel.findLaneOfClip(distantClip), -1);
    EXPECT_EQ(panel.coordinateAtPixel(0, right), initialEnd);
}

TEST_P(TimelineTrackHostTest, DragPreviewPreservesAuthoredStateUntilUndoableCommit)
{
    auto& undo = VNX::UndoRedoManager::instance();
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.setEditCursorVisible(false);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 1000000000LL, true));
    drainEvents();
    undo.clear();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    ASSERT_NE(canvas, nullptr);
    const auto authored = track_->comp<VNX::VTrack>()->clips;
    const QUuid guid = panel.clipGuid(0, 0);
    const int64_t start = panel.clipStart(0, 0);
    const int64_t duration = panel.clipDuration(0, 0);
    const QPoint from(panel.viewportPixelAtCoordinate(0, start + duration / 2),
        panel.laneRect(0).center().y());
    const QPoint to = from + QPoint(canvas->width() / 5, 0);
    QMouseEvent press(QEvent::MouseButtonPress, from, canvas->mapToGlobal(from),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent move(QEvent::MouseMove, to, canvas->mapToGlobal(to),
        Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &move);

    ASSERT_TRUE(canvas->isGestureActive());
    const int64_t preview = canvas->previewStart();
    EXPECT_NE(preview, start);
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, authored);
    EXPECT_EQ(undo.undoStack()->count(), 0);
    QMouseEvent release(QEvent::MouseButtonRelease, to, canvas->mapToGlobal(to),
        Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &release);

    EXPECT_FALSE(canvas->isGestureActive());
    EXPECT_EQ(panel.clipGuid(0, 0), guid);
    EXPECT_EQ(panel.clipStart(0, 0), preview);
    EXPECT_EQ(panel.clipDuration(0, 0), duration);
    EXPECT_EQ(undo.undoStack()->count(), 1);
    const auto committed = track_->comp<VNX::VTrack>()->clips;
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, authored);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, committed);
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
