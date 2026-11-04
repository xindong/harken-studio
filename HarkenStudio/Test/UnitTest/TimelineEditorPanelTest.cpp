#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Widgets/Timeline/TimelineCanvas.h"
#include "Widgets/Timeline/TimelineRuler.h"
#include "Widgets/Timeline/TimelineClipCanvas.h"

#include "ActionManager.h"
#include "Components/VTrack.h"
#include "Components/VSegment.h"
#include "Core/Timeline/TimelineCoordinates.h"
#include "Core/Timeline/TimelineEditing.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "LanguageContextService.h"
#include "UndoCommands.h"
#include "Core/MemorySettingsStorage.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QTemporaryDir>
#include "Core/ToolTip.h"

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
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

void sendMouse(QWidget* widget, QEvent::Type type, const QPoint& position,
               Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, position, widget->mapToGlobal(position), button, buttons, modifiers);
    QApplication::sendEvent(widget, &event);
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
        ASSERT_NE(owner, nullptr);
        owner->comp<VNX::VSegment>()->segmentTime = {};
        owner->comp<VNX::VSegment>()->markers = {};
        track_ = manager.createItem(owner, QStringLiteral("Track"),
                                    VNX::ProjectItem::Type::Track);
        segment_ = manager.createItem(nullptr, QStringLiteral("Segment"),
                                    VNX::ProjectItem::Type::Segment);
        sound_ = manager.createItem(track_, QStringLiteral("Sound"),
                                    VNX::ProjectItem::Type::Sound);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(segment_, nullptr);
        ASSERT_NE(sound_, nullptr);
        segment_->comp<VNX::VSegment>()->segmentTime = {};
        segment_->comp<VNX::VSegment>()->markers = {};
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
    auto invalidClips = track_->comp<VNX::VTrack>()->clips;
    invalidClips.items.front().start = negativeStart;
    EXPECT_FALSE(VNX::TimelineEditing::replaceClips(&manager, track_, invalidClips).success);
    EXPECT_FALSE(panel.commitClipMoves({{track_->guid, track_->guid, guid, negativeStart}}));
    EXPECT_EQ(panel.clipStart(0, 0), distantStart);
    EXPECT_EQ(undo.undoStack()->count(), 0);
    ASSERT_TRUE(panel.moveClip(0, guid, 0, negativeStart, true));
    EXPECT_EQ(panel.viewOriginCoordinate(), 0);
    EXPECT_EQ(panel.clipStart(0, 0), 0);
    EXPECT_EQ(panel.coordinateAtPixel(0, 0), 0);

    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(panel.viewOriginCoordinate(), 0);
    EXPECT_EQ(panel.coordinateAtPixel(0, right), distantStart + duration);
    EXPECT_EQ(panel.snapCoordinate(0, distantStart + 1, false), distantStart);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(panel.viewOriginCoordinate(), 0);
    EXPECT_EQ(panel.clipStart(0, 0), 0);

    ASSERT_TRUE(panel.deleteClip(0, guid));
    EXPECT_EQ(panel.viewOriginCoordinate(), 0);
    EXPECT_EQ(panel.coordinateAtPixel(0, right), initialEnd);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(panel.viewOriginCoordinate(), 0);
    EXPECT_EQ(panel.snapCoordinate(0, 1, false), 0);
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

    const int64_t firstStart = panel.clipStart(0, 0);
    const int64_t secondStart = firstStart + duration + 1000000000LL;
    ASSERT_TRUE(panel.placeItem(sound_, 0, secondStart, true));
    const QUuid secondGuid = panel.clipGuid(0, 1);
    const int64_t spacing = secondStart - firstStart;
    const auto groupAuthored = track_->comp<VNX::VTrack>()->clips;
    drainEvents();

    for (const auto modifiers : {Qt::NoModifier, Qt::AltModifier}) {
        SCOPED_TRACE(modifiers == Qt::AltModifier ? "Unsnapped" : "Snapped");
        undo.clear();
        canvas->setSelectedClip(0, guid);
        const QPoint groupFrom(panel.viewportPixelAtCoordinate(0, secondStart + duration / 2),
            panel.laneRect(0).center().y());
        QMouseEvent selectPress(QEvent::MouseButtonPress, groupFrom, canvas->mapToGlobal(groupFrom),
            Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
        QApplication::sendEvent(canvas, &selectPress);
        QMouseEvent selectRelease(QEvent::MouseButtonRelease, groupFrom, canvas->mapToGlobal(groupFrom),
            Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
        QApplication::sendEvent(canvas, &selectRelease);
        ASSERT_EQ(canvas->selectedClipGuids(), (QSet<QUuid>{guid, secondGuid}));

        const QPoint groupTo(-canvas->width(), groupFrom.y());
        QMouseEvent groupPress(QEvent::MouseButtonPress, groupFrom, canvas->mapToGlobal(groupFrom),
            Qt::LeftButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(canvas, &groupPress);
        QMouseEvent groupMove(QEvent::MouseMove, groupTo, canvas->mapToGlobal(groupTo),
            Qt::NoButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(canvas, &groupMove);

        ASSERT_TRUE(canvas->isGestureActive());
        EXPECT_EQ(canvas->previewStart(), spacing);
        EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, groupAuthored);
        EXPECT_EQ(undo.undoStack()->count(), 0);
        canvas->repaint();
        const auto& visuals = canvas->lastRenderData().clipItems;
        ASSERT_EQ(visuals.size(), 2);
        for (const auto& visual : visuals) {
            const int64_t expectedStart = visual.guid == guid ? 0 : spacing;
            EXPECT_EQ(visual.rect.left(), panel.viewportPixelAtCoordinate(0, expectedStart));
        }

        QMouseEvent groupRelease(QEvent::MouseButtonRelease, groupTo, canvas->mapToGlobal(groupTo),
            Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(canvas, &groupRelease);

        EXPECT_FALSE(canvas->isGestureActive());
        const int firstIndex = panel.findClipIndex(0, guid);
        const int secondIndex = panel.findClipIndex(0, secondGuid);
        ASSERT_GE(firstIndex, 0);
        ASSERT_GE(secondIndex, 0);
        EXPECT_EQ(panel.clipStart(0, firstIndex), 0);
        EXPECT_EQ(panel.clipStart(0, secondIndex), spacing);
        EXPECT_EQ(panel.clipDuration(0, firstIndex), duration);
        EXPECT_EQ(panel.clipDuration(0, secondIndex), duration);
        EXPECT_EQ(undo.undoStack()->count(), 1);
        const auto groupCommitted = track_->comp<VNX::VTrack>()->clips;
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, groupAuthored);
        ASSERT_TRUE(undo.redo());
        EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, groupCommitted);
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, groupAuthored);
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

TEST_F(TimelineEditorPanelTest, ClockChangesPreserveCoordinateMeaningAndViewportHistory)
{
    auto& undo = VNX::UndoRedoManager::instance();
    track_->parent->comp<VNX::VSegment>()->markers.items.push_back(
        {QUuid::createUuid(), VNX::MarkerKind::Custom, QStringLiteral("Point"), 2000000000LL});
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 1000000000LL, true));
    panel.setEditCursor(3000000000LL, true);
    drainEvents();
    const auto absolute = panel.effectiveTimeSpec(0);
    const auto absoluteState = panel.snapshotState();
    const int64_t start = panel.clipStart(0, 0);
    const int64_t duration = panel.clipDuration(0, 0);
    const int64_t marker = panel.markerPosition(0);
    const int64_t cursor = panel.editCursor();
    undo.clear();

    ASSERT_TRUE(panel.setTimelineDomain(VNX::TimelineDomain::Musical));
    drainEvents();
    const auto musical = panel.effectiveTimeSpec(0);
    const auto toAbsolute = [&](int64_t coordinate) {
        const auto converted = VNX::TimelineCoordinates::convert(coordinate, musical, absolute);
        EXPECT_TRUE(converted.ok());
        return converted.value;
    };
    EXPECT_EQ(toAbsolute(panel.clipStart(0, 0)), start);
    EXPECT_EQ(toAbsolute(panel.clipDuration(0, 0)), duration);
    EXPECT_EQ(toAbsolute(panel.markerPosition(0)), marker);
    EXPECT_EQ(toAbsolute(panel.editCursor()), cursor);
    ASSERT_FALSE(panel.timeTicks().ticks.isEmpty());
    const int64_t cursorTick = panel.editCursor();
    EXPECT_EQ(panel.coordinateAtViewportPixel(0,
        panel.viewportPositionAtCoordinate(0, cursorTick)), cursorTick);
    panel.setZoomPercent(400.0);
    panel.panHorizontally(100.0);
    const auto musicalState = panel.snapshotState();
    const auto left = panel.coordinateAtViewportPixel(0, 0);
    panel.fitClips();
    panel.restoreState(musicalState);
    EXPECT_EQ(panel.coordinateAtViewportPixel(0, 0), left);
    EXPECT_EQ(panel.editCursor(), cursorTick);
    panel.restoreState(absoluteState);
    EXPECT_EQ(panel.editCursor(), cursorTick);

    ASSERT_TRUE(undo.undo());
    drainEvents();
    EXPECT_EQ(panel.timelineDomain(), VNX::TimelineDomain::Absolute);
    EXPECT_EQ(panel.clipStart(0, 0), start);
    EXPECT_EQ(panel.markerPosition(0), marker);
    EXPECT_EQ(panel.editCursor(), cursor);
    ASSERT_TRUE(undo.redo());
    drainEvents();
    EXPECT_EQ(panel.timelineDomain(), VNX::TimelineDomain::Musical);
    EXPECT_EQ(panel.editCursor(), cursorTick);
}

