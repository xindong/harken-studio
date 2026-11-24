#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Widgets/Timeline/TimelineCanvas.h"
#include "Widgets/Timeline/TimelineRuler.h"

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
    static char arg0[] = "harken_studio_tests";
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
        auto& manager = harken::studio::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        harken::studio::LanguageContextService::instance().setStorageForTesting(
            std::make_unique<harken::studio::MemorySettingsStorage>());
        harken::studio::ActionManager::instance().clearAllForTesting();
        harken::studio::registerProjectActions(harken::studio::ActionManager::instance(), &manager, nullptr);

        ASSERT_TRUE(tempDir_.isValid());
        harken::studio::ProjectConfig config;
        config.name = QStringLiteral("TimelinePanelProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));

        auto* owner = manager.createItem(nullptr, QStringLiteral("TrackSegment"), harken::studio::ProjectItem::Type::Segment);
        ASSERT_NE(owner, nullptr);
        track_ = manager.createItem(owner, QStringLiteral("Track"),
                                    harken::studio::ProjectItem::Type::Track);
        segment_ = manager.createItem(nullptr, QStringLiteral("Segment"),
                                    harken::studio::ProjectItem::Type::Segment);
        sound_ = manager.createItem(track_, QStringLiteral("Sound"),
                                    harken::studio::ProjectItem::Type::Sound);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(segment_, nullptr);
        ASSERT_NE(sound_, nullptr);
        track_->comp<harken::studio::VTrack>()->clips = {};
        harken::studio::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        harken::studio::UndoRedoManager::instance().clear();
        auto& manager = harken::studio::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        harken::studio::ActionManager::instance().clearAllForTesting();
        harken::studio::LanguageContextService::instance().setStorageForTesting(
            harken::studio::SettingsStorageFactory::create(
                harken::studio::SettingsStorageFactory::StorageType::QSettings,
                QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    QTemporaryDir tempDir_;
    harken::studio::ProjectItem* track_ = nullptr;
    harken::studio::ProjectItem* segment_ = nullptr;
    harken::studio::ProjectItem* sound_ = nullptr;
};

class TimelineTrackHostTest : public TimelineEditorPanelTest,
                              public ::testing::WithParamInterface<bool>
{
protected:
    void SetUp() override
    {
        TimelineEditorPanelTest::SetUp();
        if (HasFatalFailure() || GetParam()) return;
        auto& manager = harken::studio::ProjectManager::instance();
        auto* layer = manager.createItem(nullptr, QStringLiteral("StandaloneTracks"),
            harken::studio::ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        ASSERT_TRUE(manager.moveItem(track_, layer));
        harken::studio::UndoRedoManager::instance().clear();
    }
};

INSTANTIATE_TEST_SUITE_P(TimelineHosts, TimelineTrackHostTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Segment" : "Standalone"; });

TEST_F(TimelineEditorPanelTest, DeletingBoundItemClearsBinding)
{
    auto& manager = harken::studio::ProjectManager::instance();
    harken::studio::TimelineEditorPanel panel;
    panel.bindItem(segment_);
    ASSERT_EQ(panel.boundItem(), segment_);

    ASSERT_TRUE(manager.deleteItem(segment_));
    EXPECT_EQ(panel.boundItem(), nullptr);
}

TEST_P(TimelineTrackHostTest, TrackCursorDragEndsBeforePanelDestruction)
{
    for (const bool handle : {false, true}) {
        auto panel = std::make_unique<harken::studio::TimelineEditorPanel>();
        panel->setEditCursorVisible(true);
        panel->resize(900, 360);
        panel->bindItem(track_);
        panel->show();
        drainEvents();
        panel->setZoomPercent(200);
        panel->setEditCursor(5000000000LL, true);
        auto* canvas = panel->findChild<harken::studio::TimelineCanvas*>("timelineLaneViewport");
        auto* ruler = panel->findChild<harken::studio::TimelineRuler*>("timelineRuler");
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
    auto& manager = harken::studio::ProjectManager::instance();
    auto& undo = harken::studio::UndoRedoManager::instance();
    harken::studio::TimelineEditorPanel panel;
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
    track_->comp<harken::studio::VTrack>()->clips.items.front().start = distantStart;
    emit manager.itemPropertyChanged(track_, QStringLiteral("clips"));

    EXPECT_EQ(panel.coordinateAtPixel(0, right), distantStart + duration);
    EXPECT_EQ(panel.snapCoordinate(0, distantStart + 1, false), distantStart);
    const int64_t negativeStart = -initialEnd / 2;
    auto invalidClips = track_->comp<harken::studio::VTrack>()->clips;
    invalidClips.items.front().start = negativeStart;
    EXPECT_FALSE(harken::studio::TimelineEditing::replaceClips(&manager, track_, invalidClips).success);
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
    auto& manager = harken::studio::ProjectManager::instance();
    harken::studio::registerContainerActions(harken::studio::ActionManager::instance(), &manager);
    auto* otherTrack = manager.createItem(track_->parent, QStringLiteral("OtherTrack"),
        harken::studio::ProjectItem::Type::Track);
    ASSERT_NE(otherTrack, nullptr);
    auto* otherSound = manager.createItem(otherTrack, QStringLiteral("OtherSound"),
        harken::studio::ProjectItem::Type::Sound);
    ASSERT_NE(otherSound, nullptr);
    otherTrack->comp<harken::studio::VTrack>()->clips = {};
    const QUuid firstTrackGuid = track_->guid;
    const QUuid otherTrackGuid = otherTrack->guid;

    harken::studio::TimelineEditorPanel panel;
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
    auto& undo = harken::studio::UndoRedoManager::instance();
    harken::studio::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.setEditCursorVisible(false);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 1000000000LL, true));
    drainEvents();
    undo.clear();
    auto* canvas = panel.findChild<harken::studio::TimelineCanvas*>("timelineLaneViewport");
    ASSERT_NE(canvas, nullptr);
    const auto authored = track_->comp<harken::studio::VTrack>()->clips;
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
    EXPECT_EQ(track_->comp<harken::studio::VTrack>()->clips, authored);
    EXPECT_EQ(undo.undoStack()->count(), 0);
    QMouseEvent release(QEvent::MouseButtonRelease, to, canvas->mapToGlobal(to),
        Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &release);

    EXPECT_FALSE(canvas->isGestureActive());
    EXPECT_EQ(panel.clipGuid(0, 0), guid);
    EXPECT_EQ(panel.clipStart(0, 0), preview);
    EXPECT_EQ(panel.clipDuration(0, 0), duration);
    EXPECT_EQ(undo.undoStack()->count(), 1);
    const auto committed = track_->comp<harken::studio::VTrack>()->clips;
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(track_->comp<harken::studio::VTrack>()->clips, authored);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(track_->comp<harken::studio::VTrack>()->clips, committed);

    const int64_t firstStart = panel.clipStart(0, 0);
    const int64_t secondStart = firstStart + duration + 1000000000LL;
    ASSERT_TRUE(panel.placeItem(sound_, 0, secondStart, true));
    const QUuid secondGuid = panel.clipGuid(0, 1);
    const int64_t spacing = secondStart - firstStart;
    const auto groupAuthored = track_->comp<harken::studio::VTrack>()->clips;
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
        EXPECT_EQ(track_->comp<harken::studio::VTrack>()->clips, groupAuthored);
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
        const auto groupCommitted = track_->comp<harken::studio::VTrack>()->clips;
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(track_->comp<harken::studio::VTrack>()->clips, groupAuthored);
        ASSERT_TRUE(undo.redo());
        EXPECT_EQ(track_->comp<harken::studio::VTrack>()->clips, groupCommitted);
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(track_->comp<harken::studio::VTrack>()->clips, groupAuthored);
    }
}

TEST_F(TimelineEditorPanelTest, BatchCommitFailureRollsBackWithoutUndoStep)
{
    harken::studio::TimelineEditorPanel panel;
    panel.bindItem(track_);
    ASSERT_TRUE(panel.placeItem(sound_, 0, 0, true));
    const int64_t originalStart = panel.clipStart(0, 0);
    const int64_t originalDuration = panel.clipDuration(0, 0);
    harken::studio::UndoRedoManager::instance().clear();
    const int undoCount = harken::studio::UndoRedoManager::instance().undoStack()->count();

    bool firstUndone = false;
    std::vector<harken::studio::ActionResult> results(2);
    results[0].success = true;
    results[0].undoFn = [&firstUndone, track = track_, originalStart]() {
        firstUndone = true;
        auto* timeline = track->comp<harken::studio::VTrack>();
        ASSERT_NE(timeline, nullptr);
        ASSERT_FALSE(timeline->clips.items.empty());
        timeline->clips.items.front().start = originalStart;
    };
    results[1].success = false;
    results[1].errorMessage = QStringLiteral("forced batch failure");
    track_->comp<harken::studio::VTrack>()->clips.items.front().start = originalStart + 123;

    EXPECT_FALSE(panel.commitTimelineActions(std::move(results), QStringLiteral("Move Clips")));
    EXPECT_TRUE(firstUndone);
    EXPECT_EQ(panel.clipStart(0, 0), originalStart);
    EXPECT_EQ(panel.clipDuration(0, 0), originalDuration);
    EXPECT_EQ(harken::studio::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_F(TimelineEditorPanelTest, CursorAndViewportStayWithinEditableRange)
{
    harken::studio::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    constexpr int64_t unit = harken::studio::TimelineCoordinates::kNanosecondsPerSecond;
    drainEvents();
    harken::studio::UndoRedoManager::instance().clear();

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
    EXPECT_EQ(harken::studio::UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(TimelineEditorPanelTest, CursorLineInteractionsShareStateAcrossSurfaces)
{
    auto& undo = harken::studio::UndoRedoManager::instance();
    harken::studio::TimelineEditorPanel panel;
    panel.resize(900, 480);
    panel.bindItem(track_);
    panel.show();
    constexpr int64_t unit = harken::studio::TimelineCoordinates::kNanosecondsPerSecond;
    ASSERT_TRUE(panel.placeItem(sound_, 0, 0, true));
    ASSERT_TRUE(panel.resizeClip(0, panel.clipGuid(0, 0), 0, unit * 6, true));
    drainEvents();
    undo.clear();
    const auto authored = track_->comp<harken::studio::VTrack>()->clips;
    auto* ruler = panel.findChild<harken::studio::TimelineRuler*>("timelineRuler");
    auto* canvas = panel.findChild<harken::studio::TimelineCanvas*>("timelineLaneViewport");
    ASSERT_NE(ruler, nullptr);
    ASSERT_NE(canvas, nullptr);
    for (int surface = 0; surface < 3; ++surface) {
        SCOPED_TRACE(surface);
        panel.setEditCursor(unit * 4, true);
        QWidget* widget = surface == 0 ? static_cast<QWidget*>(ruler) : canvas;
        const int offset = surface == 0 ? panel.laneHeaderWidth() : 0;
        const int y = surface == 0 ? ruler->height() / 2
            : surface == 1 ? panel.laneRect(0).center().y() : canvas->height() - 16;
        const QPoint from(offset + panel.viewportPixelAtCoordinate(0, unit * 4), y);
        const QPoint to(offset + panel.viewportPixelAtCoordinate(0, unit * 5), y);
        sendMouse(widget, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
        EXPECT_TRUE(panel.isCursorHovered());
        EXPECT_TRUE(harken::studio::ToolTip::text().contains(QStringLiteral("Cursor")));
        EXPECT_EQ(widget->cursor().shape(), Qt::SizeHorCursor);
        sendMouse(widget, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        ASSERT_TRUE(panel.isCursorDragActive());
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
        EXPECT_EQ(track_->comp<harken::studio::VTrack>()->clips, authored);
        EXPECT_EQ(undo.undoStack()->count(), 0);

        sendMouse(widget, QEvent::MouseButtonDblClick, from, Qt::LeftButton, Qt::LeftButton);
        auto* editor = panel.findChild<QDialog*>("timelineCursorValueEditor");
        ASSERT_NE(editor, nullptr);
        auto* spin = editor->findChild<QDoubleSpinBox*>("timelineCursorValueInput");
        ASSERT_NE(spin, nullptr);
        spin->setValue(5.0);
        editor->accept();
        drainEvents();
        EXPECT_EQ(panel.editCursor(), unit * 5);
        EXPECT_EQ(undo.undoStack()->count(), 0);
    }
    harken::studio::ToolTip::hideText();
}