TEST_F(TimelineEditorPanelTest, AxisPresentationPreservesAuthoredCoordinatesAndViewportState)
{
    auto& undo = VNX::UndoRedoManager::instance();
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.setTimelineDomain(VNX::TimelineDomain::Musical));
    ASSERT_TRUE(panel.setTempoMicrosecondsPerQuarter(487805));
    ASSERT_TRUE(panel.setMeter(7, 8));
    const auto spec = panel.effectiveTimeSpec(0);
    const auto start = VNX::TimelineCoordinates::fromSeconds(1.23, spec);
    const auto end = VNX::TimelineCoordinates::fromSeconds(10.0, spec);
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(end.ok());
    ASSERT_TRUE(panel.placeItem(sound_, 0, start.value, true));
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Entry, 0));
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Exit, end.value));
    panel.setEditCursor(start.value + 137, true);
    drainEvents();
    panel.setZoomPercent(250.0);
    panel.panHorizontally(137.25);
    const auto authoredTrack = track_->comp<VNX::VTrack>()->toJson();
    const auto authoredSegment = track_->parent->comp<VNX::VSegment>()->toJson();
    const auto cursor = panel.editCursor();
    const auto zoom = panel.zoomPercent();
    const auto scroll = panel.horizontalScroll();
    const auto left = panel.coordinateAtViewportPixel(0, 0.0);
    const auto center = panel.coordinateAtViewportPixel(0, panel.timelineViewportWidth() / 2.0);
    const auto clipPixel = panel.viewportPositionAtCoordinate(0, start.value);
    undo.clear();

    for (const auto mode : {VNX::TimelineEditorPanel::AxisMode::Clock,
                           VNX::TimelineEditorPanel::AxisMode::Time}) {
        panel.setAxisMode(mode);
        ASSERT_FALSE(panel.timeTicks().ticks.isEmpty());
        const auto state = panel.snapshotState();
        panel.setAxisMode(mode == VNX::TimelineEditorPanel::AxisMode::Time
            ? VNX::TimelineEditorPanel::AxisMode::Clock : VNX::TimelineEditorPanel::AxisMode::Time);
        panel.fitClips();
        panel.setEditCursor(0, true);
        panel.restoreState(state);
        drainEvents();
        EXPECT_EQ(panel.axisMode(), mode);
        EXPECT_EQ(track_->comp<VNX::VTrack>()->toJson(), authoredTrack);
        EXPECT_EQ(track_->parent->comp<VNX::VSegment>()->toJson(), authoredSegment);
        EXPECT_EQ(panel.editCursor(), cursor);
        EXPECT_DOUBLE_EQ(panel.zoomPercent(), zoom);
        EXPECT_DOUBLE_EQ(panel.horizontalScroll(), scroll);
        EXPECT_EQ(panel.coordinateAtViewportPixel(0, 0.0), left);
        EXPECT_EQ(panel.coordinateAtViewportPixel(0, panel.timelineViewportWidth() / 2.0), center);
        EXPECT_DOUBLE_EQ(panel.viewportPositionAtCoordinate(0, start.value), clipPixel);
        EXPECT_EQ(undo.undoStack()->count(), 0);
    }
}

class TimelineDomainTest : public TimelineEditorPanelTest,
                           public ::testing::WithParamInterface<VNX::TimelineDomain>
{
};

INSTANTIATE_TEST_SUITE_P(Clocks, TimelineDomainTest,
    ::testing::Values(VNX::TimelineDomain::Absolute, VNX::TimelineDomain::Musical),
    [](const ::testing::TestParamInfo<VNX::TimelineDomain>& info) {
        return info.param == VNX::TimelineDomain::Absolute ? "Absolute" : "Musical";
    });

TEST_P(TimelineDomainTest, CursorAndViewportStayWithinEditableRange)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.setTimelineDomain(GetParam()));
    const int64_t unit = VNX::TimelineSemantics::coordinateUnitFor(panel.effectiveTimeSpec(0));
    drainEvents();
    VNX::UndoRedoManager::instance().clear();

    for (const auto mode : {VNX::TimelineEditorPanel::AxisMode::Time,
                           VNX::TimelineEditorPanel::AxisMode::Clock}) {
        panel.setAxisMode(mode);
        for (const bool disableSnap : {false, true}) {
            panel.setEditCursor(-unit, disableSnap);
            EXPECT_EQ(panel.editCursor(), 0);
            panel.setEditCursor(unit * 2, true);
            panel.beginCursorDrag(panel.viewportPixelAtCoordinate(0, unit * 2), disableSnap, false);
            panel.updateCursorDrag(-100, disableSnap);
            EXPECT_EQ(panel.editCursor(), 0);
            panel.endCursorDrag(true);
            EXPECT_EQ(panel.editCursor(), unit * 2);
        }
        panel.setZoomPercent(200.0);
        panel.panHorizontally(100.0);
        panel.panHorizontally(-1000000.0);
        EXPECT_EQ(panel.coordinateAtViewportPixel(0, 0), 0);
        auto state = panel.snapshotState();
        state.insert(QStringLiteral("cursor"), QString::number(-unit));
        auto viewport = state.value(QStringLiteral("timelineViewport")).toMap();
        viewport.insert(QStringLiteral("startCoordinate"), QString::number(-unit));
        state.insert(QStringLiteral("timelineViewport"), viewport);
        panel.restoreState(state);
        drainEvents();
        EXPECT_EQ(panel.editCursor(), 0);
        EXPECT_EQ(panel.coordinateAtViewportPixel(0, 0), 0);
        EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
    }
}

TEST_P(TimelineDomainTest, MarkerDragPreviewCommitsOnceAndSharesViewportCoordinates)
{
    auto& undo = VNX::UndoRedoManager::instance();
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 480);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.setTimelineDomain(GetParam()));
    if (GetParam() == VNX::TimelineDomain::Musical)
        ASSERT_TRUE(panel.setMeter(7, 8));
    const auto spec = panel.effectiveTimeSpec(0);
    const int64_t unit = VNX::TimelineSemantics::coordinateUnitFor(spec);
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Entry, unit * 2));
    ASSERT_TRUE(panel.placeItem(sound_, 0, 0, true));
    ASSERT_TRUE(panel.resizeClip(0, panel.clipGuid(0, 0), 0, unit * 6, true));
    panel.setEditCursor(unit * 8, true);
    drainEvents();
    undo.clear();
    auto* ruler = panel.findChild<VNX::TimelineMarkerRuler*>("timelineMarkerRuler");
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    ASSERT_NE(ruler, nullptr);
    ASSERT_NE(canvas, nullptr);
    ASSERT_TRUE(ruler->isVisible());
    for (const auto& tick : panel.timeTicks().ticks) {
        const auto coordinate = GetParam() == VNX::TimelineDomain::Musical
            ? VNX::TimelineCoordinates::beatsToTicks(tick.value)
            : VNX::TimelineCoordinates::fromSeconds(tick.value, {});
        ASSERT_TRUE(coordinate.ok());
        EXPECT_EQ(panel.snapCoordinate(0, coordinate.value, false), coordinate.value);
    }
    const auto before = track_->parent->comp<VNX::VSegment>()->markers;
    const auto clips = track_->comp<VNX::VTrack>()->clips;
    const QUuid marker = panel.markerGuid(0);
    const int clipY = panel.laneRect(0).center().y();
    const int blankY = canvas->height() - 16;
    ASSERT_GT(blankY, panel.laneRect(0).bottom());
    for (int surface = 0; surface < 3; ++surface) {
        SCOPED_TRACE(surface);
        undo.clear();
        QWidget* widget = surface == 0 ? static_cast<QWidget*>(ruler) : canvas;
        const int y = surface == 0 ? ruler->height() / 2 : surface == 1 ? clipY : blankY;
        const QPoint from(panel.viewportPixelAtCoordinate(0, unit * 2), y);
        const QPoint to(panel.viewportPixelAtCoordinate(0, unit * 4), y);
        sendMouse(widget, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
        EXPECT_TRUE(VNX::ToolTip::text().contains(QStringLiteral("Entry")));
        EXPECT_EQ(widget->cursor().shape(), Qt::SizeHorCursor);
        sendMouse(widget, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        ASSERT_TRUE(panel.isMarkerDragActive());
        ASSERT_FALSE(panel.isCursorDragActive());
        auto* value = panel.findChild<QLabel*>("timelineMarkerValue");
        ASSERT_NE(value, nullptr);
        EXPECT_TRUE(value->isVisible());
        sendMouse(widget, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
        ASSERT_TRUE(panel.isMarkerDragging());
        const auto preview = panel.markerDragPreviewPosition();
        EXPECT_EQ(preview, panel.coordinateAtViewportPixel(0, to.x()));
        EXPECT_EQ(panel.markerDisplayPosition(0), preview);
        EXPECT_EQ(track_->parent->comp<VNX::VSegment>()->markers, before);
        EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, clips);
        EXPECT_EQ(undo.undoStack()->count(), 0);
        EXPECT_TRUE(value->text().contains(QStringLiteral("Entry")));
        sendMouse(widget, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
        EXPECT_FALSE(panel.isMarkerDragActive());
        EXPECT_FALSE(value->isVisible());
        EXPECT_EQ(panel.markerPosition(0), preview);
        EXPECT_EQ(undo.undoStack()->count(), 1);
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(track_->parent->comp<VNX::VSegment>()->markers, before);
        ASSERT_TRUE(undo.redo());
        EXPECT_EQ(panel.markerPosition(0), preview);
        ASSERT_TRUE(undo.undo());
        undo.clear();

        sendMouse(widget, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        sendMouse(widget, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
        QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(widget, &cancel);
        EXPECT_FALSE(panel.isMarkerDragActive());
        EXPECT_FALSE(value->isVisible());
        EXPECT_EQ(panel.markerDisplayPosition(0), unit * 2);
        EXPECT_EQ(track_->parent->comp<VNX::VSegment>()->markers, before);
        EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, clips);
        EXPECT_EQ(undo.undoStack()->count(), 0);

        sendMouse(widget, QEvent::MouseButtonDblClick, from, Qt::LeftButton, Qt::LeftButton);
        auto* editor = panel.findChild<QDialog*>("timelineMarkerEditor");
        ASSERT_NE(editor, nullptr);
        auto* spin = editor->findChild<QDoubleSpinBox*>("timelineMarkerPositionSpin");
        ASSERT_NE(spin, nullptr);
        const double divisor = GetParam() == VNX::TimelineDomain::Musical
            ? VNX::TimelineCoordinates::kPpq : VNX::TimelineCoordinates::kNanosecondsPerSecond;
        spin->setValue(double(unit * 3) / divisor);
        editor->accept();
        drainEvents();
        EXPECT_EQ(panel.markerPosition(0), unit * 3);
        EXPECT_EQ(undo.undoStack()->count(), 1);
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(track_->parent->comp<VNX::VSegment>()->markers, before);
    }
    undo.clear();
    ASSERT_TRUE(panel.moveMarker(marker, 0, true));
    undo.clear();
    panel.setEditCursor(0, true);
    const QPoint overlap(panel.viewportPixelAtCoordinate(0, 0), clipY);
    sendMouse(canvas, QEvent::MouseButtonPress, overlap, Qt::LeftButton, Qt::LeftButton);
    EXPECT_TRUE(panel.isCursorDragActive());
    EXPECT_FALSE(panel.isMarkerDragActive());
    QKeyEvent cancelCursor(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &cancelCursor);
    const QPoint entryGrip(panel.viewportPixelAtCoordinate(0, 0) + 9, 4);
    sendMouse(ruler, QEvent::MouseButtonPress, entryGrip, Qt::LeftButton, Qt::LeftButton);
    EXPECT_TRUE(panel.isMarkerDragActive());
    EXPECT_FALSE(panel.isCursorDragActive());
    QKeyEvent cancelMarker(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(ruler, &cancelMarker);
    EXPECT_FALSE(panel.isMarkerDragActive());
    EXPECT_EQ(panel.editCursor(), 0);
    EXPECT_EQ(panel.markerPosition(0), 0);
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, clips);
    EXPECT_EQ(undo.undoStack()->count(), 0);
    VNX::ToolTip::hideText();
}

TEST_P(TimelineDomainTest, CursorLineInteractionsShareStateAcrossSurfaces)
{
    auto& undo = VNX::UndoRedoManager::instance();
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 480);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.setTimelineDomain(GetParam()));
    const int64_t unit = VNX::TimelineSemantics::coordinateUnitFor(panel.effectiveTimeSpec(0));
    ASSERT_TRUE(panel.placeItem(sound_, 0, 0, true));
    ASSERT_TRUE(panel.resizeClip(0, panel.clipGuid(0, 0), 0, unit * 6, true));
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Entry, unit * 2));
    drainEvents();
    undo.clear();
    const auto authored = track_->comp<VNX::VTrack>()->clips;
    const auto markers = track_->parent->comp<VNX::VSegment>()->markers;
    auto* ruler = panel.findChild<VNX::TimelineRuler*>("timelineRuler");
    auto* band = panel.findChild<VNX::TimelineMarkerRuler*>("timelineMarkerRuler");
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    ASSERT_NE(ruler, nullptr);
    ASSERT_NE(band, nullptr);
    ASSERT_NE(canvas, nullptr);
    for (int surface = 0; surface < 4; ++surface) {
        SCOPED_TRACE(surface);
        panel.setEditCursor(unit * 4, true);
        QWidget* widget = surface == 0 ? static_cast<QWidget*>(ruler)
            : surface == 1 ? static_cast<QWidget*>(band) : canvas;
        const int offset = surface == 0 ? panel.laneHeaderWidth() : 0;
        const int y = surface == 0 ? band->y() - 3 : surface == 1 ? band->height() / 2
            : surface == 2 ? panel.laneRect(0).center().y() : canvas->height() - 16;
        const QPoint from(offset + panel.viewportPixelAtCoordinate(0, unit * 4), y);
        const QPoint to(offset + panel.viewportPixelAtCoordinate(0, unit * 5), y);
        sendMouse(widget, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
        EXPECT_TRUE(panel.isCursorHovered());
        EXPECT_TRUE(VNX::ToolTip::text().contains(QStringLiteral("Cursor")));
        EXPECT_EQ(widget->cursor().shape(), Qt::SizeHorCursor);
        sendMouse(widget, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        ASSERT_TRUE(panel.isCursorDragActive());
        EXPECT_FALSE(panel.isMarkerDragActive());
        auto* value = panel.findChild<QLabel*>("timelineCursorValue");
        ASSERT_NE(value, nullptr);
        EXPECT_TRUE(value->isVisible());
        sendMouse(widget, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
        EXPECT_EQ(panel.editCursor(), panel.coordinateAtViewportPixel(0, to.x() - offset));
        EXPECT_TRUE(value->text().contains(QStringLiteral("Cursor")));
        QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(widget, &cancel);
        EXPECT_FALSE(panel.isCursorDragActive());
        EXPECT_FALSE(value->isVisible());
        EXPECT_EQ(panel.editCursor(), unit * 4);
        EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, authored);
        EXPECT_EQ(track_->parent->comp<VNX::VSegment>()->markers, markers);
        EXPECT_EQ(undo.undoStack()->count(), 0);

        sendMouse(widget, QEvent::MouseButtonDblClick, from, Qt::LeftButton, Qt::LeftButton);
        auto* editor = panel.findChild<QDialog*>("timelineCursorValueEditor");
        ASSERT_NE(editor, nullptr);
        auto* spin = editor->findChild<QDoubleSpinBox*>("timelineCursorValueInput");
        ASSERT_NE(spin, nullptr);
        const double divisor = GetParam() == VNX::TimelineDomain::Musical
            ? VNX::TimelineCoordinates::kPpq : VNX::TimelineCoordinates::kNanosecondsPerSecond;
        spin->setValue(double(unit * 5) / divisor);
        editor->accept();
        drainEvents();
        EXPECT_EQ(panel.editCursor(), unit * 5);
        EXPECT_EQ(undo.undoStack()->count(), 0);
    }
    VNX::ToolTip::hideText();
}
