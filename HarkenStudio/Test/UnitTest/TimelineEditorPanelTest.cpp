#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Widgets/Timeline/TimelineCanvas.h"
#include "Widgets/Timeline/TimelineClipCanvas.h"
#include "Widgets/Timeline/TimelineHeaderColumn.h"
#include "Widgets/Timeline/TimelineRuler.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Timeline/TimelineTicks.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/RefDomainAdapter.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Shared/ProjectItemMime.h"
#include "Widgets/Shared/InlineRenameEdit.h"

#include "ActionManager.h"
#include "AuthoringPolicy.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/ImportAssetsCommand.h"
#include "AssetPipeline/WaveformCache.h"
#include "Components/VAudio.h"
#include "Components/VSound.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VPiece.h"
#include "Components/VTrack.h"
#include "ItemTypeCatalog.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "LanguageContextService.h"
#include "Timeline/TimelineSemantics.h"
#include "UndoCommands.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/QtControlSkin.h"
#include "Core/ShortcutManager.h"
#include "Core/ThemeManager.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QAction>
#include <QKeySequence>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QDragEnterEvent>
#include <QEvent>
#include <QEnterEvent>
#include <QEventLoop>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScrollArea>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <limits>
#include <cmath>
#include <memory>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

bool containsCursorValue(const QLabel* label, const QString& value)
{
    return label->text().contains(QRegularExpression(
        QStringLiteral("(?<![-.0-9])") + QRegularExpression::escape(value)));
}

void drainEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

bool chooseClipTarget(uint32_t objectID)
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* selector = qobject_cast<VNX::SelectorWindow*>(widget);
        if (!selector || !selector->isVisible()) continue;
        auto* tree = selector->findChild<QTreeWidget*>(QStringLiteral("SelectorTreeWidget"));
        if (!tree) return false;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            auto* item = *it;
            if (item->data(0, VNX::kRoleObjectID).toUInt() != objectID) continue;
            if (!item->data(0, VNX::kRoleSelectable).toBool()) return false;
            tree->setCurrentItem(item);
            selector->accept();
            drainEvents();
            return true;
        }
    }
    return false;
}

VNX::ClipLedger makeClipLedger(std::initializer_list<VNX::Clip> clips)
{
    VNX::ClipLedger ledger;
    for (VNX::Clip clip : clips) {
        EXPECT_TRUE(ledger.mintOccurrenceOrdinal(&clip));
        ledger.items.push_back(std::move(clip));
    }
    return ledger;
}

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
}

bool writePcm16Wav(const QString& path, int frames, uint16_t channels, uint32_t sampleRate)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const uint16_t bits = 16;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * (bits / 8));
    const uint32_t dataBytes = static_cast<uint32_t>(frames) * blockAlign;
    const uint32_t byteRate = sampleRate * blockAlign;
    QByteArray bytes;
    bytes.reserve(static_cast<int>(44 + dataBytes));
    bytes.append("RIFF", 4);
    appendU32(bytes, 36 + dataBytes);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    appendU32(bytes, 16);
    appendU16(bytes, 1);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, byteRate);
    appendU16(bytes, blockAlign);
    appendU16(bytes, bits);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    for (int i = 0; i < frames; ++i) {
        const int16_t sample = static_cast<int16_t>(((i % 8) - 4) * 4000);
        for (uint16_t ch = 0; ch < channels; ++ch) {
            bytes.append(static_cast<char>(sample & 0xFF));
            bytes.append(static_cast<char>((sample >> 8) & 0xFF));
        }
    }
    return file.write(bytes) == bytes.size();
}

}

class TimelineEditorPanelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::registerBuiltinInspectorCustomizations();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::LanguageContextService::instance().setStorageForTesting(
            std::make_unique<VNX::MemorySettingsStorage>());
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(), &manager, nullptr, {});

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
        queue_ = manager.createItem(nullptr, QStringLiteral("Queue"),
                                    VNX::ProjectItem::Type::Container);
        container_ = manager.createItem(nullptr, QStringLiteral("NotQueue"),
                                        VNX::ProjectItem::Type::Container);
        sound_ = manager.createItem(track_, QStringLiteral("Sound"),
                                    VNX::ProjectItem::Type::Sound);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(piece_, nullptr);
        ASSERT_NE(queue_, nullptr);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(sound_, nullptr);
        track_->comp<VNX::VTrack>()->clips = {};
        queue_->compOrCreate<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
        queue_->compOrCreate<VNX::VSelection>()->playType = VNX::SelectionPlayType::Queue;
        container_->compOrCreate<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
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

    VNX::ProjectItem* ownedChild(VNX::ProjectItem* track)
    {
        auto& manager = VNX::ProjectManager::instance();
        if (!track) return nullptr;
        for (const auto& child : track->children) {
            if (child && VNX::ItemTypeCatalog::isTimelineClipTarget(child->type))
                return child.get();
        }
        auto* child = manager.createItem(track, QStringLiteral("OwnedClipSound"),
                                         VNX::ProjectItem::Type::Sound);
        track->comp<VNX::VTrack>()->clips = {};
        return child;
    }

    QUuid ownedGuid(VNX::ProjectItem* track)
    {
        VNX::ProjectItem* child = ownedChild(track);
        return child ? child->guid : QUuid();
    }

    bool placeOnLane(VNX::TimelineEditorPanel& panel, VNX::ProjectItem* target,
                     int laneIndex, int64_t start, bool disableSnap = false)
    {
        auto& manager = VNX::ProjectManager::instance();
        VNX::ProjectItem* track = manager.findItemByGuid(panel.laneGuid(laneIndex));
        if (!track && panel.boundItem() && panel.boundItem()->type == VNX::ProjectItem::Type::Track)
            track = panel.boundItem();
        if (!track) return false;
        VNX::ProjectItem* owned = (target && target->parent == track) ? target : ownedChild(track);
        if (!owned) return false;
        return panel.placeItem(owned, laneIndex, start, disableSnap);
    }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* track_ = nullptr;
    VNX::ProjectItem* piece_ = nullptr;
    VNX::ProjectItem* queue_ = nullptr;
    VNX::ProjectItem* container_ = nullptr;
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

TEST_F(TimelineEditorPanelTest, ClipTableAddsRepeatedChildrenAndUndoUpdatesTimeline)
{
    VNX::TimelineEditorPanel timeline;
    timeline.bindItem(track_);
    VNX::InspectorPanel inspector;
    inspector.resize(320, 600);
    inspector.showItemInspector(track_);
    inspector.show();
    drainEvents();
    auto* section = inspector.findChild<QWidget*>("section_clips");
    ASSERT_NE(section, nullptr);
    auto* add = section->findChild<QToolButton*>("listAdd_clips");
    auto* remove = section->findChild<QToolButton*>("listRemoveSelected_clips");
    ASSERT_NE(add, nullptr);
    ASSERT_NE(remove, nullptr);
    for (int count = 1; count <= 2; ++count) {
        add->click();
        ASSERT_TRUE(chooseClipTarget(sound_->objectID));
        drainEvents();
        EXPECT_EQ(timeline.clipCount(0), count);
    }
    const QUuid first = timeline.clipGuid(0, 0);
    const QUuid second = timeline.clipGuid(0, 1);
    EXPECT_NE(first, second);
    EXPECT_EQ(timeline.clipTargetGuid(0, 0), sound_->guid);
    const auto rows = section->findChildren<QWidget*>("clipRow");
    ASSERT_EQ(rows.size(), 2);
    QWidget* row = rows.back();
    const QPointF local = row->rect().center();
    QMouseEvent click(QEvent::MouseButtonPress, local,
        QPointF(row->mapToGlobal(local.toPoint())), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(row, &click);
    ASSERT_TRUE(remove->isEnabled());
    remove->click();
    drainEvents();
    EXPECT_EQ(timeline.clipCount(0), 1);
    EXPECT_EQ(timeline.clipGuid(0, 0), first);
    EXPECT_EQ(VNX::ProjectManager::instance().findItemByGuid(sound_->guid), sound_);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    drainEvents();
    EXPECT_EQ(timeline.clipCount(0), 2);
    EXPECT_EQ(timeline.clipGuid(0, 1), second);
}

TEST_F(TimelineEditorPanelTest, ClipTableReplacementPreservesPlacementIdentityAndUndo)
{
    auto& project = VNX::ProjectManager::instance();
    auto* other = project.createItem(track_, QStringLiteral("OtherChild"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(other, nullptr);
    track_->comp<VNX::VTrack>()->clips.items.clear();
    VNX::TimelineEditorPanel timeline;
    timeline.bindItem(track_);
    ASSERT_TRUE(timeline.placeItem(sound_, 0, 1250000000LL, true));
    drainEvents();
    const auto before = track_->comp<VNX::VTrack>()->clips.items.front();
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(track_);
    auto* reference = inspector.findChild<VNX::ObjectRefWidget*>("clipChildObject");
    ASSERT_NE(reference, nullptr);
    EXPECT_TRUE(reference->canAcceptObject(sound_->objectID));
    EXPECT_TRUE(reference->canAcceptObject(other->objectID));
    EXPECT_FALSE(reference->canAcceptObject(queue_->objectID));
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    reference->openSelector();
    ASSERT_TRUE(chooseClipTarget(sound_->objectID));
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.items.front(), before);
    emit reference->objectChanged(other->objectID);
    drainEvents();
    const auto after = track_->comp<VNX::VTrack>()->clips.items.front();
    EXPECT_EQ(after.targetGuid, other->guid);
    EXPECT_EQ(after.clipGuid, before.clipGuid);
    EXPECT_EQ(after.stableOccurrenceOrdinal, before.stableOccurrenceOrdinal);
    EXPECT_EQ(after.runtimeId(), before.runtimeId());
    EXPECT_EQ(after.start, before.start);
    EXPECT_EQ(after.duration, before.duration);
    EXPECT_EQ(timeline.clipTargetGuid(0, 0), other->guid);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    drainEvents();
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.items.front(), before);
}

TEST_F(TimelineEditorPanelTest, ClipTablePendingSelectorCannotWriteAfterInspectorRebind)
{
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(track_);
    inspector.show();
    drainEvents();
    auto* add = inspector.findChild<QToolButton*>("listAdd_clips");
    ASSERT_NE(add, nullptr);
    add->click();
    inspector.showItemInspector(piece_);
    drainEvents();
    chooseClipTarget(sound_->objectID);
    VNX::SelectorWindow::dismiss();
    EXPECT_TRUE(track_->comp<VNX::VTrack>()->clips.items.empty());
}

TEST_F(TimelineEditorPanelTest, SoundPlacementSeedsAndThenPreservesAuthoredDuration)
{
    auto* sound = sound_->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    VNX::AssetInfo asset;
    asset.sourceID = 9001;
    asset.relativePath = QStringLiteral("trimmed.wav");
    asset.absolutePath = tempDir_.path() + QStringLiteral("/trimmed.wav");
    asset.metadata.sampleRate = 48000;
    asset.metadata.totalSamples = 480000;
    asset.metadata.durationSec = 10.0;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    VNX::SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 96000;
    source.trimEndFrame = 264000;
    sound->sources.plain.entries = {source};
    sound->sources.plain.active = source.entryId;
    sound_->sourceID = asset.sourceID;

    VNX::TimelineEditorPanel trackPanel;
    trackPanel.resize(1000, 420);
    trackPanel.bindItem(track_);
    trackPanel.show();
    ASSERT_TRUE(placeOnLane(trackPanel, sound_, 0, 0));
    drainEvents();
    auto* canvas = trackPanel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    ASSERT_NE(canvas, nullptr);
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    const int seededWidth = canvas->lastRenderData().clipItems.front().rect.width();
    EXPECT_EQ(trackPanel.clipSourceDuration(0, 0), std::optional<int64_t>(3500000000LL));
    EXPECT_EQ(trackPanel.clipDuration(0, 0), 3500000000LL);

    const QUuid clipGuid = trackPanel.clipGuid(0, 0);
    ASSERT_TRUE(trackPanel.resizeClip(0, clipGuid, 0, 1250000000LL, true));
    EXPECT_EQ(trackPanel.clipDuration(0, 0), 1250000000LL);
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.items.front().duration, 1250000000LL);
    EXPECT_EQ(trackPanel.timelineSemantics().content.end, 1250000000LL);
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    const auto shortened = canvas->lastRenderData().clipItems.front();
    EXPECT_LT(shortened.rect.width(), seededWidth);
    EXPECT_EQ(shortened.waveformRect.width(), shortened.rect.width());
    EXPECT_DOUBLE_EQ(shortened.waveformStartFrame, 96000.0);
    EXPECT_DOUBLE_EQ(shortened.waveformFrameCount, 60000.0);

    const auto authored = track_->comp<VNX::VTrack>()->clips.toJsonObject();
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    sound->sources.plain.entries.front().trimEndFrame = 216000;
    emit VNX::ProjectManager::instance().itemPropertyChanged(sound_, QStringLiteral("sources"));
    drainEvents();
    EXPECT_EQ(trackPanel.clipSourceDuration(0, 0), std::optional<int64_t>(2500000000LL));
    EXPECT_EQ(trackPanel.clipDuration(0, 0), 1250000000LL);
    EXPECT_EQ(trackPanel.timelineSemantics().content.end, 1250000000LL);
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.toJsonObject(), authored);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);

    auto* nestedTrack = VNX::ProjectManager::instance().createItem(
        piece_, QStringLiteral("SourceTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(nestedTrack, nullptr);
    auto* nestedSound = VNX::ProjectManager::instance().duplicateItem(sound_);
    ASSERT_NE(nestedSound, nullptr);
    ASSERT_TRUE(VNX::ProjectManager::instance().moveItem(nestedSound, nestedTrack));
    ASSERT_TRUE(VNX::ProjectManager::instance().moveItem(piece_, track_));
    VNX::TimelineEditorPanel piecePanel;
    piecePanel.bindItem(piece_);
    ASSERT_TRUE(piecePanel.moveClip(0, piecePanel.clipGuid(0, 0), 0, 80000000, true));
    ASSERT_TRUE(trackPanel.moveClip(0, trackPanel.clipGuid(0, 1), 0, 240000000, true));
    EXPECT_EQ(trackPanel.clipSourceDuration(0, 1), std::optional<int64_t>(2580000000LL));
    const auto nestedPlacement = track_->comp<VNX::VTrack>()->clips.toJsonObject();
    nestedSound->comp<VNX::VAudio>()->loopCount = 0;
    EXPECT_EQ(trackPanel.clipSourceDuration(0, 0), std::optional<int64_t>(2500000000LL));
    EXPECT_FALSE(trackPanel.clipSourceDuration(0, 1));
    nestedSound->comp<VNX::VAudio>()->loopCount = 1;
    nestedSound->comp<VNX::VMixNode>()->pitch = 12.0f;
    EXPECT_FALSE(trackPanel.clipSourceDuration(0, 1));
    nestedSound->comp<VNX::VMixNode>()->pitch = 0.0f;
    nestedSound->comp<VNX::VSound>()->scheduleOffsetSec = 0.1;
    EXPECT_FALSE(trackPanel.clipSourceDuration(0, 1));
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.toJsonObject(), nestedPlacement);
}

TEST_F(TimelineEditorPanelTest, PieceRowsTrackDirectChildrenAcrossCreateMoveRenameAndDelete)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* otherPiece = manager.createItem(
        nullptr, QStringLiteral("OtherPiece"), VNX::ProjectItem::Type::Piece);
    VNX::ProjectItem* first = manager.createItem(
        piece_, QStringLiteral("First"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(otherPiece, nullptr);
    ASSERT_NE(first, nullptr);

    QObject observer;
    VNX::TimelineEditorPanel* observedPanel = nullptr;
    std::optional<VNX::TimelineSemanticsResult> deletedSpan;
    QObject::connect(&manager, &VNX::ProjectManager::itemDeleted, &observer,
                     [&](VNX::ProjectItem* item) {
        if (item == first && observedPanel)
            deletedSpan = observedPanel->timelineSemantics();
    });
    VNX::TimelineEditorPanel panel;
    observedPanel = &panel;
    panel.bindItem(piece_);
    ASSERT_EQ(panel.laneCount(), 1);
    EXPECT_EQ(panel.laneGuid(0), first->guid);

    VNX::ProjectItem* second = manager.createItem(
        piece_, QStringLiteral("Second"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(panel.laneCount(), 2);

    ASSERT_TRUE(manager.renameItem(second, QStringLiteral("Renamed")));
    EXPECT_EQ(panel.laneName(1), QStringLiteral("Renamed"));

    ASSERT_TRUE(manager.moveItem(second, otherPiece));
    EXPECT_EQ(panel.laneCount(), 1);
    ASSERT_TRUE(manager.moveItem(second, piece_));
    EXPECT_EQ(panel.laneCount(), 2);

    ASSERT_TRUE(manager.deleteItem(first));
    ASSERT_TRUE(deletedSpan);
    EXPECT_TRUE(*deletedSpan);
    EXPECT_EQ(deletedSpan->content, (VNX::TimelineSpan{0, 0}));
    EXPECT_EQ(panel.laneCount(), 1);
    EXPECT_EQ(panel.laneGuid(0), second->guid);
}

TEST_F(TimelineEditorPanelTest, TrackCreationEntriesRevealAndEditTheNewLane)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 500);
    panel.bindItem(piece_);
    panel.show();
    panel.activateWindow();
    drainEvents();
    for (const bool toolbar : {true, false}) {
        if (toolbar) {
            auto* add = panel.findChild<QToolButton*>(QStringLiteral("timelineAddTrackButton"));
            ASSERT_NE(add, nullptr);
            add->click();
        } else {
            auto* canvas = panel.findChild<VNX::TimelineCanvas*>();
            ASSERT_NE(canvas, nullptr);
            auto menu = std::unique_ptr<QMenu>(canvas->createLaneBlankMenu(-1, 0));
            auto* add = menu->findChild<QAction*>(QStringLiteral("timelineAddTrack"));
            ASSERT_NE(add, nullptr);
            add->trigger();
        }
        drainEvents();
        const int lane = panel.laneCount() - 1;
        const QUuid guid = panel.laneGuid(lane);
        const QString initialName = panel.laneName(lane);
        auto* editor = panel.findChild<VNX::InlineRenameEdit*>();
        ASSERT_NE(editor, nullptr);
        EXPECT_TRUE(editor->hasFocus());
        EXPECT_EQ(editor->selectedText(), initialName);
        editor->setText(QStringLiteral("Discarded"));
        ASSERT_TRUE(editor->finish(false));
        EXPECT_EQ(panel.laneName(lane), initialName);
        auto& undo = VNX::UndoRedoManager::instance();
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(VNX::ProjectManager::instance().findItemByGuid(guid), nullptr);
        ASSERT_TRUE(undo.redo());
        drainEvents();
        EXPECT_EQ(panel.laneGuid(lane), guid);
        EXPECT_EQ(panel.findChild<VNX::InlineRenameEdit*>(), nullptr);
    }
}

TEST_F(TimelineEditorPanelTest, LaneMenuRenamesInPlaceAndKeepsInvalidInputEditable)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* track = manager.createItem(piece_, QStringLiteral("First"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    VNX::UndoRedoManager::instance().clear();
    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    panel.resize(800, 400);
    panel.show();
    QApplication::processEvents();
    auto* column = panel.findChild<VNX::TimelineHeaderColumn*>();
    ASSERT_NE(column, nullptr);
    std::unique_ptr<QMenu> menu(column->createLaneMenu(0));
    ASSERT_NE(menu, nullptr);
    menu->actions().first()->trigger();
    auto* editor = panel.findChild<VNX::InlineRenameEdit*>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->text(), QStringLiteral("First"));
    QObject observer;
    int warnings = 0;
    QObject::connect(&manager, &VNX::ProjectManager::errorOccurred, &observer,
                     [&](const QString&) { ++warnings; });
    editor->setText(QStringLiteral("Bad/Name"));
    EXPECT_TRUE(editor->property("inputInvalid").toBool());
    EXPECT_FALSE(editor->finish(true));
    EXPECT_TRUE(editor->isVisible());
    EXPECT_EQ(track->name, QStringLiteral("First"));
    EXPECT_EQ(warnings, 0);
    editor->setText(QStringLiteral("Renamed"));
    EXPECT_FALSE(editor->property("inputInvalid").toBool());
    EXPECT_TRUE(editor->finish(true));
    EXPECT_EQ(panel.laneName(0), QStringLiteral("Renamed"));
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(panel.laneName(0), QStringLiteral("First"));
    EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
}

TEST_F(TimelineEditorPanelTest, SelectedLaneSlowClickRenamesAndFastDoubleClickDoesNot)
{
    ASSERT_NE(VNX::ProjectManager::instance().createItem(
        piece_, QStringLiteral("First"), VNX::ProjectItem::Type::Track), nullptr);
    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    panel.resize(800, 400);
    panel.show();
    QApplication::processEvents();
    auto* name = panel.findChild<QLabel*>(QStringLiteral("timelineLaneName"));
    ASSERT_NE(name, nullptr);
    const QPoint point(name->fontMetrics().horizontalAdvance(name->text()) / 2, name->rect().center().y());
    const auto mouse = [&](QEvent::Type type, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(point), QPointF(name->mapToGlobal(point)),
                          Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(name, &event);
    };
    const auto wait = [] {
        QEventLoop loop;
        QTimer::singleShot(QApplication::doubleClickInterval() + 100, &loop, &QEventLoop::quit);
        loop.exec();
    };
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_EQ(panel.findChild<VNX::InlineRenameEdit*>(), nullptr);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    QLineEdit otherWindow;
    otherWindow.show();
    QApplication::setActiveWindow(&otherWindow);
    otherWindow.setFocus();
    wait();
    EXPECT_EQ(panel.findChild<VNX::InlineRenameEdit*>(), nullptr);
    QApplication::setActiveWindow(&panel);
    name->setFocus(Qt::MouseFocusReason);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_EQ(panel.findChild<VNX::InlineRenameEdit*>(), nullptr);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    auto* editor = panel.findChild<VNX::InlineRenameEdit*>();
    ASSERT_NE(editor, nullptr);
    editor->finish(false);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    mouse(QEvent::MouseButtonPress, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    mouse(QEvent::MouseButtonDblClick, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::NoButton);
    wait();
    EXPECT_EQ(panel.findChild<VNX::InlineRenameEdit*>(), nullptr);
}

TEST_F(TimelineEditorPanelTest, RestoringQueueBindingClearsItWithoutChangingSpliceSetting)
{
    queue_->comp<VNX::VSelection>()->spliceByMarker = true;
    const QVariantMap state{
        {QStringLiteral("itemGuid"), queue_->guid.toString(QUuid::WithoutBraces)},
        {QStringLiteral("zoom"), 225},
    };

    VNX::TimelineEditorPanel panel;
    panel.restoreState(state);
    EXPECT_EQ(panel.boundItem(), nullptr);
    EXPECT_TRUE(panel.currentItemGuid().isNull());
    EXPECT_EQ(panel.viewKind(), VNX::TimelineEditorPanel::ViewKind::Empty);
    EXPECT_EQ(panel.zoomPercent(), 100);
    EXPECT_TRUE(queue_->comp<VNX::VSelection>()->spliceByMarker);
}

TEST_F(TimelineEditorPanelTest, ProjectReloadRestoresBindingByGuid)
{
    auto& manager = VNX::ProjectManager::instance();
    const QUuid guid = piece_->guid;
    VNX::TimelineEditorPanel panel;
    panel.restoreState({{QStringLiteral("itemGuid"), QUuid::createUuid().toString()}});
    ASSERT_EQ(panel.boundItem(), nullptr);
    panel.bindItem(piece_);
    ASSERT_EQ(panel.boundItem(), piece_);
    VNX::ProjectItem* oldPointer = piece_;
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    drainEvents();

    ASSERT_NE(panel.boundItem(), nullptr);
    EXPECT_NE(panel.boundItem(), oldPointer);
    EXPECT_EQ(panel.currentItemGuid(), guid);
    EXPECT_EQ(panel.boundItem()->guid, guid);
}

TEST_F(TimelineEditorPanelTest, DeletingBoundItemsAncestorClearsBinding)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* child = manager.createItem(
        piece_, QStringLiteral("Child"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(child, nullptr);
    VNX::TimelineEditorPanel panel;
    panel.bindItem(child);

    ASSERT_TRUE(manager.deleteItem(piece_));
    EXPECT_EQ(panel.viewKind(), VNX::TimelineEditorPanel::ViewKind::Empty);
    EXPECT_TRUE(panel.currentItemGuid().isNull());
    EXPECT_EQ(panel.boundItem(), nullptr);
}

TEST_P(TimelineTrackHostTest, TrackRowClipEditingSnapsAndRoundtripsUndoRedo)
{
    VNX::TimelineEditorPanel panel;
    panel.bindItem(track_);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    const int baseline = undo->count();

    ASSERT_TRUE(placeOnLane(panel, sound_, 0, 1600000000LL));
    ASSERT_EQ(panel.clipCount(0), 1);
    const QUuid clipGuid = panel.clipGuid(0, 0);
    EXPECT_EQ(panel.clipStart(0, 0), 2000000000LL);
    EXPECT_EQ(panel.clipDuration(0, 0), 1000000000LL);
    EXPECT_EQ(undo->count(), baseline + 1);

    ASSERT_TRUE(panel.moveClip(0, clipGuid, 0, 2250000000LL, true));
    EXPECT_EQ(panel.clipStart(0, 0), 2250000000LL);
    EXPECT_EQ(undo->count(), baseline + 2);
    ASSERT_TRUE(panel.resizeClip(
        0, clipGuid, 2250000000LL, 1350000000LL, true));
    EXPECT_EQ(panel.clipStart(0, 0), 2250000000LL);
    EXPECT_EQ(panel.clipDuration(0, 0), 1350000000LL);
    EXPECT_EQ(undo->count(), baseline + 3);
    undo->undo();
    EXPECT_EQ(panel.clipDuration(0, 0), 1000000000LL);
    undo->redo();
    EXPECT_EQ(panel.clipDuration(0, 0), 1350000000LL);
    ASSERT_TRUE(panel.deleteClip(0, clipGuid));
    EXPECT_EQ(panel.clipCount(0), 0);

    undo->undo();
    ASSERT_EQ(panel.clipCount(0), 1);
    EXPECT_EQ(panel.clipGuid(0, 0), clipGuid);
    undo->redo();
    EXPECT_EQ(panel.clipCount(0), 0);
}

TEST_F(TimelineEditorPanelTest, PieceCrossTrackMoveRejectsExternalClipWithoutChangingEitherTrack)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* first = manager.createItem(
        piece_, QStringLiteral("FirstTrack"), VNX::ProjectItem::Type::Track);
    VNX::ProjectItem* second = manager.createItem(
        piece_, QStringLiteral("SecondTrack"), VNX::ProjectItem::Type::Track);
    VNX::ProjectItem* owned = manager.createItem(
        first, QStringLiteral("OwnedSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(owned, nullptr);
    VNX::UndoRedoManager::instance().clear();

    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    const QUuid clipGuid = panel.clipGuid(0, 0);
    EXPECT_FALSE(panel.clipIsExternal(0, 0));
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    EXPECT_FALSE(panel.moveClip(0, clipGuid, 1, 3000000000LL));
    EXPECT_EQ(panel.clipCount(0), 1);
    EXPECT_EQ(panel.clipCount(1), 0);
    EXPECT_EQ(panel.clipGuid(0, 0), clipGuid);
    EXPECT_EQ(owned->parent, first);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_F(TimelineEditorPanelTest, PreviewAndCommitAgreeAcrossTrackHosts)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* nestedPiece = manager.createItem(container_, QStringLiteral("NestedPiece"), VNX::ProjectItem::Type::Piece);
    ASSERT_NE(nestedPiece, nullptr);
    for (VNX::ProjectItem* host : {nestedPiece, piece_}) {
        auto* track = manager.createItem(host,
            manager.suggestUniqueName(VNX::ProjectItem::Type::Track, QStringLiteral("OwnershipTrack")),
            VNX::ProjectItem::Type::Track);
        ASSERT_NE(track, nullptr);
        VNX::TimelineEditorPanel panel;
        panel.bindItem(track);
        for (const auto type : {VNX::ProjectItem::Type::Sound, VNX::ProjectItem::Type::Container,
                                VNX::ProjectItem::Type::Piece}) {
            auto* target = manager.createItem(
                track, manager.suggestUniqueName(type, QStringLiteral("DirectTarget")), type);
            ASSERT_NE(target, nullptr);
            QString error;
            EXPECT_TRUE(panel.validateClipTarget(target, 0, &error)) << error.toStdString();
            EXPECT_TRUE(panel.placeItem(target, 0, 0)) << panel.lastInteractionError().toStdString();
        }
        auto* nested = manager.createItem(track,
            manager.suggestUniqueName(VNX::ProjectItem::Type::Container, QStringLiteral("Nested")),
            VNX::ProjectItem::Type::Container);
        ASSERT_NE(nested, nullptr);
        auto* grandchild = manager.createItem(nested,
            manager.suggestUniqueName(VNX::ProjectItem::Type::Sound, QStringLiteral("Grandchild")),
            VNX::ProjectItem::Type::Sound);
        ASSERT_NE(grandchild, nullptr);
        const auto clips = track->comp<VNX::VTrack>()->clips;
        const bool dirty = track->dirtyBit;
        auto* undo = VNX::UndoRedoManager::instance().undoStack();
        const int count = undo->count();
        for (auto* target : {sound_, host, grandchild}) {
            QString error;
            EXPECT_FALSE(panel.validateClipTarget(target, 0, &error));
            EXPECT_TRUE(error.contains(QStringLiteral("direct child")));
            EXPECT_FALSE(panel.placeItem(target, 0, 0));
            EXPECT_EQ(track->comp<VNX::VTrack>()->clips, clips);
            EXPECT_EQ(track->dirtyBit, dirty);
            EXPECT_EQ(undo->count(), count);
        }
    }
}

TEST_F(TimelineEditorPanelTest, DragFeedbackAndStaleCandidatesRespectOwnership)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* external = manager.createItem(container_, QStringLiteral("External"),
                                        VNX::ProjectItem::Type::Sound);
    ASSERT_NE(external, nullptr);
    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    for (auto* target : {sound_, external}) {
        QMimeData mime;
        VNX::ProjectItemMime::writeObjectIDs(&mime, {target->objectID});
        QDragEnterEvent enter(QPoint(40, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &enter);
        EXPECT_EQ(enter.isAccepted(), target == sound_);
    }
    EXPECT_TRUE(panel.validateClipTarget(sound_, 0));
    ASSERT_TRUE(manager.moveItem(sound_, container_));
    const auto clips = track_->comp<VNX::VTrack>()->clips;
    const bool dirty = track_->dirtyBit;
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    const int count = undo->count();
    EXPECT_FALSE(panel.placeItem(sound_, 0, 0));
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, clips);
    EXPECT_EQ(track_->dirtyBit, dirty);
    EXPECT_EQ(undo->count(), count);
    EXPECT_EQ(sound_->parent, container_);
}

TEST_P(TimelineTrackHostTest, ClipDrillPathOpensAndReturns)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* hostTrack = track_;
    VNX::ProjectItem* nested = manager.createItem(
        hostTrack, QStringLiteral("NestedPiece"), VNX::ProjectItem::Type::Piece);
    ASSERT_NE(hostTrack, nullptr);
    ASSERT_NE(nested, nullptr);
    hostTrack->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), nested->guid, 0, 1000000000LL},
    });

    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(hostTrack);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(
        QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    panel.zoomAroundPixel(0, 4.0);
    panel.zoomVertically(panel.trackWaveformRect().center().y(), 2.0);
    panel.panHorizontally(12);
    panel.setEditCursor(123456789LL, true);
    canvas->setSelectedClip(0, panel.clipGuid(0, 0));
    const QVariantMap parentState = panel.snapshotState();
    const int clickX = panel.pixelAtCoordinate(0, 500000000LL) - panel.horizontalScroll();
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(clickX, 20),
                            QPointF(canvas->mapToGlobal(QPoint(clickX, 20))),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &doubleClick);
    drainEvents();
    EXPECT_EQ(panel.currentItemGuid(), nested->guid);
    EXPECT_EQ(panel.drillDepth(), 1);
    EXPECT_TRUE(panel.canDrillBack());

    const QVariantMap state = panel.snapshotState();
    VNX::TimelineEditorPanel restored;
    restored.resize(panel.size());
    restored.restoreState(state);
    restored.show();
    drainEvents();
    EXPECT_EQ(restored.currentItemGuid(), nested->guid);
    EXPECT_EQ(restored.drillDepth(), 1);
    auto* breadcrumb = restored.findChild<QToolButton*>(QStringLiteral("timelineBreadcrumb"));
    ASSERT_NE(breadcrumb, nullptr);
    EXPECT_TRUE(breadcrumb->isVisible());
    breadcrumb->click();
    drainEvents();
    EXPECT_EQ(restored.drillDepth(), 0);
    EXPECT_EQ(restored.currentItemGuid(), QUuid(parentState.value("itemGuid").toString()));
    EXPECT_EQ(restored.snapshotState().value("timelineViewport"), parentState.value("timelineViewport"));
    EXPECT_EQ(restored.selectedClipGuids(),
        QSet<QUuid>{hostTrack->comp<VNX::VTrack>()->clips.items.front().clipGuid});

    ASSERT_TRUE(panel.drillBack());
    EXPECT_EQ(panel.currentItemGuid(), QUuid(parentState.value("itemGuid").toString()));
    EXPECT_EQ(panel.drillDepth(), 0);
    EXPECT_EQ(panel.snapshotState().value("timelineViewport"), parentState.value("timelineViewport"));
    EXPECT_EQ(panel.editCursor(), 123456789LL);
    EXPECT_EQ(panel.selectedClipGuids(), restored.selectedClipGuids());
}

TEST_F(TimelineEditorPanelTest, BlueprintRequestCarriesSelectedClipAndDrillOccurrences)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* outerTrack = manager.createItem(
        piece_, QStringLiteral("OuterTrack"), VNX::ProjectItem::Type::Track);
    VNX::ProjectItem* nested = manager.createItem(
        nullptr, QStringLiteral("NestedPiece"), VNX::ProjectItem::Type::Piece);
    VNX::ProjectItem* innerTrack = manager.createItem(
        nested, QStringLiteral("InnerTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(outerTrack, nullptr);
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(innerTrack, nullptr);

    const QUuid outerClipGuid = QUuid::createUuid();
    const QUuid innerClipGuid = QUuid::createUuid();
    outerTrack->comp<VNX::VTrack>()->clips = makeClipLedger({
        {outerClipGuid, nested->guid, 0, 1000000000LL},
    });
    innerTrack->comp<VNX::VTrack>()->clips = makeClipLedger({
        {innerClipGuid, ownedGuid(innerTrack), 0, 1000000000LL},
    });

    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    ASSERT_TRUE(panel.drillIntoClip(0, outerClipGuid));
    ASSERT_EQ(panel.currentItemGuid(), nested->guid);

    VNX::ProjectItem* requestedSubject = nullptr;
    VNX::PlacementContext requestedContext;
    int requestCount = 0;
    QObject::connect(
        &panel, &VNX::TimelineEditorPanel::blueprintRequested, &panel,
        [&](VNX::ProjectItem* subject, const VNX::PlacementContext& context) {
            requestedSubject = subject;
            requestedContext = context;
            ++requestCount;
        });

    VNX::ProjectItem* selectedLane = nullptr;
    VNX::PlacementContext selectedContext;
    QObject::connect(
        &panel, &VNX::TimelineEditorPanel::laneSelectionRequested, &panel,
        [&](VNX::ProjectItem* track, const VNX::PlacementContext& context) {
            selectedLane = track;
            selectedContext = context;
        });
    auto* laneName = panel.findChild<QLabel*>(
        QStringLiteral("timelineLaneName"));
    ASSERT_NE(laneName, nullptr);
    const QPoint point = laneName->rect().center();
    QMouseEvent lanePress(QEvent::MouseButtonPress, point, laneName->mapToGlobal(point),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(laneName, &lanePress);
    QMouseEvent laneRelease(QEvent::MouseButtonRelease, point, laneName->mapToGlobal(point),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(laneName, &laneRelease);
    EXPECT_EQ(selectedLane, innerTrack);
    ASSERT_EQ(selectedContext.occurrences().size(), 1u);
    EXPECT_EQ(selectedContext.occurrences().front(),
              (VNX::PlacementOccurrence{outerTrack->guid, outerClipGuid, nested->guid}));

    auto* headers = panel.findChild<VNX::TimelineHeaderColumn*>(QStringLiteral("timelineHeaderColumn"));
    ASSERT_NE(headers, nullptr);
    std::unique_ptr<QMenu> laneMenu(headers->createLaneMenu(0));
    QAction* laneBlueprint = laneMenu->findChild<QAction*>(QStringLiteral("timelineLaneMenuBlueprint"));
    ASSERT_NE(laneBlueprint, nullptr);
    laneBlueprint->trigger();
    EXPECT_EQ(requestCount, 1);
    EXPECT_EQ(requestedSubject, innerTrack);
    ASSERT_EQ(requestedContext.occurrences().size(), 1u);
    EXPECT_EQ(requestedContext.occurrences().front(),
              (VNX::PlacementOccurrence{outerTrack->guid, outerClipGuid, nested->guid}));

    ASSERT_TRUE(panel.requestBlueprintForClip(0, innerClipGuid));
    EXPECT_EQ(requestCount, 2);
    EXPECT_EQ(requestedSubject, innerTrack->children.front().get());
    ASSERT_EQ(requestedContext.occurrences().size(), 2u);
    EXPECT_EQ(requestedContext.occurrences()[0],
              (VNX::PlacementOccurrence{innerTrack->guid, innerClipGuid, ownedGuid(innerTrack)}));
    EXPECT_EQ(requestedContext.occurrences()[1],
              (VNX::PlacementOccurrence{outerTrack->guid, outerClipGuid, nested->guid}));

    const VNX::PlacementContext laneContext = panel.placementContextForLane(0);
    ASSERT_EQ(laneContext.occurrences().size(), 1u);
    EXPECT_EQ(laneContext.occurrences().front(), requestedContext.occurrences()[1]);

    const QVariantMap state = panel.snapshotState();
    VNX::TimelineEditorPanel restored;
    restored.restoreState(state);
    const VNX::PlacementContext restoredContext =
        restored.placementContextForClip(0, innerClipGuid);
    EXPECT_EQ(restoredContext, requestedContext);
}

TEST_F(TimelineEditorPanelTest, LegacyMusicalTimelineIsReadOnlyAndPreservesCoordinates)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* track = manager.createItem(
        piece_, QStringLiteral("MusicTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* musicSound = manager.createItem(
        track, QStringLiteral("MusicSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(musicSound, nullptr);
    piece_->comp<VNX::VPiece>()->pieceTime = VNX::PieceTimeSpec::musical(500000, 6, 8);
    track->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), musicSound->guid, 1440, 480}
    });
    const auto before = track->comp<VNX::VTrack>()->clips.toJsonObject();
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    const int count = undo->count();
    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    EXPECT_EQ(panel.viewKind(), VNX::TimelineEditorPanel::ViewKind::Unsupported);
    EXPECT_EQ(panel.laneCount(), 0);
    EXPECT_FALSE(panel.placeItem(sound_, 0, 1450));
    EXPECT_FALSE(panel.addMarker(VNX::MarkerKind::Custom, 1440, QStringLiteral("Hit")));
    EXPECT_FALSE(panel.setTimelineDomain(VNX::TimelineDomain::Absolute));
    EXPECT_EQ(track->comp<VNX::VTrack>()->clips.toJsonObject(), before);
    EXPECT_EQ(undo->count(), count);
    EXPECT_EQ(panel.formatCoordinate(0, 3360), QStringLiteral("Unsupported arrangement"));
    EXPECT_EQ(panel.pixelAtCoordinate(0, 3360), 0);
    EXPECT_EQ(panel.coordinateAtPixel(0, 300), 0);
}

TEST_P(TimelineTrackHostTest, CanvasGestureCommitsOnceOnRelease)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(placeOnLane(panel, sound_, 0, 1000000000LL));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(
        QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    VNX::UndoRedoManager::instance().clear();

    const QPointF pressPos(panel.pixelAtCoordinate(0, 1500000000LL)
                               - panel.horizontalScroll(), 24.0);
    const QPointF movePos(panel.pixelAtCoordinate(0, 2500000000LL)
                              - panel.horizontalScroll(), 24.0);
    QMouseEvent press(QEvent::MouseButtonPress, pressPos,
                      canvas->mapToGlobal(pressPos), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent move(QEvent::MouseMove, movePos,
                     canvas->mapToGlobal(movePos), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, movePos,
                        canvas->mapToGlobal(movePos), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    EXPECT_EQ(panel.clipStart(0, 0), 2000000000LL);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(TimelineEditorPanelTest, PieceRejectsMusicalAuthoringWithoutChangingData)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* track = manager.createItem(
        piece_, QStringLiteral("AbsoluteTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    track->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), ownedGuid(track), 1000000000LL, 1000000000LL}
    });
    piece_->comp<VNX::VPiece>()->markers.items = {
        {QUuid::createUuid(), VNX::MarkerKind::Entry, {}, 0},
        {QUuid::createUuid(), VNX::MarkerKind::Exit, {}, 2000000000LL}
    };
    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    VNX::UndoRedoManager::instance().clear();
    const bool dirty = piece_->isDirtyObject();
    EXPECT_FALSE(panel.setTimelineDomain(VNX::TimelineDomain::Musical));
    EXPECT_EQ(panel.lastInteractionError(), VNX::AuthoringPolicy::musicalTimeRestriction());
    EXPECT_FALSE(panel.setTempoMicrosecondsPerQuarter(600000));
    EXPECT_EQ(panel.lastInteractionError(), VNX::AuthoringPolicy::musicalTimeRestriction());
    EXPECT_FALSE(panel.setMeter(6, 8));
    EXPECT_EQ(panel.lastInteractionError(), VNX::AuthoringPolicy::musicalTimeRestriction());
    EXPECT_EQ(panel.timelineDomain(), VNX::TimelineDomain::Absolute);
    EXPECT_EQ(panel.clipStart(0, 0), 1000000000LL);
    EXPECT_EQ(panel.clipDuration(0, 0), 1000000000LL);
    EXPECT_EQ(panel.markerPosition(1), 2000000000LL);
    EXPECT_EQ(piece_->isDirtyObject(), dirty);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(TimelineEditorPanelTest, MarkerCrudEnforcesKindsAndEveryEditRoundtrips)
{
    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    VNX::UndoRedoManager::instance().clear();

    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Entry, -1000000000LL));
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Exit, 2000000000LL));
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Custom, 0, QStringLiteral("Hit")));
    ASSERT_EQ(panel.markerCount(), 3);
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    EXPECT_FALSE(panel.addMarker(VNX::MarkerKind::Entry, 0));
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);

    const QUuid entryGuid = panel.markerGuid(0);
    const QUuid customGuid = panel.markerGuid(2);
    EXPECT_FALSE(panel.moveMarker(entryGuid, 3000000000LL, true));
    EXPECT_EQ(panel.markerPosition(0), -1000000000LL);
    ASSERT_TRUE(panel.renameMarker(customGuid, QStringLiteral("Impact")));
    EXPECT_EQ(panel.markerName(2), QStringLiteral("Impact"));
    ASSERT_TRUE(panel.moveMarker(customGuid, 1000000000LL, true));
    EXPECT_EQ(panel.markerPosition(2), 1000000000LL);
    ASSERT_TRUE(panel.deleteMarker(customGuid));
    EXPECT_EQ(panel.markerCount(), 2);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(panel.markerCount(), 3);
    EXPECT_EQ(panel.markerName(2), QStringLiteral("Impact"));
    VNX::UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(panel.markerCount(), 2);

}

TEST_F(TimelineEditorPanelTest, MarkerRenameKeepsMenuEditsUntilCommitOrCancel)
{
    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Custom, 0, QStringLiteral("Hit")));
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    VNX::TimelineMarkerRuler ruler(&panel);
    ruler.setMinimumSize(800, 30);
    QLineEdit nextField;
    layout->addWidget(&ruler);
    layout->addWidget(&nextField);
    QMenu menu(&window);
    auto& shortcuts = VNX::ShortcutManager::instance();
    QAction* copy = shortcuts.createMenuAction(VNX::Shortcuts::Copy, &menu);
    QAction* cut = shortcuts.createMenuAction(VNX::Shortcuts::Cut, &menu);
    QAction* paste = shortcuts.createMenuAction(VNX::Shortcuts::Paste, &menu);
    const QString previousClipboard = QApplication::clipboard()->text();
    auto restoreClipboard = qScopeGuard([&]() {
        QApplication::clipboard()->setText(previousClipboard);
    });
    window.show();
    window.activateWindow();
    drainEvents();
    auto* history = VNX::UndoRedoManager::instance().undoStack();

    for (Qt::Key completion : {Qt::Key_Return, Qt::Key_Escape, Qt::Key_Tab}) {
        SCOPED_TRACE(static_cast<int>(completion));
        history->clear();
        const QString original = panel.markerName(0);
        const QString renamed = original + QStringLiteral(" edited");
        const QPointF point(panel.pixelAtCoordinate(0, panel.markerDisplayPosition(0))
                                - panel.horizontalScroll(), ruler.height() / 2.0);
        QMouseEvent doubleClick(QEvent::MouseButtonDblClick, point, ruler.mapToGlobal(point),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&ruler, &doubleClick);
        auto* editor = ruler.findChild<QLineEdit*>();
        ASSERT_NE(editor, nullptr);
        ASSERT_TRUE(editor->isVisible());
        ASSERT_EQ(QApplication::focusWidget(), editor);
        editor->setText(renamed);
        editor->selectAll();

        for (QAction* action : {copy, cut, paste}) {
            menu.popup(window.mapToGlobal(QPoint(0, window.height())));
            drainEvents();
            ASSERT_TRUE(menu.isVisible());
            ASSERT_TRUE(editor->isVisible());
            ASSERT_TRUE(action->isEnabled());
            menu.setActiveAction(action);
            QKeyEvent choose(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(&menu, &choose);
            drainEvents();
            EXPECT_FALSE(menu.isVisible());
            EXPECT_TRUE(editor->isVisible());
            EXPECT_EQ(editor->text(), action == cut ? QString() : renamed);
            EXPECT_EQ(QApplication::clipboard()->text(), renamed);
            EXPECT_EQ(panel.markerName(0), original);
            EXPECT_EQ(history->count(), 0);
        }

        if (completion == Qt::Key_Tab) {
            nextField.setFocus(Qt::TabFocusReason);
        } else {
            QKeyEvent finish(QEvent::KeyPress, completion, Qt::NoModifier);
            QApplication::sendEvent(editor, &finish);
        }
        EXPECT_FALSE(editor->isVisible());
        if (completion == Qt::Key_Escape) {
            EXPECT_EQ(panel.markerName(0), original);
            EXPECT_EQ(history->count(), 0);
        } else {
            EXPECT_EQ(panel.markerName(0), renamed);
            ASSERT_EQ(history->count(), 1);
            history->undo();
            EXPECT_EQ(panel.markerName(0), original);
            history->redo();
            EXPECT_EQ(panel.markerName(0), renamed);
        }
    }
}

TEST_F(TimelineEditorPanelTest, RulerGestureMovesCursorWithoutEditingHiddenMarker)
{
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.resize(1200, 420);
    panel.bindItem(piece_);
    panel.show();
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Custom, 0, QStringLiteral("Hit")));
    drainEvents();
    auto* ruler = panel.findChild<VNX::TimelineRuler*>(QStringLiteral("timelineRuler"));
    ASSERT_NE(ruler, nullptr);
    VNX::UndoRedoManager::instance().clear();

    const QPointF pressPos(panel.laneHeaderWidth() + panel.pixelAtCoordinate(0, 0)
                               - panel.horizontalScroll(), ruler->cursorHandleRect().center().y());
    const QPointF movePos(panel.laneHeaderWidth() + panel.pixelAtCoordinate(0, 1000000000LL)
                              - panel.horizontalScroll(), ruler->cursorHandleRect().center().y());
    QMouseEvent press(QEvent::MouseButtonPress, pressPos, ruler->mapToGlobal(pressPos),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ruler, &press);
    QMouseEvent move(QEvent::MouseMove, movePos, ruler->mapToGlobal(movePos),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ruler, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, movePos, ruler->mapToGlobal(movePos),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(ruler, &release);
    EXPECT_EQ(panel.editCursor(), 1000000000LL);
    EXPECT_EQ(panel.markerPosition(0), 0);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(TimelineEditorPanelTest, NominalSpanAndOverlapFollowPlacementEdits)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* track = manager.createItem(
        piece_, QStringLiteral("OverlapTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    VNX::ProjectItem* owned = manager.createItem(
        track, QStringLiteral("OverlapSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(owned, nullptr);
    const QUuid firstGuid = QUuid::createUuid();
    const QUuid secondGuid = QUuid::createUuid();
    track->comp<VNX::VTrack>()->clips = makeClipLedger({
        {firstGuid, owned->guid, 0, 10},
        {secondGuid, owned->guid, 8, 12}
    });
    piece_->comp<VNX::VPiece>()->markers.items = {
        {QUuid::createUuid(), VNX::MarkerKind::Entry, {}, 2},
        {QUuid::createUuid(), VNX::MarkerKind::Exit, {}, 18}
    };

    VNX::TimelineEditorPanel panel;
    panel.bindItem(piece_);
    const VNX::TimelineSemanticsResult semantics = panel.timelineSemantics();
    ASSERT_TRUE(semantics);
    EXPECT_EQ(semantics.nominal, (VNX::TimelineSpan{2, 18}));
    EXPECT_EQ(semantics.pickup, 2);
    EXPECT_EQ(semantics.tail, 2);
    EXPECT_EQ(panel.overlapRegions(0), (std::vector<VNX::TimelineSpan>{{8, 10}}));

    ASSERT_TRUE(panel.moveClip(0, secondGuid, 0, 10, true));
    EXPECT_TRUE(panel.overlapRegions(0).empty());
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(panel.overlapRegions(0), (std::vector<VNX::TimelineSpan>{{8, 10}}));
    EXPECT_TRUE(panel.resizeClip(0, firstGuid, 0, 11, true));
    EXPECT_EQ(panel.overlapRegions(0), (std::vector<VNX::TimelineSpan>{{8, 11}}));

    track->comp<VNX::VTrack>()->clips.items.front().targetGuid = QUuid::createUuid();
    emit manager.itemPropertyChanged(track, QStringLiteral("clips"));
    drainEvents();
    EXPECT_FALSE(panel.timelineSemantics());
}

TEST_P(TimelineTrackHostTest, TrackOverviewResizesAndFollowsPlacedClips)
{
    VNX::TimelineEditorPanel panel;
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 12000000000LL, true));
    const auto clips = track_->comp<VNX::VTrack>()->clips.toJsonObject();
    for (const QSize size : {QSize(320, 180), QSize(800, 400), QSize(1600, 720)}) {
        panel.resize(size);
        drainEvents();
        auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
        ASSERT_NE(canvas, nullptr);
        EXPECT_EQ(panel.coordinateAtPixel(0, canvas->width() - 1), 13000000000LL);
        EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.toJsonObject(), clips);
    }
    ASSERT_TRUE(panel.deleteClip(0, panel.clipGuid(0, 0)));
    drainEvents();
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.timelineViewportWidth() - 1), 10000000000LL);
    ASSERT_TRUE(panel.placeItem(sound_, 0, 25000000000LL, true));
    drainEvents();
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.timelineViewportWidth() - 1), 26000000000LL);
    ASSERT_TRUE(panel.deleteClip(0, panel.clipGuid(0, 0)));
    drainEvents();
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.timelineViewportWidth() - 1), 10000000000LL);
    EXPECT_EQ(panel.zoomPercent(), 100);
    ASSERT_TRUE(panel.placeItem(sound_, 0, 1000000000LL, true));
    drainEvents();
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.timelineViewportWidth() - 1), 10000000000LL);
}

TEST_P(TimelineTrackHostTest, TrackAxisWheelZoomPreservesTimeAnchor)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 400);
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    auto* ruler = panel.findChild<VNX::TimelineRuler*>("timelineRuler");
    const QPoint rulerPoint(panel.laneHeaderWidth() + canvas->width() / 2, 10);
    const int64_t time = panel.coordinateAtPixel(0, canvas->width() / 2);
    QWheelEvent timeWheel(rulerPoint, ruler->mapToGlobal(rulerPoint), QPoint(40, 0), QPoint(),
        Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(ruler, &timeWheel);
    EXPECT_EQ(panel.zoomPercent(), 125);
    EXPECT_NEAR(panel.coordinateAtPixel(0, canvas->width() / 2 + panel.horizontalScroll()),
        time, 10000000LL);

}

TEST_F(TimelineEditorPanelTest, TrackSnapUsesVisibleMinorTicksAndNearestClipBoundary)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 400);
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    EXPECT_DOUBLE_EQ(panel.timeTicks().minorStep, 0.2);
    EXPECT_EQ(panel.snapCoordinate(0, 1370000000LL, false), 1400000000LL);
    panel.setZoomPercent(800);
    drainEvents();
    EXPECT_DOUBLE_EQ(panel.timeTicks().minorStep, 0.02);
    EXPECT_EQ(panel.snapCoordinate(0, 1370000000LL, false), 1380000000LL);
    EXPECT_EQ(panel.snapCoordinate(0, -1370000000LL, false), -1380000000LL);
    ASSERT_TRUE(panel.placeItem(sound_, 0, 1373000000LL, true));
    const QUuid clip = panel.clipGuid(0, 0);
    EXPECT_EQ(panel.snapCoordinate(0, 1375000000LL, false), 1373000000LL);
    EXPECT_EQ(panel.snapCoordinate(0, 1361000000LL, false), 1360000000LL);
    EXPECT_EQ(panel.snapCoordinate(0, 2375000000LL, false), 2373000000LL);
    EXPECT_EQ(panel.snapCoordinate(0, 1375000000LL, true), 1375000000LL);
    EXPECT_EQ(panel.snapCoordinate(0, 1375000000LL, false, nullptr, {clip}), 1380000000LL);
    ASSERT_TRUE(panel.moveClip(0, clip, 0, 1374000000LL));
    EXPECT_EQ(panel.clipStart(0, 0), 1380000000LL);
    ASSERT_TRUE(panel.deleteClip(0, clip));
    auto& manager = VNX::ProjectManager::instance();
    auto* other = manager.createItem(track_->parent, QStringLiteral("OtherTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(other, nullptr);
    ASSERT_TRUE(placeOnLane(panel, sound_, 1, 1373000000LL, true));
    drainEvents();
    EXPECT_EQ(panel.snapCoordinate(0, 1375000000LL, false), 1373000000LL);
    EXPECT_EQ(panel.snapCoordinate(0, 1375000000LL, false, nullptr, {panel.clipGuid(1, 0)}), 1380000000LL);
    panel.setZoomPercent(100);
    drainEvents();
    EXPECT_DOUBLE_EQ(panel.timeTicks().minorStep, 0.2);
    const auto piece = VNX::buildTimelineTicks({}, 0, 0, 800, 640, panel.timelineMetrics());
    EXPECT_GE(piece.majorStep, 1000000000LL);
}

TEST_P(TimelineTrackHostTest, TrackClipStartPopupCommitsOnceAndDiscardsCancelledOrStaleText)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 0, true));
    ASSERT_TRUE(panel.placeItem(sound_, 0, 3000000000LL, true));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    const auto original = track_->comp<VNX::VTrack>()->clips.items.front();
    const auto second = panel.clipGuid(0, 1);
    const auto openEditor = [&]() {
        std::unique_ptr<QMenu> menu(canvas->createClipMenu(0, original.clipGuid));
        auto* action = menu->findChild<QAction*>("timelineEditClipStart");
        if (!action) return static_cast<QDialog*>(nullptr);
        action->trigger();
        drainEvents();
        for (auto* dialog : canvas->findChildren<QDialog*>("timelineClipStartEditor"))
            if (dialog->isVisible()) return dialog;
        return static_cast<QDialog*>(nullptr);
    };
    auto* dialog = openEditor();
    ASSERT_NE(dialog, nullptr);
    auto* input = dialog->findChild<QDoubleSpinBox*>("timelineClipStartInput");
    ASSERT_NE(input, nullptr);
    input->setValue(1.375123456);
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    EXPECT_EQ(panel.clipStart(0, panel.findClipIndex(0, original.clipGuid)), 0);
    dialog->accept();
    drainEvents();
    EXPECT_EQ(panel.clipStart(0, panel.findClipIndex(0, original.clipGuid)), 1375123456LL);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount + 1);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    drainEvents();
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.items.front(), original);

    dialog = openEditor();
    ASSERT_NE(dialog, nullptr);
    input = dialog->findChild<QDoubleSpinBox*>("timelineClipStartInput");
    const int zoom = panel.zoomPercent();
    QWheelEvent numericWheel(input->rect().center(), input->mapToGlobal(input->rect().center()), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(input, &numericWheel);
    EXPECT_EQ(panel.zoomPercent(), zoom);
    input->setValue(8.0);
    dialog->reject();
    drainEvents();
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.items.front(), original);

    dialog = openEditor();
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<QDoubleSpinBox*>("timelineClipStartInput")->setValue(9.0);
    canvas->setSelectedClip(0, second);
    EXPECT_FALSE(dialog->isVisible());
    drainEvents();
    EXPECT_EQ(panel.clipStart(0, panel.findClipIndex(0, original.clipGuid)), 0);
    EXPECT_EQ(panel.clipStart(0, panel.findClipIndex(0, second)), 3000000000LL);

    dialog = openEditor();
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<QDoubleSpinBox*>("timelineClipStartInput")->setValue(10.0);
    panel.bindItem(piece_);
    EXPECT_FALSE(dialog->isVisible());
    drainEvents();
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.items.front(), original);
}

TEST_F(TimelineEditorPanelTest, PieceViewportRestoresTimeAndTrackScrollWithoutAuthoringChanges)
{
    auto& manager = VNX::ProjectManager::instance();
    for (int row = 0; row < 12; ++row)
        ASSERT_NE(manager.createItem(piece_, QStringLiteral("Track"), VNX::ProjectItem::Type::Track), nullptr);
    VNX::TimelineEditorPanel panel;
    panel.setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    panel.resize(900, 300);
    panel.bindItem(piece_);
    panel.show();
    drainEvents();
    panel.selectLane(panel.laneGuid(7));
    auto* scroll = panel.findChild<QScrollArea*>("timelineViewport");
    ASSERT_NE(scroll, nullptr);
    scroll->verticalScrollBar()->setValue(180);
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>();
    auto* table = panel.findChild<VNX::TableWidget*>();
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(placeOnLane(panel, sound_, 0, 123456789LL, true));
    drainEvents();
    const auto authored = manager.findItemByGuid(panel.laneGuid(0))->comp<VNX::VTrack>()->clips;
    const int resizeUndoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    const auto selection = panel.selectedClipGuids();
    const auto cursor = panel.editCursor();
    const int defaultHeight = panel.laneRect(0).height();
    const auto checkRows = [&] {
        drainEvents();
        canvas->repaint();
        const auto numbers = table->findChildren<QLabel*>("tableRowNumber");
        ASSERT_EQ(numbers.size(), 12);
        ASSERT_EQ(canvas->lastRenderData().laneRects.size(), 12);
        int nextTop = 0;
        for (int row = 0; row < numbers.size(); ++row) {
            const auto* header = numbers[row]->parentWidget();
            const auto lane = canvas->lastRenderData().laneRects[row].rect;
            EXPECT_EQ(lane.top(), nextTop);
            EXPECT_EQ(header->mapToGlobal(QPoint()).y(), canvas->mapToGlobal(lane.topLeft()).y());
            EXPECT_EQ(header->height(), lane.height());
            nextTop = lane.bottom() + 1;
        }
        for (const auto& clip : canvas->lastRenderData().clipItems)
            EXPECT_TRUE(panel.laneRect(clip.laneIndex).contains(clip.rect));
    };
    const auto boundaryColor = [](QWidget* boundary) {
        const qreal scale = boundary->devicePixelRatioF();
        QImage image(boundary->size() * scale, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(scale);
        image.fill(Qt::transparent);
        boundary->render(&image);
        return image.pixelColor(image.width() / 2, image.height() - 1);
    };
    const auto resizeRow = [&](int row, int x, int delta, bool cancel = false) {
        const QPoint origin = canvas->mapToGlobal(QPoint(x, panel.laneRect(row).bottom()));
        auto* boundary = panel.childAt(panel.mapFromGlobal(origin));
        ASSERT_NE(boundary, nullptr);
        ASSERT_EQ(boundary->cursor().shape(), Qt::SplitVCursor);
        EXPECT_EQ(panel.childAt(canvas->mapTo(&panel,
            QPoint(canvas->width() / 2, panel.laneRect(row).bottom()))), canvas);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(boundary, &leave);
        const QColor normalBorder = boundaryColor(boundary);
        QEnterEvent enter(boundary->mapFromGlobal(origin), panel.mapFromGlobal(origin), origin);
        QApplication::sendEvent(boundary, &enter);
        const QColor activeBorder = boundaryColor(boundary);
        EXPECT_NE(activeBorder, normalBorder);
        EXPECT_EQ(activeBorder, VNX::ThemeManager::instance().colors().accent);
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseMove,
                          QEvent::MouseMove, QEvent::MouseButtonRelease}) {
            const QPoint global = origin + QPoint(0, type == QEvent::MouseButtonPress ? 0 : delta);
            QMouseEvent event(type, boundary->mapFromGlobal(global), global,
                type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(boundary, &event);
            drainEvents();
            if (type != QEvent::MouseButtonRelease) {
                QApplication::sendEvent(boundary, &leave);
                EXPECT_EQ(boundaryColor(boundary), activeBorder);
            }
            if (cancel && type == QEvent::MouseMove) {
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(boundary, &escape);
                break;
            }
        }
        QApplication::sendEvent(boundary, &leave);
        EXPECT_EQ(boundaryColor(boundary), normalBorder);
        checkRows();
        EXPECT_EQ(manager.findItemByGuid(panel.laneGuid(0))->comp<VNX::VTrack>()->clips, authored);
        EXPECT_EQ(panel.selectedClipGuids(), selection);
        EXPECT_EQ(panel.editCursor(), cursor);
        EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), resizeUndoCount);
    };
    checkRows();
    scroll->verticalScrollBar()->setValue(0);
    resizeRow(0, -panel.laneHeaderWidth() / 2, 40);
    EXPECT_EQ(panel.laneRect(0).height(), defaultHeight + 40);
    resizeRow(0, -panel.laneHeaderWidth() + 10, 20);
    EXPECT_EQ(panel.laneRect(0).height(), defaultHeight + 60);
    resizeRow(0, -panel.laneHeaderWidth() / 2, -1000);
    EXPECT_LT(panel.laneRect(0).height(), defaultHeight);
    EXPECT_GT(panel.laneRect(0).height(), panel.timelineMetrics().clipVerticalInset * 2);
    const QPoint resetPosition = canvas->mapToGlobal(
        QPoint(-panel.laneHeaderWidth() + 30, panel.laneRect(0).bottom()));
    auto* boundary = panel.childAt(panel.mapFromGlobal(resetPosition));
    ASSERT_NE(boundary, nullptr);
    QMouseEvent reset(QEvent::MouseButtonDblClick, boundary->mapFromGlobal(resetPosition),
        resetPosition, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(boundary, &reset);
    checkRows();
    EXPECT_EQ(panel.laneRect(0).height(), defaultHeight);
    resizeRow(0, -panel.laneHeaderWidth() / 2, 55, true);
    EXPECT_EQ(panel.laneRect(0).height(), defaultHeight);
    resizeRow(0, -panel.laneHeaderWidth() / 2, 35);
    scroll->verticalScrollBar()->setValue(180);
    resizeRow(5, -panel.laneHeaderWidth() / 2, 25);
    EXPECT_EQ(panel.laneRect(5).height(), defaultHeight + 25);
    panel.selectLane(panel.laneGuid(7));
    scroll->verticalScrollBar()->setValue(180);
    checkRows();
    const auto numbers = table->findChildren<QLabel*>("tableRowNumber");
    panel.zoomAroundPixel(panel.timelineViewportWidth() / 2, 8.0);
    panel.panHorizontally(35);
    const auto viewport = panel.snapshotState().value("timelineViewport");
    auto* header = numbers[5];
    const QPoint point = header->rect().center();
    const int scrollBefore = scroll->verticalScrollBar()->value();
    QWheelEvent wheel(point, header->mapToGlobal(point), QPoint(), QPoint(0, -120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(header, &wheel);
    EXPECT_GT(scroll->verticalScrollBar()->value(), scrollBefore);
    EXPECT_EQ(panel.snapshotState().value("timelineViewport"), viewport);
    panel.setEditCursor(3456789123LL, true);
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    const auto state = panel.snapshotState();
    VNX::TimelineEditorPanel restored;
    restored.setStyleSheet(panel.styleSheet());
    restored.resize(panel.size());
    restored.show();
    restored.restoreState(state);
    drainEvents();
    EXPECT_EQ(restored.boundItem(), piece_);
    EXPECT_EQ(restored.editCursor(), panel.editCursor());
    EXPECT_EQ(restored.zoomPercent(), panel.zoomPercent());
    EXPECT_EQ(restored.snapshotState().value("timelineViewport"), state.value("timelineViewport"));
    EXPECT_EQ(restored.snapshotState().value("verticalScroll"), state.value("verticalScroll"));
    for (int lane = 0; lane < panel.laneCount(); ++lane)
        EXPECT_EQ(restored.laneRect(lane), panel.laneRect(lane));
    EXPECT_EQ(restored.laneGuid(restored.headerSelectedLane()), panel.laneGuid(7));
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_P(TimelineTrackHostTest, ViewportRestoreReplacesSelectionWithoutEditingClips)
{
    const QUuid first = QUuid::createUuid();
    const QUuid second = QUuid::createUuid();
    const auto clips = makeClipLedger({
        {first, sound_->guid, 0, 1000000000LL},
        {second, sound_->guid, 2000000000LL, 1000000000LL},
    });
    track_->comp<VNX::VTrack>()->clips = clips;
    VNX::TimelineEditorPanel panel;
    panel.bindItem(track_);
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>();
    ASSERT_NE(canvas, nullptr);
    ASSERT_TRUE(panel.selectLane({}));
    const auto emptyState = panel.snapshotState();
    ASSERT_TRUE(panel.selectLane(track_->guid));
    const auto laneState = panel.snapshotState();
    canvas->restoreSelectedClips({first, second}, second);
    const auto clipState = panel.snapshotState();
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();

    panel.restoreState(emptyState);
    EXPECT_TRUE(panel.selectedClipGuids().isEmpty());
    EXPECT_TRUE(panel.selectedClipAnchor().isNull());
    EXPECT_EQ(panel.headerSelectedLane(), -1);
    EXPECT_FALSE(panel.deleteSelectedClips());
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, clips);

    panel.restoreState(clipState);
    EXPECT_EQ(panel.selectedClipGuids(), (QSet<QUuid>{first, second}));
    EXPECT_EQ(panel.selectedClipAnchor(), second);
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), track_->guid);

    panel.restoreState(laneState);
    EXPECT_TRUE(panel.selectedClipGuids().isEmpty());
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), track_->guid);
    EXPECT_FALSE(panel.deleteSelectedClips());

    panel.restoreState(emptyState);
    EXPECT_EQ(panel.headerSelectedLane(), -1);
    EXPECT_TRUE(QUuid(panel.snapshotState().value("selectedLaneGuid").toString()).isNull());
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, clips);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_P(TimelineTrackHostTest, TrackZoomPassesLegacyLimitContinuouslyAndStopsAtOneMicrosecond)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(1000, 420);
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    auto* zoomIn = panel.findChild<QToolButton*>("timelineZoomIn");
    auto* scroll = panel.findChild<QScrollBar*>("timelineHorizontalScroll");
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(zoomIn, nullptr);
    ASSERT_NE(scroll, nullptr);
    const auto authored = track_->comp<VNX::VTrack>()->clips;
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    const int anchor = canvas->width() / 3;
    for (int i = 0; i < 4; ++i)
        zoomIn->click();
    EXPECT_DOUBLE_EQ(panel.zoomPercent(), 1600.0);
    const double zoom = panel.zoomPercent();
    panel.zoomAroundPixel(anchor, 1.0001);
    EXPECT_GT(panel.zoomPercent(), zoom);
    panel.zoomAroundPixel(anchor, 1.0 / 1.0001);
    EXPECT_NEAR(panel.zoomPercent(), zoom, 1e-9);
    const int64_t focus = panel.coordinateAtPixel(0, anchor + panel.horizontalScroll());
    panel.zoomAroundPixel(anchor, 1e12);
    EXPECT_GT(panel.zoomPercent(), 100000000.0);
    EXPECT_NEAR(panel.coordinateAtPixel(0, anchor + panel.horizontalScroll()), focus, 1);
    for (const QSize size : {QSize(320, 200), QSize(1600, 640)}) {
        panel.resize(size);
        drainEvents();
        const double offset = panel.horizontalScroll();
        EXPECT_EQ(panel.coordinateAtPixel(0, offset + canvas->width() - 1)
            - panel.coordinateAtPixel(0, offset), 1000);
        EXPECT_LT(panel.timeTicks().minorStep, 1e-6);
    }
    scroll->setValue(scroll->maximum());
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.horizontalScroll() + canvas->width() - 1),
        10000000000LL);
    scroll->setValue(0);
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.horizontalScroll()), 0);
    panel.zoomAroundPixel(anchor, 1e-15);
    EXPECT_DOUBLE_EQ(panel.zoomPercent(), 100.0);
    EXPECT_EQ(panel.coordinateAtPixel(0, canvas->width() - 1), 10000000000LL);
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, authored);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_F(TimelineEditorPanelTest, TrackZoomPanAndViewportCacheKeepBothAxesConsistent)
{
    auto* standalone = VNX::ProjectManager::instance().createItem(
        nullptr, QStringLiteral("StandaloneTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(standalone, nullptr);
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(standalone);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    auto* axis = panel.findChild<VNX::TimelineHeaderColumn*>();
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(axis, nullptr);
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    const QPoint point(10, canvas->height() / 3);
    QWheelEvent zoom(point, axis->mapToGlobal(point), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(axis, &zoom);
    EXPECT_DOUBLE_EQ(panel.verticalZoom(), 1.25);
    EXPECT_EQ(panel.zoomPercent(), 100);
    EXPECT_NEAR((panel.verticalScroll() + point.y()) / panel.verticalZoom(), point.y(), 1.0);
    panel.zoomAroundPixel(canvas->width() / 2, 4.0);
    panel.zoomVertically(canvas->height() / 2, 2.0);
    panel.setVerticalScroll(panel.verticalScroll() + 20);
    panel.setEditCursor(3500000000LL, true);
    const auto state = panel.snapshotState();
    VNX::TimelineEditorPanel restored;
    restored.resize(panel.size());
    restored.restoreState(state);
    restored.show();
    drainEvents();
    EXPECT_EQ(restored.boundItem(), standalone);
    EXPECT_EQ(restored.zoomPercent(), panel.zoomPercent());
    EXPECT_EQ(restored.horizontalScroll(), panel.horizontalScroll());
    EXPECT_EQ(restored.verticalScroll(), panel.verticalScroll());
    EXPECT_EQ(restored.verticalZoom(), panel.verticalZoom());
    EXPECT_EQ(restored.editCursor(), panel.editCursor());
    EXPECT_EQ(restored.snapshotState().value("timelineViewport"), state.value("timelineViewport"));
    panel.resize(1100, 500);
    drainEvents();
    EXPECT_EQ(panel.snapshotState().value("timelineViewport"), state.value("timelineViewport"));
    const int64_t cursor = panel.editCursor();
    auto* ruler = panel.findChild<VNX::TimelineRuler*>();
    const QPoint timePoint(ruler->timeAxisRect().center().x(), ruler->height() - 2);
    QMouseEvent resetTime(QEvent::MouseButtonDblClick, timePoint, ruler->mapToGlobal(timePoint),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ruler, &resetTime);
    QMouseEvent resetAmplitude(QEvent::MouseButtonDblClick, point, axis->mapToGlobal(point),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(axis, &resetAmplitude);
    EXPECT_EQ(panel.zoomPercent(), 100);
    EXPECT_DOUBLE_EQ(panel.verticalZoom(), 1.0);
    EXPECT_EQ(panel.horizontalScroll(), 0);
    EXPECT_EQ(panel.verticalScroll(), 0);
    EXPECT_EQ(panel.editCursor(), cursor);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_P(TimelineTrackHostTest, CursorReadoutPrecisionFollowsZoomWithoutChangingPosition)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    panel.setEditCursor(1354351650, true);
    EXPECT_EQ(panel.formatCoordinate(0, panel.editCursor()), QStringLiteral("1.4 s"));
    panel.setZoomPercent(1000);
    EXPECT_EQ(panel.formatCoordinate(0, panel.editCursor()), QStringLiteral("1.35 s"));
    panel.setZoomPercent(10000);
    EXPECT_EQ(panel.formatCoordinate(0, panel.editCursor()), QStringLiteral("1.354 s"));
    EXPECT_EQ(panel.editCursor(), 1354351650);
}

TEST_P(TimelineTrackHostTest, TrackHighZoomKeepsLongTimelineGeometryCursorAndCachedViewportPrecise)
{
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.resize(1000, 420);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 3600000000000LL, true));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    auto* scroll = panel.findChild<QScrollBar*>("timelineHorizontalScroll");
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(scroll, nullptr);
    const int64_t end = panel.clipStart(0, 0) + panel.clipDuration(0, 0);
    const auto authored = track_->comp<VNX::VTrack>()->clips;
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    panel.zoomAroundPixel(canvas->width() / 2, 1e15);
    scroll->setValue(scroll->maximum());
    EXPECT_GT(panel.horizontalScroll(), double(std::numeric_limits<int>::max()));
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.horizontalScroll()), end - 1000);
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.horizontalScroll() + canvas->width() - 1), end);
    panel.setEditCursor(end - 500, true);
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    EXPECT_TRUE(canvas->lastRenderData().clipItems.front().rect.intersects(canvas->rect()));
    EXPECT_NEAR(canvas->lastRenderData().editCursor.x, (canvas->width() - 1) / 2.0, 1.0);
    const int cursorX = panel.viewportPixelAtCoordinate(0, panel.editCursor());
    panel.beginCursorDrag(cursorX, true, false);
    panel.updateCursorDrag(cursorX + 30, true);
    panel.endCursorDrag();
    EXPECT_EQ(panel.editCursor(), panel.coordinateAtPixel(0,
        panel.horizontalScroll() + cursorX + 30));
    EXPECT_GT(panel.editCursor(), end - 500);
    EXPECT_EQ(panel.formatCoordinate(0, end - 500), QStringLiteral("3600.9999995 s"));
    const double offset = panel.horizontalScroll();
    panel.setHorizontalScroll(offset - 17);
    EXPECT_DOUBLE_EQ(panel.horizontalScroll(), offset - 17);
    const auto state = panel.snapshotState();
    VNX::TimelineEditorPanel restored;
    restored.resize(panel.size());
    restored.restoreState(state);
    restored.show();
    drainEvents();
    EXPECT_DOUBLE_EQ(restored.zoomPercent(), panel.zoomPercent());
    EXPECT_DOUBLE_EQ(restored.horizontalScroll(), panel.horizontalScroll());
    EXPECT_EQ(restored.editCursor(), panel.editCursor());
    EXPECT_EQ(restored.snapshotState().value("timelineViewport"), state.value("timelineViewport"));
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, authored);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_P(TimelineTrackHostTest, TrackMicrosecondViewportStaysPreciseAcrossTheSignedTimeRange)
{
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.resize(1600, 420);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, minimum, true));
    ASSERT_TRUE(panel.placeItem(sound_, 0, maximum - 1000000000, true));
    drainEvents();
    ASSERT_EQ(panel.viewKind(), (GetParam() ? VNX::TimelineEditorPanel::ViewKind::Piece : VNX::TimelineEditorPanel::ViewKind::Track));
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    auto* ruler = panel.findChild<VNX::TimelineRuler*>("timelineRuler");
    auto* scroll = panel.findChild<QScrollBar*>("timelineHorizontalScroll");
    auto* cursorValue = panel.findChild<QLabel*>("timelineCursorValue");
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(ruler, nullptr);
    ASSERT_NE(scroll, nullptr);
    ASSERT_NE(cursorValue, nullptr);
    const auto authored = track_->comp<VNX::VTrack>()->clips;
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    panel.zoomAroundPixel(0, 1e25);
    const auto mouse = [](QWidget* widget, QEvent::Type type, const QPoint& pos,
                          Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, pos, widget->mapToGlobal(pos), button, buttons, Qt::AltModifier);
        QApplication::sendEvent(widget, &event);
    };
    for (const bool positive : {false, true}) {
        SCOPED_TRACE(positive);
        scroll->setValue(positive ? scroll->maximum() : 0);
        const int64_t start = positive ? maximum - 1000 : minimum;
        EXPECT_EQ(panel.coordinateAtViewportPixel(0, 0), start);
        EXPECT_EQ(panel.coordinateAtViewportPixel(0, canvas->width() - 1), start + 1000);
        panel.setEditCursor(start + 500, true);
        canvas->repaint();
        ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
        EXPECT_TRUE(canvas->lastRenderData().clipItems.front().rect.intersects(canvas->rect()));
        EXPECT_NEAR(canvas->lastRenderData().editCursor.x, (canvas->width() - 1) / 2.0, 0.5);
        const auto ticks = panel.timeTicks();
        ASSERT_GT(ticks.ticks.size(), 10);
        const int64_t grid = std::llround(ticks.minorStep * 1e9);
        int previousPixel = -1;
        for (const auto& tick : ticks.ticks) {
            EXPECT_GT(tick.pixel, previousPixel);
            const int64_t coordinate = panel.coordinateAtViewportPixel(0, tick.pixel);
            EXPECT_EQ(coordinate % grid, 0);
            EXPECT_EQ(panel.snapCoordinate(0, coordinate + 1, false), coordinate);
            previousPixel = tick.pixel;
        }
        const int x = panel.viewportPixelAtCoordinate(0, panel.editCursor());
        panel.beginCursorDrag(x, true, false);
        EXPECT_TRUE(containsCursorValue(cursorValue, positive
            ? QStringLiteral("9223372036.85477531 s")
            : QStringLiteral("-9223372036.85477531 s"))) << cursorValue->text().toStdString();
        panel.updateCursorDrag(x + 30, true);
        EXPECT_EQ(panel.editCursor(), start + std::llround(double(x + 30) * 1000 / (canvas->width() - 1)));
        panel.endCursorDrag();

        const int pan = positive ? -17 : 17;
        const int64_t cursor = panel.editCursor();
        const double beforePan = panel.viewportPositionAtCoordinate(0, cursor);
        const QPoint from(panel.laneHeaderWidth() + canvas->width() / 2, ruler->timeRulerHeight() / 2);
        QWheelEvent wheel(from, ruler->mapToGlobal(from), QPoint(-pan, 0), QPoint(),
            Qt::NoButton, Qt::ShiftModifier, Qt::ScrollUpdate, false);
        QApplication::sendEvent(ruler, &wheel);
        EXPECT_NEAR(panel.viewportPositionAtCoordinate(0, cursor), beforePan - pan, 1e-9);
        panel.panHorizontally((positive ? -2 : 2) * canvas->width());
        const double beforeZoom = panel.viewportPositionAtCoordinate(0, cursor);
        panel.zoomAroundPixel(canvas->width() / 3, 0.5);
        panel.zoomAroundPixel(canvas->width() / 3, 2.0);
        EXPECT_NEAR(panel.viewportPositionAtCoordinate(0, cursor), beforeZoom, 1e-9);
        const auto state = QJsonDocument::fromJson(QJsonDocument::fromVariant(panel.snapshotState()).toJson())
            .toVariant().toMap();
        VNX::TimelineEditorPanel restored;
        restored.resize(panel.size());
        restored.restoreState(state);
        restored.show();
        drainEvents();
        EXPECT_EQ(restored.editCursor(), cursor);
        EXPECT_EQ(restored.coordinateAtViewportPixel(0, 0), panel.coordinateAtViewportPixel(0, 0));
        EXPECT_DOUBLE_EQ(restored.viewportPositionAtCoordinate(0, cursor), panel.viewportPositionAtCoordinate(0, cursor));
        EXPECT_EQ(restored.snapshotState().value("timelineViewport"), state.value("timelineViewport"));

        const int64_t visibleStart = panel.coordinateAtViewportPixel(0, 0);
        const int64_t visibleEnd = panel.coordinateAtViewportPixel(0, canvas->width() - 1);
        panel.beginCursorDrag(canvas->width() / 2, true, true);
        panel.updateCursorDrag(positive ? -40 : canvas->width() + 40, true);
        QEventLoop loop;
        QTimer::singleShot(60, &loop, &QEventLoop::quit);
        loop.exec();
        panel.endCursorDrag();
        if (positive)
            EXPECT_LT(panel.editCursor(), visibleStart);
        else
            EXPECT_GT(panel.editCursor(), visibleEnd);
    }
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, authored);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);

    panel.setHorizontalScroll(panel.pixelAtCoordinate(0, maximum));
    const QPoint from(canvas->width() / 3, panel.timelineMetrics().laneHeight / 2);
    const QPoint to = from - QPoint(27, 0);
    const int64_t delta = panel.coordinateAtViewportPixel(0, to.x()) - panel.coordinateAtViewportPixel(0, from.x());
    ASSERT_LT(delta, 0);
    mouse(canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(panel.clipStart(0, 1), maximum - 1000000000 + delta);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount + 1);
}

TEST_F(TimelineEditorPanelTest, TrackBindingSelectsOwningPieceAndPreservesItsViewport)
{
    VNX::TimelineEditorPanel panel;
    panel.bindItem(track_->parent);
    panel.resize(900, 400);
    panel.show();
    drainEvents();
    panel.zoomAroundPixel(200, 10000.0);
    const auto state = panel.snapshotState();
    panel.bindItem(track_);
    EXPECT_EQ(panel.boundItem(), track_->parent);
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), track_->guid);
    EXPECT_EQ(panel.snapshotState().value("timelineViewport"), state.value("timelineViewport"));
    EXPECT_EQ(panel.zoomPercent(), state.value("zoom").toDouble());
    auto* orphan = VNX::ProjectManager::instance().createItem(nullptr,
        QStringLiteral("Orphan"), VNX::ProjectItem::Type::Track);
    panel.bindItem(orphan);
    EXPECT_EQ(panel.boundItem(), orphan);
    EXPECT_EQ(panel.viewKind(), VNX::TimelineEditorPanel::ViewKind::Track);
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), orphan->guid);
    EXPECT_TRUE(VNX::TimelineEditorPanel::acceptsItem(track_));
    EXPECT_EQ(VNX::TimelineEditorPanel::timelineOwner(orphan), orphan);
    EXPECT_EQ(VNX::TimelineEditorPanel::timelineOwner(track_), track_->parent);

    auto& manager = VNX::ProjectManager::instance();
    const QUuid guid = orphan->guid;
    panel.zoomAroundPixel(200, 8.0);
    panel.setEditCursor(1000000000LL, true);
    const auto standaloneView = panel.snapshotState();
    ASSERT_TRUE(manager.moveItem(orphan, piece_));
    EXPECT_EQ(panel.boundItem(), piece_);
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), guid);
    EXPECT_EQ(panel.zoomPercent(), standaloneView.value("zoom").toDouble());
    EXPECT_EQ(panel.editCursor(), 1000000000LL);
    VNX::TimelineEditorPanel restored;
    restored.restoreState(standaloneView);
    EXPECT_EQ(restored.boundItem(), piece_);
    EXPECT_EQ(restored.laneGuid(restored.headerSelectedLane()), guid);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    orphan = manager.findItemByGuid(guid);
    ASSERT_NE(orphan, nullptr);
    panel.bindItem(orphan);
    EXPECT_EQ(panel.boundItem(), orphan);
    EXPECT_EQ(panel.viewKind(), VNX::TimelineEditorPanel::ViewKind::Track);
}

TEST_P(TimelineTrackHostTest, RulerAndHandlePositionCursorWithoutPanning)
{
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.resize(900, 360);
    panel.move(100, 100);
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    auto* ruler = panel.findChild<VNX::TimelineRuler*>("timelineRuler");
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(ruler, nullptr);
    auto* value = ruler->findChild<QLabel*>("timelineCursorValue");
    ASSERT_NE(value, nullptr);
    EXPECT_FALSE(value->isVisible());
    const int header = panel.laneHeaderWidth();
    const QRect axis = ruler->timeAxisRect();
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    const auto mouse = [&](QEvent::Type type, const QPoint& pos, Qt::MouseButton button,
                           Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, pos, ruler->mapToGlobal(pos), button, buttons, modifiers);
        QApplication::sendEvent(ruler, &event);
    };
    const QPoint rulerClick(header + canvas->width() / 3, 2);
    mouse(QEvent::MouseButtonPress, rulerClick, Qt::LeftButton, Qt::LeftButton);
    EXPECT_TRUE(value->isVisible());
    mouse(QEvent::MouseButtonRelease, rulerClick, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(panel.editCursor(), panel.snapCoordinate(0,
        panel.coordinateAtViewportPixel(0, rulerClick.x() - header), false));
    const QPoint click(rulerClick.x(), axis.bottom() - 2);
    mouse(QEvent::MouseMove, click, Qt::NoButton, Qt::NoButton);
    mouse(QEvent::MouseButtonPress, click, Qt::LeftButton, Qt::LeftButton);
    EXPECT_EQ(panel.editCursor(), panel.snapCoordinate(0, panel.coordinateAtPixel(0, click.x() - header), false));
    EXPECT_TRUE(value->isVisible());
    const QPoint release = click + QPoint(43, 0);
    mouse(QEvent::MouseMove, release, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, release, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(panel.editCursor(), panel.snapCoordinate(0, panel.coordinateAtPixel(0, release.x() - header), false));
    EXPECT_FALSE(value->isVisible());
    EXPECT_EQ(panel.horizontalScroll(), 0);

    const int64_t clickedCursor = panel.editCursor();
    panel.setZoomPercent(200);
    panel.setHorizontalScroll(panel.pixelAtCoordinate(0, clickedCursor) - canvas->width() / 3);
    drainEvents();
    const int scroll = panel.horizontalScroll();
    for (const auto modifiers : {Qt::KeyboardModifiers(Qt::AltModifier), Qt::KeyboardModifiers(Qt::NoModifier)}) {
        const int x = panel.viewportPixelAtCoordinate(0, panel.editCursor());
        const QPoint from(header + x + 2, ruler->cursorHandleRect().center().y());
        const QPoint to = from + QPoint(43, 0);
        mouse(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
        const int64_t before = panel.editCursor();
        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, modifiers);
        EXPECT_EQ(panel.editCursor(), before);
        EXPECT_TRUE(value->isVisible());
        mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, modifiers);
        EXPECT_TRUE(containsCursorValue(value, panel.formatCoordinate(0, panel.editCursor())));
        mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, modifiers);
        EXPECT_FALSE(value->isVisible());
        const int64_t coordinate = panel.coordinateAtViewportPixel(0, x + 43);
        EXPECT_EQ(panel.editCursor(), modifiers.testFlag(Qt::AltModifier)
            ? coordinate : panel.snapCoordinate(0, coordinate, false));
        EXPECT_EQ(panel.horizontalScroll(), scroll);
    }

    mouse(QEvent::MouseButtonPress, rulerClick, Qt::LeftButton, Qt::LeftButton);
    const QPoint dragTo(rulerClick.x() - 20, axis.bottom() - 2);
    mouse(QEvent::MouseMove, dragTo, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, dragTo, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(panel.editCursor(), panel.snapCoordinate(0,
        panel.coordinateAtViewportPixel(0, dragTo.x() - header), false));
    EXPECT_EQ(panel.horizontalScroll(), scroll);
    EXPECT_FALSE(value->isVisible());

    const QPoint doubleClick(header + canvas->width() / 3, axis.bottom() - 2);
    mouse(QEvent::MouseButtonPress, doubleClick, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, doubleClick, Qt::LeftButton, Qt::NoButton);
    mouse(QEvent::MouseButtonDblClick, doubleClick, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, doubleClick, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(panel.zoomPercent(), 100);
    EXPECT_EQ(panel.horizontalScroll(), 0);
    EXPECT_FALSE(panel.isCursorDragActive());
    EXPECT_FALSE(value->isVisible());
    const int64_t cursor = panel.editCursor();
    const QPoint corner(header / 2, ruler->height() / 2);
    mouse(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, corner, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(panel.editCursor(), cursor);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_P(TimelineTrackHostTest, TrackCursorHandleAndLineSupportExactValueEntry)
{
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.resize(900, 360);
    panel.move(100, 100);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 2000000000LL, true));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    auto* ruler = panel.findChild<VNX::TimelineRuler*>("timelineRuler");
    const auto clips = track_->comp<VNX::VTrack>()->clips.toJsonObject();
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    const auto editor = [&]() -> QDialog* {
        for (auto* dialog : panel.findChildren<QDialog*>("timelineCursorValueEditor"))
            if (dialog->isVisible()) return dialog;
        return nullptr;
    };
    for (QWidget* widget : {static_cast<QWidget*>(ruler), static_cast<QWidget*>(canvas)}) {
        SCOPED_TRACE(widget->objectName().toStdString());
        const auto point = [&]() {
            const int x = panel.viewportPixelAtCoordinate(0, panel.editCursor());
            return widget == ruler
                ? QPoint(panel.laneHeaderWidth() + x, ruler->cursorHandleRect().center().y())
                : QPoint(x, canvas->height() / 2);
        };
        panel.setEditCursor(2250000000LL, true);
        QMouseEvent click(QEvent::MouseButtonDblClick, point(), widget->mapToGlobal(point()),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &click);
        drainEvents();
        ASSERT_TRUE(click.isAccepted());
        auto* dialog = editor();
        ASSERT_NE(dialog, nullptr);
        EXPECT_FALSE(panel.isCursorDragActive());
        auto* input = dialog->findChild<QDoubleSpinBox*>("timelineCursorValueInput");
        ASSERT_NE(input, nullptr);
        EXPECT_DOUBLE_EQ(input->value(), 2.25);
        input->setValue(2.375123456);
        EXPECT_EQ(panel.editCursor(), 2250000000LL);
        QKeyEvent accept(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(dialog, &accept);
        drainEvents();
        EXPECT_EQ(editor(), nullptr);
        EXPECT_EQ(panel.editCursor(), 2375123456LL);

        QContextMenuEvent context(QContextMenuEvent::Mouse, point(), widget->mapToGlobal(point()));
        QApplication::sendEvent(widget, &context);
        drainEvents();
        ASSERT_TRUE(context.isAccepted());
        auto* menu = panel.findChild<QMenu*>("timelineCursorContextMenu");
        ASSERT_NE(menu, nullptr);
        EXPECT_TRUE(menu->isVisible());
        EXPECT_EQ(editor(), nullptr);
        EXPECT_EQ(panel.editCursor(), 2375123456LL);
        auto* enter = menu->findChild<QAction*>("timelineCursorEnterValue");
        ASSERT_NE(enter, nullptr);
        menu->setActiveAction(enter);
        QKeyEvent choose(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(menu, &choose);
        drainEvents();
        dialog = editor();
        ASSERT_NE(dialog, nullptr);
        input = dialog->findChild<QDoubleSpinBox*>("timelineCursorValueInput");
        EXPECT_DOUBLE_EQ(input->value(), 2.375123456);
        input->setValue(4.0);
        QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(dialog, &cancel);
        drainEvents();
        EXPECT_EQ(editor(), nullptr);
        EXPECT_EQ(panel.editCursor(), 2375123456LL);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.toJsonObject(), clips);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_P(TimelineTrackHostTest, CursorValueInputDiscardsStaleHiddenAndReboundEdits)
{
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    panel.setEditCursor(4000000001LL, true);
    drainEvents();
    const auto open = [&]() -> QDialog* {
        panel.editCursorValue();
        drainEvents();
        for (auto* dialog : panel.findChildren<QDialog*>("timelineCursorValueEditor"))
            if (dialog->isVisible()) return dialog;
        return nullptr;
    };
    auto* dialog = open();
    ASSERT_NE(dialog, nullptr);
    dialog->accept();
    EXPECT_EQ(panel.editCursor(), 4000000001LL);
    dialog = open();
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<QDoubleSpinBox*>("timelineCursorValueInput")->setValue(7.25);
    panel.setEditCursor(5000000000LL, true);
    dialog->accept();
    EXPECT_EQ(panel.editCursor(), 5000000000LL);
    dialog = open();
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<QDoubleSpinBox*>("timelineCursorValueInput")->setValue(7.25);
    panel.hide();
    EXPECT_FALSE(dialog->isVisible());
    EXPECT_EQ(panel.editCursor(), 5000000000LL);
    panel.show();
    dialog = open();
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<QDoubleSpinBox*>("timelineCursorValueInput")->setValue(7.25);
    panel.bindItem(piece_);
    EXPECT_FALSE(dialog->isVisible());
    EXPECT_EQ(panel.editCursor(), 5000000000LL);
}

TEST_P(TimelineTrackHostTest, TrackCursorLineDragsAcrossCanvasAndTakesPriorityOverClips)
{
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 4000000000LL, true));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    auto* value = panel.findChild<QLabel*>("timelineCursorValue");
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(value, nullptr);
    canvas->restoreSelectedClips({}, QUuid());
    const auto authored = track_->comp<VNX::VTrack>()->clips.toJsonObject();
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    int selections = 0;
    QObject::connect(&panel, &VNX::TimelineEditorPanel::clipSelectionRequested,
        &panel, [&] { ++selections; });
    const auto mouse = [&](QEvent::Type type, const QPoint& pos, Qt::MouseButton button,
                           Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, pos, canvas->mapToGlobal(pos), button, buttons, modifiers);
        QApplication::sendEvent(canvas, &event);
    };
    for (const int y : {1, canvas->height() / 2, canvas->height() - 2}) {
        for (const auto modifiers : {Qt::KeyboardModifiers(Qt::AltModifier), Qt::KeyboardModifiers(Qt::NoModifier)}) {
            panel.setEditCursor(4500000000LL, true);
            const int x = panel.viewportPixelAtCoordinate(0, panel.editCursor());
            const QPoint from(x + 2, y);
            const QPoint to = from + QPoint(13, 0);
            mouse(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
            EXPECT_EQ(canvas->hoveredClipIndex(), -1);
            mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, modifiers);
            EXPECT_EQ(panel.editCursor(), 4500000000LL);
            EXPECT_TRUE(panel.isCursorDragActive());
            EXPECT_TRUE(canvas->isGestureActive());
            EXPECT_TRUE(value->isVisible());
            mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, modifiers);
            const int64_t coordinate = panel.coordinateAtPixel(0, x + 13);
            EXPECT_EQ(panel.editCursor(), panel.snapCoordinate(0, coordinate,
                modifiers.testFlag(Qt::AltModifier)));
            EXPECT_TRUE(containsCursorValue(value, panel.formatCoordinate(0, panel.editCursor())));
            mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, modifiers);
            EXPECT_FALSE(panel.isCursorDragActive());
            EXPECT_FALSE(canvas->isGestureActive());
            EXPECT_FALSE(value->isVisible());
            EXPECT_TRUE(canvas->selectedClipGuids().isEmpty());
        }
    }
    panel.setEditCursor(4500000000LL, true);
    const QPoint cursorPoint(panel.viewportPixelAtCoordinate(0, panel.editCursor()), panel.timelineMetrics().laneHeight / 2);
    mouse(QEvent::MouseButtonDblClick, cursorPoint, Qt::LeftButton, Qt::LeftButton);
    EXPECT_FALSE(panel.isCursorDragActive());
    auto* editor = panel.findChild<QDialog*>("timelineCursorValueEditor");
    ASSERT_NE(editor, nullptr);
    EXPECT_TRUE(editor->isVisible());
    editor->reject();
    mouse(QEvent::MouseButtonRelease, cursorPoint, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(selections, 0);
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.toJsonObject(), authored);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
    for (const int side : {-8, 8}) {
        canvas->restoreSelectedClips({}, QUuid());
        const QPoint from = cursorPoint + QPoint(side, 0);
        const QPoint to = from + QPoint(side * 2, 0);
        mouse(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
        EXPECT_EQ(canvas->hoveredClipIndex(), 0);
        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
        EXPECT_FALSE(panel.isCursorDragActive());
        mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
        EXPECT_NE(canvas->previewStart(), 4000000000LL);
        EXPECT_EQ(panel.editCursor(), 4500000000LL);
        QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(canvas, &cancel);
        mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    }
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.toJsonObject(), authored);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_P(TimelineTrackHostTest, TrackCursorHandleAndLineScrollBeyondViewportAndStopAtTrackBounds)
{
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, -2125000000LL, true));
    ASSERT_TRUE(panel.placeItem(sound_, 0, 23250000000LL, true));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    auto* ruler = panel.findChild<VNX::TimelineRuler*>("timelineRuler");
    auto* scroll = panel.findChild<QScrollBar*>("timelineHorizontalScroll");
    auto* value = panel.findChild<QLabel*>("timelineCursorValue");
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(ruler, nullptr);
    ASSERT_NE(scroll, nullptr);
    ASSERT_NE(value, nullptr);
    const auto authored = track_->comp<VNX::VTrack>()->clips.toJsonObject();
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    const int64_t trackStart = panel.viewOriginCoordinate();
    const int64_t trackEnd = panel.coordinateAtPixel(0, canvas->width() - 1);
    EXPECT_EQ(trackStart, -3000000000LL);
    EXPECT_GT(trackEnd, 20000000000LL);
    panel.setZoomPercent(225);
    const auto wait = [](int milliseconds) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };
    const auto mouse = [](QWidget* widget, QEvent::Type type, const QPoint& pos,
                          Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, pos, widget->mapToGlobal(pos), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    };
    for (QWidget* widget : {static_cast<QWidget*>(ruler), static_cast<QWidget*>(canvas)}) {
        const int offset = widget == ruler ? panel.laneHeaderWidth() : 0;
        const int y = widget == ruler ? ruler->cursorHandleRect().center().y() : canvas->height() / 2;
        for (const int direction : {-1, 1}) {
            scroll->setValue(scroll->maximum() / 2);
            panel.setEditCursor(panel.coordinateAtPixel(0,
                panel.horizontalScroll() + canvas->width() / 2), true);
            const int beforeScroll = panel.horizontalScroll();
            const int64_t visibleStart = panel.coordinateAtPixel(0, beforeScroll);
            const int64_t visibleEnd = panel.coordinateAtPixel(0, beforeScroll + canvas->width() - 1);
            const QPoint from(offset + panel.pixelAtCoordinate(0, panel.editCursor()) - beforeScroll, y);
            const QPoint to(offset + (direction < 0 ? -200 : canvas->width() + 200), y);
            mouse(widget, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            mouse(widget, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
            EXPECT_TRUE(panel.isCursorDragActive());
            EXPECT_TRUE(value->isVisible());
            QEventLoop loop;
            const int limit = direction < 0 ? scroll->minimum() : scroll->maximum();
            const double pixelLimit = direction < 0 ? 0.0
                : panel.pixelAtCoordinate(0, trackEnd) - canvas->width() + 1;
            const auto connection = QObject::connect(scroll, &QScrollBar::valueChanged, &loop,
                [&](int position) { if (position == limit) loop.quit(); });
            QTimer::singleShot(1500, &loop, &QEventLoop::quit);
            loop.exec();
            QObject::disconnect(connection);
            EXPECT_EQ(panel.horizontalScroll(), std::round(pixelLimit));
            EXPECT_EQ(panel.editCursor(), direction < 0 ? trackStart : trackEnd);
            if (direction < 0)
                EXPECT_LT(panel.editCursor(), visibleStart);
            else
                EXPECT_GT(panel.editCursor(), visibleEnd);
            mouse(widget, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
            EXPECT_FALSE(panel.isCursorDragActive());
            EXPECT_FALSE(canvas->isGestureActive());
            EXPECT_FALSE(value->isVisible());
            const int64_t released = panel.editCursor();
            wait(40);
            EXPECT_EQ(panel.editCursor(), released);
            EXPECT_EQ(panel.horizontalScroll(), std::round(pixelLimit));
        }
        for (const int stop : {0, 1, 2, 3}) {
            scroll->setValue(scroll->maximum() / 2);
            panel.setEditCursor(panel.coordinateAtPixel(0,
                panel.horizontalScroll() + canvas->width() / 2), true);
            const int64_t before = panel.editCursor();
            const QPoint from(offset + panel.pixelAtCoordinate(0, before) - panel.horizontalScroll(), y);
            const QPoint to(offset + canvas->width() + 200, y);
            mouse(widget, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            mouse(widget, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
            if (stop == 0) {
                mouse(widget, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
            } else if (stop == 1) {
                QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(widget, &cancel);
                EXPECT_EQ(panel.editCursor(), before);
            } else if (stop == 2) {
                panel.hide();
            } else {
                panel.bindItem(piece_);
            }
            EXPECT_FALSE(panel.isCursorDragActive());
            EXPECT_FALSE(canvas->isGestureActive());
            EXPECT_FALSE(value->isVisible());
            const int stoppedScroll = panel.horizontalScroll();
            const int64_t stopped = panel.editCursor();
            wait(40);
            EXPECT_EQ(panel.horizontalScroll(), stoppedScroll);
            EXPECT_EQ(panel.editCursor(), stopped);
            if (stop == 2)
                panel.show();
            if (stop == 3) {
                panel.bindItem(track_);
                panel.setZoomPercent(225);
            }
            drainEvents();
        }
    }
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.toJsonObject(), authored);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
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
        EXPECT_TRUE(value->isVisible());
        panel.reset();
        drainEvents();
        EXPECT_TRUE(value.isNull());
    }
}

TEST_P(TimelineTrackHostTest, ImportIntoTrackCreatesOneClipPerChildAndPreservesOrderAcrossUndoRedo)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* binding = VNX::findFileBindingForItem(*manager.currentProject(), track_);
    ASSERT_NE(binding, nullptr);
    const QString path = tempDir_.path() + QStringLiteral("/AutoClipImport.wav");
    const QString secondPath = tempDir_.path() + QStringLiteral("/SecondClipImport.wav");
    ASSERT_TRUE(writePcm16Wav(path, 4800, 1, 48000));
    ASSERT_TRUE(writePcm16Wav(secondPath, 9600, 1, 48000));
    VNX::TimelineEditorPanel panel;
    panel.bindItem(track_);
    auto* command = VNX::ImportAssetsCommand::pushImport({path, secondPath}, binding, track_);
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->successCount(), 2);
    ASSERT_EQ(panel.clipCount(0), 2);
    const auto imported = command->importedItemGuids();
    ASSERT_EQ(imported.size(), 2);
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(panel.clipTargetGuid(0, i), imported.at(i));
        EXPECT_EQ(panel.clipStart(0, i), 0);
    }
    const auto original = track_->comp<VNX::VTrack>()->clips;
    auto* stack = VNX::UndoRedoManager::instance().undoStack();
    for (int repeat = 0; repeat < 3; ++repeat) {
        stack->undo();
        EXPECT_EQ(panel.clipCount(0), 0);
        for (const auto& guid : imported)
            EXPECT_EQ(manager.findItemByGuid(guid), nullptr);
        stack->redo();
        EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, original);
        for (const auto& guid : imported)
            EXPECT_NE(manager.findItemByGuid(guid), nullptr);
    }
    const int before = stack->index();
    ASSERT_TRUE(panel.importAudioFiles({path, secondPath}, 0, 3000000000LL, true));
    ASSERT_EQ(panel.clipCount(0), 4);
    EXPECT_EQ(panel.clipStart(0, 2), 3000000000LL);
    EXPECT_EQ(panel.clipStart(0, 3), 3000000000LL);
    const auto placed = track_->comp<VNX::VTrack>()->clips;
    const int after = stack->index();
    while (stack->index() > before) stack->undo();
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.items, original.items);
    while (stack->index() < after) stack->redo();
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips, placed);
}

TEST_P(TimelineTrackHostTest, TrackSourceTrimUpdatesWaveformButPreservesAuthoredOverview)
{
    VNX::AssetInfo asset;
    asset.sourceID = 9101;
    asset.relativePath = QStringLiteral("track-span.wav");
    asset.absolutePath = tempDir_.path() + QStringLiteral("/track-span.wav");
    asset.metadata.sampleRate = 48000;
    asset.metadata.totalSamples = 480000;
    asset.metadata.durationSec = 10.0;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    VNX::SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 96000;
    source.trimEndFrame = 384000;
    auto* sound = sound_->comp<VNX::VSound>();
    sound->sources.plain.entries = {source};
    sound->sources.plain.active = source.entryId;
    VNX::TimelineEditorPanel panel;
    panel.resize(1000, 420);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, 18000000000LL, true));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>("timelineLaneViewport");
    ASSERT_NE(canvas, nullptr);
    EXPECT_EQ(panel.coordinateAtPixel(0, canvas->width() - 1), 24000000000LL);
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    auto item = canvas->lastRenderData().clipItems.front();
    EXPECT_DOUBLE_EQ(item.waveformStartFrame, 96000.0);
    EXPECT_DOUBLE_EQ(item.waveformFrameCount, 288000.0);
    const auto authored = track_->comp<VNX::VTrack>()->clips.toJsonObject();
    ASSERT_TRUE(writePcm16Wav(asset.absolutePath, 480000, 1, 48000));
    ASSERT_TRUE(VNX::WaveformCache::generate(asset.absolutePath, 2048));
    panel.setZoomPercent(800);
    auto* horizontal = panel.findChild<QScrollBar*>("timelineHorizontalScroll");
    horizontal->setValue(horizontal->maximum());
    drainEvents();
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    item = canvas->lastRenderData().clipItems.front();
    EXPECT_GT(item.waveformStartFrame, 96000.0);
    EXPECT_LT(item.waveformFrameCount, 288000.0);
    EXPECT_LE(item.waveformStartFrame + item.waveformFrameCount, 384001.0);
    panel.zoomAroundPixel(canvas->width() - 1, 1e12);
    horizontal->setValue(horizontal->maximum());
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    item = canvas->lastRenderData().clipItems.front();
    EXPECT_FALSE(item.waveformRect.isEmpty());
    EXPECT_GT(item.waveformStartFrame, 383999.95);
    EXPECT_GT(item.waveformFrameCount, 0.0);
    EXPECT_LE(item.waveformFrameCount, 0.049);
    EXPECT_NEAR(item.waveformStartFrame + item.waveformFrameCount, 384000.0, 1e-5);
    panel.setZoomPercent(100);
    sound->sources.plain.entries.front().trimEndFrame = 240000;
    emit VNX::ProjectManager::instance().itemPropertyChanged(sound_, QStringLiteral("sources"));
    drainEvents();
    EXPECT_EQ(panel.coordinateAtPixel(0, canvas->width() - 1), 24000000000LL);
    EXPECT_EQ(panel.clipSourceDuration(0, 0), std::optional<int64_t>(3000000000LL));
    EXPECT_EQ(panel.clipDuration(0, 0), 6000000000LL);
    EXPECT_EQ(track_->comp<VNX::VTrack>()->clips.toJsonObject(), authored);
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    item = canvas->lastRenderData().clipItems.front();
    EXPECT_LT(item.waveformRect.width(), item.rect.width());
    EXPECT_DOUBLE_EQ(item.waveformStartFrame, 96000.0);
    EXPECT_DOUBLE_EQ(item.waveformFrameCount, 144000.0);
    sound->sources.plain.entries.front().trimEndFrame = 144000;
    emit VNX::ProjectManager::instance().itemPropertyChanged(sound_, QStringLiteral("sources"));
    drainEvents();
    EXPECT_EQ(panel.coordinateAtPixel(0, canvas->width() - 1), 24000000000LL);
    EXPECT_EQ(panel.clipSourceDuration(0, 0), std::optional<int64_t>(1000000000LL));
    EXPECT_EQ(panel.clipDuration(0, 0), 6000000000LL);
    ASSERT_TRUE(panel.moveClip(0, panel.clipGuid(0, 0), 0,
        std::numeric_limits<int64_t>::max() - 10000000000LL, true));
    panel.zoomAroundPixel(canvas->width() - 1, 1e25);
    horizontal->setValue(horizontal->maximum());
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    item = canvas->lastRenderData().clipItems.front();
    EXPECT_FALSE(item.rect.isEmpty());
    EXPECT_TRUE(item.waveformRect.isEmpty());
    EXPECT_DOUBLE_EQ(item.waveformStartFrame, 0.0);
    EXPECT_DOUBLE_EQ(item.waveformFrameCount, 0.0);
}

TEST_P(TimelineTrackHostTest, TrackViewportRestoresBeforeProjectLoads)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::TimelineEditorPanel panel;
    panel.resize(800, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(panel.placeItem(sound_, 0, std::numeric_limits<int64_t>::max() - 1000000000, true));
    drainEvents();
    panel.zoomAroundPixel(panel.timelineViewportWidth() / 2, 1e25);
    auto* scroll = panel.findChild<QScrollBar*>("timelineHorizontalScroll");
    scroll->setValue(scroll->maximum());
    panel.panHorizontally(-17);
    drainEvents();
    const auto state = panel.snapshotState();
    ASSERT_GT(state.value("zoom").toDouble(), double(std::numeric_limits<int>::max()));
    const QString path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    VNX::TimelineEditorPanel restored;
    restored.resize(800, 360);
    restored.restoreState(state);
    restored.show();
    drainEvents();
    EXPECT_EQ(restored.viewKind(), VNX::TimelineEditorPanel::ViewKind::Empty);
    ASSERT_TRUE(manager.loadProject(path));
    drainEvents();
    EXPECT_EQ(restored.viewKind(), (GetParam() ? VNX::TimelineEditorPanel::ViewKind::Piece : VNX::TimelineEditorPanel::ViewKind::Track));
    EXPECT_EQ(restored.zoomPercent(), state.value("zoom").toDouble());
    EXPECT_EQ(restored.snapshotState().value("timelineViewport"), state.value("timelineViewport"));
}

TEST_P(TimelineTrackHostTest, TrackRatioCacheRestoresIntoPreciseViewport)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.restoreState({
        {QStringLiteral("itemGuid"), track_->guid.toString()},
        {QStringLiteral("zoom"), 400.0},
        {QStringLiteral("trackViewport"), QVariantMap{{QStringLiteral("horizontalRatio"), 0.75}}},
    });
    panel.show();
    drainEvents();
    EXPECT_EQ(panel.coordinateAtViewportPixel(0, 0), 5625000000LL);
    EXPECT_EQ(panel.coordinateAtViewportPixel(0, panel.timelineViewportWidth() - 1), 8125000000LL);
}

TEST_P(TimelineTrackHostTest, TrackLegacyPixelCacheStartsWithFullOverview)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(480, 300);
    panel.restoreState({
        {QStringLiteral("itemGuid"), track_->guid.toString()},
        {QStringLiteral("zoom"), 246},
        {QStringLiteral("horizontalScroll"), 140},
        {QStringLiteral("laneHeaderWidth"), 400},
    });
    panel.show();
    drainEvents();
    EXPECT_EQ(panel.zoomPercent(), 100);
    EXPECT_EQ(panel.horizontalScroll(), 0);
    EXPECT_EQ(panel.coordinateAtPixel(0, panel.timelineViewportWidth() - 1), 10000000000LL);
}

TEST_F(TimelineEditorPanelTest, DeleteShortcutRemovesSelectedClip)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(placeOnLane(panel, sound_, 0, 1000000000LL, true));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(
        QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    const QPointF pressPos(panel.pixelAtCoordinate(0, 1500000000LL)
                               - panel.horizontalScroll(), 24.0);
    QMouseEvent press(QEvent::MouseButtonPress, pressPos, canvas->mapToGlobal(pressPos),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pressPos, canvas->mapToGlobal(pressPos),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    EXPECT_EQ(canvas->selectedClipGuid(), panel.clipGuid(0, 0));
    VNX::UndoRedoManager::instance().clear();
    QAction* del = nullptr;
    for (QAction* action : canvas->actions()) {
        if (action->shortcut() == QKeySequence::Delete) {
            del = action;
            break;
        }
    }
    ASSERT_NE(del, nullptr);
    del->trigger();
    EXPECT_EQ(panel.clipCount(0), 0);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(TimelineEditorPanelTest, DeleteShortcutPreservesHiddenMarker)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(piece_);
    panel.show();
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Custom, 0, QStringLiteral("Hit")));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    ASSERT_EQ(panel.selectedMarkerGuid(), panel.markerGuid(0));
    VNX::UndoRedoManager::instance().clear();
    QAction* del = nullptr;
    for (QAction* action : canvas->actions()) {
        if (action->shortcut() == QKeySequence::Delete) {
            del = action;
            break;
        }
    }
    ASSERT_NE(del, nullptr);
    del->trigger();
    EXPECT_EQ(panel.markerCount(), 1);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(TimelineEditorPanelTest, RulerDoubleClickDoesNotRenameHiddenMarkers)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(piece_);
    panel.show();
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Custom, 0, QStringLiteral("Hit")));
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Entry, 1000000000LL));
    drainEvents();
    auto* ruler = panel.findChild<VNX::TimelineRuler*>(QStringLiteral("timelineRuler"));
    ASSERT_NE(ruler, nullptr);
    VNX::UndoRedoManager::instance().clear();
    const auto markers = piece_->comp<VNX::VPiece>()->markers.toJson();
    const QPointF customPos(panel.laneHeaderWidth() + panel.pixelAtCoordinate(0, 0)
                                - panel.horizontalScroll(), 7.0);
    QMouseEvent dbl(QEvent::MouseButtonDblClick, customPos, ruler->mapToGlobal(customPos),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ruler, &dbl);
    const QPointF entryPos(panel.laneHeaderWidth() + panel.pixelAtCoordinate(0, 1000000000LL)
                               - panel.horizontalScroll(), 7.0);
    QMouseEvent entryDbl(QEvent::MouseButtonDblClick, entryPos, ruler->mapToGlobal(entryPos),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ruler, &entryDbl);
    drainEvents();
    EXPECT_EQ(piece_->comp<VNX::VPiece>()->markers.toJson(), markers);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(TimelineEditorPanelTest, LassoSelectsClipsAcrossLanesWithModifiers)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* first = manager.createItem(
        piece_, QStringLiteral("LassoA"), VNX::ProjectItem::Type::Track);
    VNX::ProjectItem* second = manager.createItem(
        piece_, QStringLiteral("LassoB"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    first->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), ownedGuid(first), 0, 1000000000LL},
    });
    second->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), ownedGuid(second), 0, 1000000000LL},
    });

    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(piece_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    const int laneH = panel.timelineMetrics().laneHeight
        + panel.timelineMetrics().laneSpacing;
    const QPointF start(8.0, 1.0);
    const QPointF end(panel.pixelAtCoordinate(0, 2000000000LL) - panel.horizontalScroll(),
                      static_cast<qreal>(laneH * 2));
    QMouseEvent press(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent move(QEvent::MouseMove, end, canvas->mapToGlobal(end),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, canvas->mapToGlobal(end),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    EXPECT_EQ(panel.selectedClipGuids().size(), 2);

    const QUuid keep = panel.clipGuid(1, 0);
    const QPointF altEnd(end.x(), static_cast<qreal>(panel.timelineMetrics().laneHeight - 4));
    QMouseEvent altPress(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start),
                         Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &altPress);
    QMouseEvent altMove(QEvent::MouseMove, altEnd, canvas->mapToGlobal(altEnd),
                        Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &altMove);
    QMouseEvent altRelease(QEvent::MouseButtonRelease, altEnd, canvas->mapToGlobal(altEnd),
                           Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &altRelease);
    EXPECT_EQ(panel.selectedClipGuids().size(), 1);
    EXPECT_TRUE(panel.selectedClipGuids().contains(keep));
}

TEST_F(TimelineEditorPanelTest, MultiSelectMoveAndClipEdgesKeepLength)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* first = manager.createItem(
        piece_, QStringLiteral("BatchA"), VNX::ProjectItem::Type::Track);
    VNX::ProjectItem* second = manager.createItem(
        piece_, QStringLiteral("BatchB"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    VNX::ProjectItem* firstSound = manager.createItem(
        first, QStringLiteral("BatchASound"), VNX::ProjectItem::Type::Sound);
    VNX::ProjectItem* secondSound = manager.createItem(
        second, QStringLiteral("BatchBSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(firstSound, nullptr);
    ASSERT_NE(secondSound, nullptr);
    first->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), firstSound->guid, 0, 1000000000LL},
        {QUuid::createUuid(), firstSound->guid, 2000000000LL, 1000000000LL},
    });
    second->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), secondSound->guid, 0, 1000000000LL},
    });

    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(piece_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    const int laneH = panel.timelineMetrics().laneHeight
        + panel.timelineMetrics().laneSpacing;
    const QPointF start(8.0, 1.0);
    const QPointF end(panel.pixelAtCoordinate(0, 4000000000LL) - panel.horizontalScroll(),
                      static_cast<qreal>(laneH * 2));
    QMouseEvent press(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent move(QEvent::MouseMove, end, canvas->mapToGlobal(end),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, canvas->mapToGlobal(end),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    ASSERT_EQ(panel.selectedClipGuids().size(), 3);
    const QUuid a = panel.clipGuid(0, 0);
    const QUuid b = panel.clipGuid(0, 1);
    const QUuid c = panel.clipGuid(1, 0);
    VNX::UndoRedoManager::instance().clear();

    const QPointF dragFrom(panel.pixelAtCoordinate(0, 500000000LL) - panel.horizontalScroll(), 24.0);
    const QPointF dragTo(panel.pixelAtCoordinate(0, 1500000000LL) - panel.horizontalScroll(), 24.0);
    QMouseEvent dragPress(QEvent::MouseButtonPress, dragFrom, canvas->mapToGlobal(dragFrom),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &dragPress);
    QMouseEvent dragMove(QEvent::MouseMove, dragTo, canvas->mapToGlobal(dragTo),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &dragMove);
    EXPECT_EQ(panel.clipStart(0, 0), 0);
    QMouseEvent dragRelease(QEvent::MouseButtonRelease, dragTo, canvas->mapToGlobal(dragTo),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &dragRelease);
    EXPECT_EQ(panel.clipStart(0, panel.findClipIndex(0, a)), 1000000000LL);
    EXPECT_EQ(panel.clipStart(0, panel.findClipIndex(0, b)), 3000000000LL);
    EXPECT_EQ(panel.clipStart(1, panel.findClipIndex(1, c)), 1000000000LL);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(panel.clipStart(0, panel.findClipIndex(0, a)), 0);
    EXPECT_EQ(panel.clipStart(0, panel.findClipIndex(0, b)), 2000000000LL);
    EXPECT_EQ(panel.clipStart(1, panel.findClipIndex(1, c)), 0);

    canvas->setSelectedClip(0, a);
    QMouseEvent ctrl(QEvent::MouseButtonPress,
                     QPointF(panel.pixelAtCoordinate(0, 2500000000LL) - panel.horizontalScroll(), 24.0),
                     canvas->mapToGlobal(QPointF(panel.pixelAtCoordinate(0, 2500000000LL)
                                                 - panel.horizontalScroll(), 24.0)),
                     Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(canvas, &ctrl);
    QMouseEvent ctrlRelease(QEvent::MouseButtonRelease,
                            QPointF(panel.pixelAtCoordinate(0, 2500000000LL) - panel.horizontalScroll(), 24.0),
                            canvas->mapToGlobal(QPointF(panel.pixelAtCoordinate(0, 2500000000LL)
                                                        - panel.horizontalScroll(), 24.0)),
                            Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
    QApplication::sendEvent(canvas, &ctrlRelease);
    ASSERT_EQ(panel.selectedClipGuids().size(), 2);
    VNX::UndoRedoManager::instance().clear();
    canvas->repaint();
    const int handleWidth = panel.timelineMetrics().resizeHandleWidth;
    const int rightPx = panel.pixelAtCoordinate(0, 1000000000LL) - panel.horizontalScroll();
    const QPointF handle(rightPx - handleWidth / 2, 24.0);
    const QPointF handleTo(panel.pixelAtCoordinate(0, 2000000000LL) - panel.horizontalScroll(), 24.0);
    QMouseEvent resizePress(QEvent::MouseButtonPress, handle, canvas->mapToGlobal(handle),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &resizePress);
    QMouseEvent resizeMove(QEvent::MouseMove, handleTo, canvas->mapToGlobal(handleTo),
                           Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &resizeMove);
    QMouseEvent resizeRelease(QEvent::MouseButtonRelease, handleTo, canvas->mapToGlobal(handleTo),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &resizeRelease);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_EQ(panel.clipDuration(0, panel.findClipIndex(0, a)), 1000000000LL);
    EXPECT_EQ(panel.clipDuration(0, panel.findClipIndex(0, b)), 1000000000LL);
}

TEST_F(TimelineEditorPanelTest,
       SingleClipRightEdgeResizesWithSnapAltAndUndo)
{
    VNX::AssetInfo asset;
    asset.sourceID = 9201;
    asset.relativePath = QStringLiteral("resize-preview.wav");
    asset.absolutePath = tempDir_.path() + QStringLiteral("/resize-preview.wav");
    asset.metadata.sampleRate = 48000;
    asset.metadata.totalSamples = 480000;
    asset.metadata.durationSec = 10.0;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    VNX::SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    source.trimEndFrame = 480000;
    auto* sound = sound_->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    sound->sources.plain.entries = {source};
    sound->sources.plain.active = source.entryId;
    sound_->sourceID = asset.sourceID;

    const QUuid guid = QUuid::createUuid();
    track_->comp<VNX::VTrack>()->clips = makeClipLedger({
        {guid, sound_->guid, 0, 1000000000LL},
    });
    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(
        QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->clear();

    const auto dragEnd = [&](int64_t to,
                             Qt::KeyboardModifiers modifiers) {
        canvas->repaint();
        const auto& renderData = canvas->lastRenderData();
        EXPECT_EQ(1, renderData.clipItems.size());
        if (renderData.clipItems.size() != 1)
            return int64_t{0};
        const qreal fromX = renderData.clipItems.front().rect.right();
        const int toX = panel.viewportPixelAtCoordinate(0, to);
        const int64_t originalStart = panel.clipStart(0, 0);
        const int64_t originalDuration = panel.clipDuration(0, 0);
        const int64_t pressCoordinate =
            panel.coordinateAtViewportPixel(0, fromX);
        const int64_t currentCoordinate =
            panel.coordinateAtViewportPixel(0, toX);
        const int64_t rawDuration = std::max<int64_t>(
            1, originalDuration + currentCoordinate - pressCoordinate);
        const QPointF fromPoint(fromX, 24.0);
        const QPointF toPoint(toX, 24.0);
        QMouseEvent hover(QEvent::MouseMove, fromPoint,
                          canvas->mapToGlobal(fromPoint), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &hover);
        QMouseEvent press(QEvent::MouseButtonPress, fromPoint,
                          canvas->mapToGlobal(fromPoint), Qt::LeftButton,
                          Qt::LeftButton, modifiers);
        QApplication::sendEvent(canvas, &press);
        QMouseEvent move(QEvent::MouseMove, toPoint,
                         canvas->mapToGlobal(toPoint), Qt::NoButton,
                         Qt::LeftButton, modifiers);
        QApplication::sendEvent(canvas, &move);
        canvas->repaint();
        const auto& previewItems = canvas->lastRenderData().clipItems;
        EXPECT_EQ(1, previewItems.size());
        if (previewItems.size() == 1 &&
            canvas->previewDuration() > originalDuration) {
            const auto& preview = previewItems.back();
            const double expectedFrames = 480000.0 *
                static_cast<double>(canvas->previewDuration()) /
                10000000000.0;
            EXPECT_NEAR(expectedFrames, preview.waveformFrameCount, 1e-6);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, toPoint,
                            canvas->mapToGlobal(toPoint), Qt::LeftButton,
                            Qt::NoButton, modifiers);
        QApplication::sendEvent(canvas, &release);
        EXPECT_EQ(originalStart, panel.clipStart(0, 0));
        return rawDuration;
    };

    constexpr int64_t rawEnd = 1400000000LL;
    const int64_t snappedRawDuration =
        dragEnd(rawEnd, Qt::NoModifier);
    bool snapOk = false;
    const int64_t snappedEnd = panel.snapCoordinate(
        0, snappedRawDuration, false, &snapOk, {guid});
    ASSERT_TRUE(snapOk);
    EXPECT_EQ(0, panel.clipStart(0, 0));
    EXPECT_EQ(snappedEnd, panel.clipDuration(0, 0));
    ASSERT_EQ(1, undo->count());
    undo->undo();
    EXPECT_EQ(1000000000LL, panel.clipDuration(0, 0));

    const int64_t rawDuration = dragEnd(rawEnd, Qt::AltModifier);
    EXPECT_EQ(rawDuration, panel.clipDuration(0, 0));
    ASSERT_EQ(1, undo->count());
    undo->undo();
    EXPECT_EQ(1000000000LL, panel.clipDuration(0, 0));
    undo->redo();
    EXPECT_EQ(rawDuration, panel.clipDuration(0, 0));

    dragEnd(-1000000000LL, Qt::AltModifier);
    EXPECT_EQ(1, panel.clipDuration(0, 0));
    EXPECT_EQ(2, undo->count());
    undo->undo();
    EXPECT_EQ(rawDuration, panel.clipDuration(0, 0));
}

TEST_F(TimelineEditorPanelTest,
       MinimumWidthClipResizesFromVisibleRightEdgeWithoutPressJump)
{
    const QUuid guid = QUuid::createUuid();
    track_->comp<VNX::VTrack>()->clips = makeClipLedger({
        {guid, sound_->guid, 0, 1},
    });
    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(
        QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    canvas->repaint();
    const auto& renderData = canvas->lastRenderData();
    ASSERT_EQ(1, renderData.clipItems.size());
    const QRect rect = renderData.clipItems.front().rect;
    const QPointF from(rect.right(), 24.0);
    const QPointF to(rect.right() + 80.0, 24.0);
    const int64_t pressCoordinate =
        panel.coordinateAtViewportPixel(0, from.x());
    const int64_t currentCoordinate =
        panel.coordinateAtViewportPixel(0, to.x());
    const int64_t expectedDuration = std::max<int64_t>(
        1, 1 + currentCoordinate - pressCoordinate);
    QMouseEvent hover(QEvent::MouseMove, from, canvas->mapToGlobal(from),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &hover);
    QMouseEvent press(QEvent::MouseButtonPress, from,
                      canvas->mapToGlobal(from), Qt::LeftButton,
                      Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &press);
    EXPECT_EQ(1, panel.clipDuration(0, 0));
    QMouseEvent move(QEvent::MouseMove, to, canvas->mapToGlobal(to),
                     Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to,
                        canvas->mapToGlobal(to), Qt::LeftButton,
                        Qt::NoButton, Qt::AltModifier);
    QApplication::sendEvent(canvas, &release);
    EXPECT_EQ(expectedDuration, panel.clipDuration(0, 0));
}

TEST_P(TimelineTrackHostTest, AnchoredWheelZoomKeepsTimeCoordinateAcrossControls)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    const auto ruler = panel.findChild<VNX::TimelineRuler*>("timelineRuler");
    const auto scroll = panel.findChild<QScrollBar*>("timelineHorizontalScroll");
    const auto zoomIn = panel.findChild<QToolButton*>("timelineZoomIn");
    const auto zoomOut = panel.findChild<QToolButton*>("timelineZoomOut");
    for (auto* control : QList<QWidget*>{canvas, ruler, scroll, zoomIn, zoomOut}) {
        ASSERT_NE(control, nullptr);
        SCOPED_TRACE(control->objectName().toStdString());
        const QPoint point = control->rect().center();
        const QPoint global = control->mapToGlobal(point);
        const int anchor = std::clamp(canvas->mapFromGlobal(global).x(), 0, canvas->width() - 1);
        const int64_t before = panel.coordinateAtViewportPixel(0, anchor);
        const double zoom = panel.zoomPercent();
        QWheelEvent wheel(point, global, QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
            Qt::NoScrollPhase, false);
        QApplication::sendEvent(control, &wheel);
        EXPECT_TRUE(wheel.isAccepted());
        EXPECT_GT(panel.zoomPercent(), zoom);
        EXPECT_NEAR(static_cast<double>(panel.coordinateAtViewportPixel(0, anchor)),
            static_cast<double>(before), 1.0);
    }
}

TEST_F(TimelineEditorPanelTest, VerticalClipDragStaysOnOwningTrackAndCommitsTimeOnRelease)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* first = manager.createItem(
        piece_, QStringLiteral("DragA"), VNX::ProjectItem::Type::Track);
    VNX::ProjectItem* second = manager.createItem(
        piece_, QStringLiteral("DragB"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    VNX::ProjectItem* owned = manager.createItem(
        first, QStringLiteral("DragSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(owned, nullptr);
    first->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), owned->guid, 0, 1000000000LL},
    });
    VNX::UndoRedoManager::instance().clear();
    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(piece_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    VNX::UndoRedoManager::instance().clear();
    const int laneH = panel.timelineMetrics().laneHeight
        + panel.timelineMetrics().laneSpacing;
    const QPointF from(panel.pixelAtCoordinate(0, 500000000LL) - panel.horizontalScroll(), 24.0);
    const QPointF to(panel.pixelAtCoordinate(0, 1500000000LL) - panel.horizontalScroll(),
                     static_cast<qreal>(laneH + 24));
    QMouseEvent press(QEvent::MouseButtonPress, from, canvas->mapToGlobal(from),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent move(QEvent::MouseMove, to, canvas->mapToGlobal(to),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    EXPECT_EQ(panel.clipCount(0), 1);
    EXPECT_EQ(panel.clipCount(1), 0);
    EXPECT_EQ(panel.clipStart(0, 0), 0);
    EXPECT_TRUE(canvas->isGestureActive());
    EXPECT_EQ(canvas->previewLane(), 0);
    QMouseEvent release(QEvent::MouseButtonRelease, to, canvas->mapToGlobal(to),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    EXPECT_EQ(panel.clipCount(0), 1);
    EXPECT_EQ(panel.clipCount(1), 0);
    EXPECT_EQ(panel.clipStart(0, 0), 1000000000LL);
    EXPECT_EQ(owned->parent, first);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(panel.clipStart(0, 0), 0);
}

TEST_F(TimelineEditorPanelTest, EscapeCancelsInFlightClipDrag)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(900, 360);
    panel.bindItem(track_);
    panel.show();
    ASSERT_TRUE(placeOnLane(panel, sound_, 0, 0, true));
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    VNX::UndoRedoManager::instance().clear();
    const QPointF from(panel.pixelAtCoordinate(0, 500000000LL) - panel.horizontalScroll(), 24.0);
    const QPointF to(panel.pixelAtCoordinate(0, 2500000000LL) - panel.horizontalScroll(), 24.0);
    QMouseEvent press(QEvent::MouseButtonPress, from, canvas->mapToGlobal(from),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    QMouseEvent move(QEvent::MouseMove, to, canvas->mapToGlobal(to),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &move);
    EXPECT_TRUE(canvas->isGestureActive());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escape);
    EXPECT_FALSE(canvas->isGestureActive());
    EXPECT_EQ(panel.clipStart(0, 0), 0);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(TimelineEditorPanelTest, TrackInLegacyMusicalPieceRestoresReadOnlyWithoutTicks)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* child = manager.createItem(
        piece_, QStringLiteral("MusicalChild"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(child, nullptr);
    auto* timeline = piece_->comp<VNX::VPiece>();
    timeline->pieceTime = VNX::PieceTimeSpec::musical();
    VNX::TimelineEditorPanel panel;
    panel.bindItem(child);
    EXPECT_EQ(panel.viewKind(), VNX::TimelineEditorPanel::ViewKind::Unsupported);
    EXPECT_TRUE(VNX::buildTimelineTicks(timeline->pieceTime, 0, 0, 100, 800,
                                      panel.timelineMetrics()).ticks.isEmpty());
    VNX::TimelineEditorPanel restored;
    restored.restoreState(panel.snapshotState());
    EXPECT_EQ(restored.boundItem(), piece_);
    EXPECT_EQ(restored.viewKind(), VNX::TimelineEditorPanel::ViewKind::Unsupported);
    EXPECT_EQ(restored.formatCoordinate(0, 960), QStringLiteral("Unsupported arrangement"));
    EXPECT_EQ(timeline->pieceTime, VNX::PieceTimeSpec::musical());
}

TEST_F(TimelineEditorPanelTest, HeaderResizeAndCursorDragRemainIndependent)
{
    ASSERT_TRUE(VNX::ProjectManager::instance().moveItem(track_, piece_));
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.resize(1400, 420);
    panel.bindItem(piece_);
    panel.show();
    drainEvents();
    const int defaultWidth = panel.laneHeaderWidth();
    panel.setLaneHeaderWidth(360);
    drainEvents();
    ASSERT_EQ(panel.laneHeaderWidth(), 360);

    auto* ruler = panel.findChild<VNX::TimelineRuler*>(QStringLiteral("timelineRuler"));
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    auto* resizeBar = panel.findChild<QFrame*>(QStringLiteral("timelineHeaderResizeBar"));
    auto* divider = panel.findChild<QFrame*>(QStringLiteral("timelineHeaderDivider"));
    auto* page = panel.findChild<QFrame*>(QStringLiteral("timelineViewportPage"));
    ASSERT_NE(ruler, nullptr);
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(resizeBar, nullptr);
    ASSERT_NE(divider, nullptr);
    ASSERT_NE(page, nullptr);
    EXPECT_TRUE(resizeBar->isVisible());
    EXPECT_GT(resizeBar->width(), 1);
    EXPECT_EQ(resizeBar->geometry().right() + 1, divider->geometry().left());
    EXPECT_EQ(resizeBar->geometry().top(), page->contentsRect().top());
    EXPECT_EQ(resizeBar->geometry().bottom(), page->contentsRect().bottom());
    const int64_t initialCursor = panel.editCursor();
    const auto mouse = [&](QWidget* target, QEvent::Type type, QPoint global) {
        QMouseEvent event(type, target->mapFromGlobal(global), global,
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
            Qt::AltModifier);
        QApplication::sendEvent(target, &event);
        drainEvents();
    };
    const QRect resizeArea(ruler->mapFromGlobal(resizeBar->mapToGlobal(QPoint())), resizeBar->size());
    EXPECT_LT(resizeArea.right(), ruler->timeAxisRect().left());
    auto* addTrack = panel.findChild<QToolButton*>("timelineAddTrackButton");
    ASSERT_NE(addTrack, nullptr);
    EXPECT_FALSE(addTrack->geometry().intersects(resizeArea));
    for (auto* name : panel.findChildren<QLabel*>(QStringLiteral("timelineLaneName")))
        EXPECT_LT(name->mapTo(page, name->rect().topRight()).x(), resizeBar->geometry().left());
    for (int y : {3, resizeBar->height() / 2, resizeBar->height() - 3}) {
        panel.setLaneHeaderWidth(360);
        drainEvents();
        const QPoint resizeFrom = resizeBar->mapToGlobal(QPoint(resizeBar->width() / 2, y));
        EXPECT_EQ(panel.childAt(panel.mapFromGlobal(resizeFrom)), resizeBar);
        mouse(resizeBar, QEvent::MouseButtonPress, resizeFrom);
        mouse(resizeBar, QEvent::MouseMove, resizeFrom + QPoint(30, 0));
        mouse(resizeBar, QEvent::MouseMove, resizeFrom + QPoint(60, 0));
        mouse(resizeBar, QEvent::MouseButtonRelease, resizeFrom + QPoint(60, 0));
        EXPECT_EQ(panel.laneHeaderWidth(), 420);
        EXPECT_EQ(panel.editCursor(), initialCursor);
        EXPECT_FALSE(panel.isCursorDragActive());
    }

    const int64_t expected = panel.coordinateAtViewportPixel(0, 80);
    for (int y : {ruler->cursorHandleRect().center().y(), ruler->timeAxisRect().bottom() - 2}) {
        panel.setEditCursor(0, true);
        mouse(ruler, QEvent::MouseButtonPress, ruler->mapToGlobal(QPoint(420, y)));
        EXPECT_TRUE(panel.isCursorDragActive());
        mouse(ruler, QEvent::MouseMove, ruler->mapToGlobal(QPoint(500, y)));
        mouse(ruler, QEvent::MouseButtonRelease, ruler->mapToGlobal(QPoint(500, y)));
        EXPECT_EQ(panel.editCursor(), expected);
        EXPECT_EQ(panel.laneHeaderWidth(), 420);
    }

    panel.setEditCursor(0, true);
    const QPoint cursorFrom = canvas->mapToGlobal(QPoint(0, canvas->height() / 2));
    EXPECT_EQ(panel.childAt(panel.mapFromGlobal(cursorFrom)), canvas);
    mouse(canvas, QEvent::MouseButtonPress, cursorFrom);
    EXPECT_TRUE(panel.isCursorDragActive());
    mouse(canvas, QEvent::MouseMove, cursorFrom + QPoint(80, 0));
    mouse(canvas, QEvent::MouseButtonRelease, cursorFrom + QPoint(80, 0));
    EXPECT_EQ(panel.editCursor(), expected);
    EXPECT_EQ(panel.laneHeaderWidth(), 420);

    mouse(resizeBar, QEvent::MouseButtonDblClick, resizeBar->mapToGlobal(resizeBar->rect().center()));
    EXPECT_EQ(panel.laneHeaderWidth(), defaultWidth);
    EXPECT_EQ(panel.editCursor(), expected);
}

TEST_P(TimelineTrackHostTest, RulerAndCanvasKeepContinuousCursorAndSurfaceEdges)
{
    VNX::TimelineEditorPanel panel;
    panel.setEditCursorVisible(true);
    panel.setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    panel.resize(900, 360);
    panel.bindItem(piece_);
    panel.show();
    drainEvents();
    auto* ruler = panel.findChild<VNX::TimelineRuler*>(QStringLiteral("timelineRuler"));
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    auto* header = panel.findChild<VNX::TimelineHeaderColumn*>();
    ASSERT_NE(ruler, nullptr);
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(header, nullptr);
    const auto render = [&] {
        drainEvents();
        const qreal scale = panel.devicePixelRatioF();
        QImage image(panel.size() * scale, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(scale);
        image.fill(Qt::transparent);
        panel.render(&image);
        return image;
    };
    const auto pixel = [](const QImage& image, const QPoint& point) {
        return image.pixelColor(point * image.devicePixelRatio());
    };
    const QImage empty = render();
    const QColor headerBackground = pixel(empty, header->mapTo(&panel, header->rect().center()));
    for (int x : {1, header->width() / 2, header->width() - panel.laneHeaderResizeWidth() - 3}) {
        for (int y : {1, header->height() / 2, header->height() - 2})
            EXPECT_EQ(pixel(empty, header->mapTo(&panel, QPoint(x, y))), headerBackground);
    }
    const auto* resizeBar = panel.findChild<QFrame*>("timelineHeaderResizeBar");
    ASSERT_NE(resizeBar, nullptr);
    for (int y : {2, ruler->height() - 1, resizeBar->height() / 2, resizeBar->height() - 3}) {
        const QPoint point = resizeBar->mapTo(&panel, QPoint(resizeBar->width() / 2, y));
        EXPECT_EQ(pixel(empty, point), pixel(empty, point - QPoint(3, 0)));
    }

    panel.bindItem(track_);
    panel.setEditCursor(3500000000LL, true);
    const auto checkSeams = [&] {
        const QImage image = render();
        const int x = panel.viewportPixelAtCoordinate(0, panel.editCursor());
        const QPoint rulerEnd = ruler->mapTo(&panel,
            QPoint(panel.laneHeaderWidth() + x, ruler->height() - 1));
        const QPoint canvasStart = canvas->mapTo(&panel, QPoint(x, 0));
        EXPECT_EQ(rulerEnd.x(), canvasStart.x());
        EXPECT_EQ(rulerEnd.y() + 1, canvasStart.y());
        const auto cursorPixels = [&](const QPoint& point) {
            std::vector<int> pixels;
            const qreal scale = image.devicePixelRatio();
            const QColor color = VNX::ThemeManager::instance().colors().success;
            for (int column = int(std::floor((point.x() - 3) * scale));
                 column <= int(std::ceil((point.x() + 3) * scale)); ++column) {
                if (image.pixelColor(column, int(std::floor(point.y() * scale))) == color)
                    pixels.push_back(column);
            }
            return pixels;
        };
        const auto rulerPixels = cursorPixels(rulerEnd);
        EXPECT_FALSE(rulerPixels.empty());
        for (int y : {0, canvas->height() / 2, canvas->height() - 1})
            EXPECT_EQ(cursorPixels(canvas->mapTo(&panel, QPoint(x, y))), rulerPixels);
        for (const auto& lane : canvas->lastRenderData().laneRects) {
            EXPECT_EQ(cursorPixels(canvas->mapTo(&panel, QPoint(x, lane.rect.bottom()))), rulerPixels);
            if (panel.viewKind() == VNX::TimelineEditorPanel::ViewKind::Piece) {
                for (int laneX : {canvas->width() / 3, canvas->width() - 2}) {
                    const QPoint bottom = canvas->mapTo(&panel, QPoint(laneX, lane.rect.bottom()));
                    EXPECT_EQ(pixel(image, bottom), pixel(image, bottom - QPoint(0, 1)));
                }
            }
        }
        for (int edgeX : {canvas->width() / 3, canvas->width() - 2}) {
            EXPECT_EQ(pixel(image, canvas->mapTo(&panel, QPoint(edgeX, canvas->height() - 1))),
                pixel(image, canvas->mapTo(&panel, QPoint(edgeX, canvas->height() - 2))));
        }
        const auto* controls = panel.findChild<QFrame*>("timelineScrollControls");
        ASSERT_NE(controls, nullptr);
        EXPECT_EQ(resizeBar->isVisible(), panel.viewKind() == VNX::TimelineEditorPanel::ViewKind::Piece);
        if (panel.viewKind() == VNX::TimelineEditorPanel::ViewKind::Track) {
            const auto* corner = panel.findChild<QFrame*>("timelineScrollCorner");
            const auto* vertical = panel.findChild<QFrame*>("timelineVerticalTools");
            ASSERT_NE(corner, nullptr);
            ASSERT_NE(vertical, nullptr);
            EXPECT_TRUE(corner->isVisible());
            EXPECT_EQ(controls->mapTo(&panel, controls->rect().topRight()).x(),
                canvas->mapTo(&panel, canvas->rect().bottomRight()).x());
            EXPECT_EQ(corner->mapTo(&panel, QPoint()).x(), vertical->mapTo(&panel, QPoint()).x());
            EXPECT_EQ(corner->width(), vertical->width());
        }
        EXPECT_EQ(controls->mapTo(&panel, QPoint()).y(),
            canvas->mapTo(&panel, QPoint(0, canvas->height())).y());
        const QColor border = pixel(image, controls->mapTo(&panel, QPoint(controls->width() / 2, 0)));
        EXPECT_NE(border, VNX::ThemeManager::instance().colors().bgPrimary);
        for (int borderX : {1, controls->width() / 2, controls->width() - 2})
            EXPECT_EQ(pixel(image, controls->mapTo(&panel, QPoint(borderX, 0))), border);
    };
    checkSeams();
    const QPoint hover = ruler->cursorHandleRect().center();
    QMouseEvent move(QEvent::MouseMove, hover, ruler->mapToGlobal(hover),
        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(ruler, &move);
    checkSeams();
    panel.beginCursorDrag(panel.viewportPixelAtCoordinate(0, panel.editCursor()), true, false);
    panel.updateCursorDrag(canvas->width() / 2, true);
    checkSeams();
    panel.endCursorDrag();
    panel.setLaneHeaderWidth(320);
    panel.resize(1100, 420);
    panel.setZoomPercent(200);
    panel.panHorizontally(80);
    panel.setEditCursor(panel.coordinateAtViewportPixel(0, canvas->width() / 2), true);
    checkSeams();
}

TEST_F(TimelineEditorPanelTest, BatchCommitFailureRollsBackWithoutUndoStep)
{
    VNX::TimelineEditorPanel panel;
    panel.bindItem(track_);
    ASSERT_TRUE(placeOnLane(panel, sound_, 0, 0, true));
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
    EXPECT_EQ(panel.lastInteractionError(), QStringLiteral("forced batch failure"));
}

TEST_F(TimelineEditorPanelTest, VerticalGroupDragKeepsAllClipsOnTheirOwningTracks)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* first = manager.createItem(
        piece_, QStringLiteral("BoundA"), VNX::ProjectItem::Type::Track);
    VNX::ProjectItem* second = manager.createItem(
        piece_, QStringLiteral("BoundB"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    first->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), ownedGuid(first), 0, 1000000000LL},
    });
    second->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), ownedGuid(second), 0, 1000000000LL},
    });

    VNX::TimelineEditorPanel panel;
    panel.resize(1400, 520);
    panel.bindItem(piece_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    const int laneH = panel.timelineMetrics().laneHeight
        + panel.timelineMetrics().laneSpacing;
    const QPointF start(8.0, 1.0);
    const QPointF end(panel.pixelAtCoordinate(0, 2000000000LL) - panel.horizontalScroll(),
                      static_cast<qreal>(laneH + 8));
    QMouseEvent lassoPress(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &lassoPress);
    QMouseEvent lassoMove(QEvent::MouseMove, end, canvas->mapToGlobal(end),
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &lassoMove);
    QMouseEvent lassoRelease(QEvent::MouseButtonRelease, end, canvas->mapToGlobal(end),
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &lassoRelease);
    ASSERT_EQ(panel.selectedClipGuids().size(), 2);
    const QUuid top = panel.clipGuid(0, 0);
    const QUuid bottom = panel.clipGuid(1, 0);
    VNX::UndoRedoManager::instance().clear();

    const QPointF dragFrom(panel.pixelAtCoordinate(0, 200000000LL) - panel.horizontalScroll(), 20.0);
    const QPointF dragTo(panel.pixelAtCoordinate(0, 200000000LL) - panel.horizontalScroll(),
                         static_cast<qreal>(laneH + 20));
    QMouseEvent dragPress(QEvent::MouseButtonPress, dragFrom, canvas->mapToGlobal(dragFrom),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &dragPress);
    QMouseEvent dragMove(QEvent::MouseMove, dragTo, canvas->mapToGlobal(dragTo),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &dragMove);
    drainEvents();
    bool sawIllegalGhost = false;
    for (const VNX::TimelineGhost& ghost : canvas->lastRenderData().ghosts)
        sawIllegalGhost = sawIllegalGhost || ghost.illegal;
    EXPECT_FALSE(sawIllegalGhost);
    QMouseEvent dragRelease(QEvent::MouseButtonRelease, dragTo, canvas->mapToGlobal(dragTo),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &dragRelease);
    EXPECT_EQ(panel.findLaneOfClip(top), 0);
    EXPECT_EQ(panel.findLaneOfClip(bottom), 1);
    EXPECT_EQ(panel.clipCount(0), 1);
    EXPECT_EQ(panel.clipCount(1), 1);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_TRUE(panel.lastInteractionError().isEmpty());
    EXPECT_EQ(panel.clipStart(0, 0), 0);
    EXPECT_EQ(panel.clipStart(1, 0), 0);
}

TEST_F(TimelineEditorPanelTest, ClipDragIgnoresHiddenMarkerAndKeepsVisibleBoundarySnap)
{
    auto* track = VNX::ProjectManager::instance().createItem(
        piece_, QStringLiteral("MarkerSnapTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(piece_);
    ASSERT_TRUE(placeOnLane(panel, sound_, 0, 0, true));
    const QUuid movingGuid = panel.clipGuid(0, 0);
    ASSERT_TRUE(placeOnLane(panel, sound_, 0, 2400000000LL, true));
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Custom, 1000000000LL, QStringLiteral("Hit")));
    ASSERT_TRUE(panel.moveMarker(panel.markerGuid(0), 1370000000LL, true));
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);

    auto dragTo = [&](int64_t destination) {
        const int index = panel.findClipIndex(0, movingGuid);
        const QPointF pressPos(panel.pixelAtCoordinate(0, panel.clipStart(0, index))
                                   - panel.horizontalScroll() + 8, 20);
        const QPointF movePos(panel.pixelAtCoordinate(0, destination)
                                  - panel.horizontalScroll() + 8, 20);
        QMouseEvent press(QEvent::MouseButtonPress, pressPos, canvas->mapToGlobal(pressPos),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &press);
        QMouseEvent move(QEvent::MouseMove, movePos, canvas->mapToGlobal(movePos),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &move);
        const int64_t preview = canvas->previewStart();
        QMouseEvent release(QEvent::MouseButtonRelease, movePos, canvas->mapToGlobal(movePos),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &release);
        EXPECT_EQ(panel.clipStart(0, panel.findClipIndex(0, movingGuid)), preview);
        return preview;
    };
    EXPECT_EQ(dragTo(1370000000LL), 1400000000LL);
    EXPECT_EQ(panel.markerPosition(0), 1370000000LL);
    EXPECT_EQ(dragTo(2400000000LL), 2400000000LL);
}

TEST_F(TimelineEditorPanelTest, HiddenCustomMarkersDoNotExpandViewportRange)
{
    VNX::TimelineEditorPanel panel;
    panel.resize(1200, 420);
    panel.bindItem(piece_);
    panel.show();
    drainEvents();
    auto* scroll = panel.findChild<QScrollBar*>(QStringLiteral("timelineHorizontalScroll"));
    ASSERT_NE(scroll, nullptr);
    const int maximum = scroll->maximum();
    ASSERT_EQ(panel.viewOriginCoordinate(), 0);
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Custom, -100000000000LL,
                            QStringLiteral("Before")));
    ASSERT_TRUE(panel.addMarker(VNX::MarkerKind::Custom, 100000000000LL,
                            QStringLiteral("After")));
    drainEvents();
    EXPECT_EQ(panel.markerCount(), 2);
    EXPECT_EQ(panel.viewOriginCoordinate(), 0);
    EXPECT_EQ(scroll->maximum(), maximum);
}

TEST_F(TimelineEditorPanelTest, EmptyWaveformIsNotCachedForever)
{
    VNX::TimelineEditorPanel panel;
    panel.bindItem(track_);
    panel.show();
    drainEvents();
    auto* canvas = panel.findChild<VNX::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    const QString path = tempDir_.path() + QStringLiteral("/late-source.wav");
    EXPECT_EQ(canvas->cachedWaveform(path), nullptr);
    ASSERT_TRUE(writePcm16Wav(path, 480, 1, 48000));
    ASSERT_TRUE(VNX::WaveformCache::generate(path, 64));
    const VNX::WaveformPeakData* peaks = canvas->cachedWaveform(path);
    ASSERT_NE(peaks, nullptr);
    EXPECT_TRUE(peaks->isValid());
    EXPECT_GT(peaks->numPeaks, 0u);
}

TEST_P(TimelineTrackHostTest, LegacyReferencedPieceBlocksOnlyItsAffectedTimeline)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* nested = manager.createItem(track_, QStringLiteral("LegacyPiece"),
                                      VNX::ProjectItem::Type::Piece);
    ASSERT_NE(nested, nullptr);
    track_->comp<VNX::VTrack>()->clips.items.clear();
    VNX::TimelineEditorPanel panel;
    panel.bindItem(track_);
    EXPECT_EQ(panel.viewKind(), (GetParam() ? VNX::TimelineEditorPanel::ViewKind::Piece : VNX::TimelineEditorPanel::ViewKind::Track));
    EXPECT_TRUE(panel.placeItem(sound_, 0, 80000000, true));
    EXPECT_EQ(panel.clipStart(0, 0), 80000000);
    nested->comp<VNX::VPiece>()->pieceTime = VNX::PieceTimeSpec::musical();
    EXPECT_FALSE(panel.placeItem(nested, 0, 0, true));
    track_->comp<VNX::VTrack>()->clips.items.front().targetGuid = nested->guid;
    emit manager.itemPropertyChanged(track_, QStringLiteral("clips"));
    drainEvents();
    EXPECT_EQ(panel.viewKind(), VNX::TimelineEditorPanel::ViewKind::Unsupported);
    panel.bindItem(piece_);
    EXPECT_EQ(panel.viewKind(), VNX::TimelineEditorPanel::ViewKind::Piece);
    auto* independentTrack = manager.createItem(
        piece_, QStringLiteral("IndependentTrack"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(independentTrack, nullptr);
    auto* independentSound = manager.createItem(
        independentTrack, QStringLiteral("IndependentSound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(independentSound, nullptr);
    EXPECT_TRUE(panel.moveClip(0, panel.clipGuid(0, 0), 0, 240000000, true));
    EXPECT_EQ(panel.clipStart(0, 0), 240000000);
}

TEST_F(TimelineEditorPanelTest, TrackOrderSelectionAndReferencesRoundtripThroughUndoAndReload)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::registerContainerActions(VNX::ActionManager::instance(), &manager);
    auto* first = manager.createItem(piece_, QStringLiteral("Same"), VNX::ProjectItem::Type::Track);
    auto* second = manager.createItem(piece_, QStringLiteral("Same"), VNX::ProjectItem::Type::Track);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const QUuid firstGuid = first->guid;
    const QUuid secondGuid = second->guid;
    const QUuid pieceGuid = piece_->guid;
    VNX::TimelineEditorPanel panel;
    panel.setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    panel.resize(900, 400);
    panel.bindItem(piece_);
    panel.show();
    drainEvents();
    ASSERT_TRUE(placeOnLane(panel, sound_, 0, 123456789LL, true));
    const auto clips = first->comp<VNX::VTrack>()->clips;
    drainEvents();
    panel.setLaneHeight(0, 96);
    drainEvents();
    const int firstHeight = panel.laneRect(0).height();
    const int secondHeight = panel.laneRect(1).height();
    ASSERT_NE(firstHeight, secondHeight);
    auto* table = panel.findChild<VNX::TableWidget*>();
    ASSERT_NE(table, nullptr);
    const auto numbers = table->findChildren<QLabel*>("tableRowNumber");
    ASSERT_EQ(numbers.size(), 2);
    QMouseEvent press(QEvent::MouseButtonPress, numbers[0]->rect().center(),
        numbers[0]->mapToGlobal(numbers[0]->rect().center()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(numbers[0], &press);
    QMouseEvent release(QEvent::MouseButtonRelease, numbers[0]->rect().center(),
        numbers[0]->mapToGlobal(numbers[0]->rect().center()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(numbers[0], &release);
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), firstGuid);
    EXPECT_TRUE(numbers[0]->property("selected").toBool());
    EXPECT_FALSE(numbers[1]->property("selected").toBool());
    VNX::UndoRedoManager::instance().clear();
    QKeyEvent moveDown(QEvent::KeyPress, Qt::Key_Down, Qt::AltModifier);
    QApplication::sendEvent(numbers[0], &moveDown);
    drainEvents();
    EXPECT_TRUE(panel.lastInteractionError().isEmpty()) << panel.lastInteractionError().toStdString();
    EXPECT_EQ(piece_->children.front()->guid, secondGuid);
    EXPECT_EQ(panel.laneGuid(1), firstGuid);
    EXPECT_EQ(panel.laneRect(1).height(), firstHeight);
    EXPECT_EQ(panel.laneRect(0).height(), secondHeight);
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), firstGuid);
    EXPECT_EQ(first->comp<VNX::VTrack>()->clips, clips);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    drainEvents();
    EXPECT_EQ(panel.laneGuid(0), firstGuid);
    EXPECT_EQ(panel.laneRect(0).height(), firstHeight);
    EXPECT_EQ(panel.laneRect(1).height(), secondHeight);
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), firstGuid);
    const auto reorderedNumbers = table->findChildren<QLabel*>("tableRowNumber");
    ASSERT_EQ(reorderedNumbers.size(), 2);
    auto* handle = reorderedNumbers[0];
    const QPoint from = handle->rect().center();
    const QPoint to = handle->mapFromGlobal(reorderedNumbers[1]->mapToGlobal(
        QPoint(reorderedNumbers[1]->width() / 2, reorderedNumbers[1]->height() - 2)));
    ASSERT_TRUE(table->canMoveRow(0, 1));
    ASSERT_GE((to - from).manhattanLength(), QApplication::startDragDistance());
    ASSERT_TRUE(table->scrollArea()->viewport()->rect().contains(
        table->scrollArea()->viewport()->mapFromGlobal(handle->mapToGlobal(to))))
        << table->scrollArea()->viewport()->height() << ":" << to.y() << ":" << from.y();
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
        const QPoint position = type == QEvent::MouseButtonPress ? from : to;
        QWidget* receiver = type == QEvent::MouseButtonPress ? static_cast<QWidget*>(handle) : table;
        const QPoint global = handle->mapToGlobal(position);
        QMouseEvent event(type, receiver->mapFromGlobal(global), global,
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(receiver, &event);
        if (type == QEvent::MouseButtonPress)
            EXPECT_EQ(QApplication::focusWidget(), handle);
        if (type == QEvent::MouseMove) {
            auto* indicator = table->findChild<QFrame*>("tableRowInsertion");
            ASSERT_NE(indicator, nullptr);
            EXPECT_TRUE(indicator->isVisible());
        }
    }
    drainEvents();
    EXPECT_EQ(panel.laneGuid(1), firstGuid);
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), firstGuid);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    drainEvents();
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    drainEvents();
    EXPECT_EQ(panel.laneGuid(panel.headerSelectedLane()), firstGuid);
    EXPECT_EQ(panel.findChild<QToolButton*>("timelineLaneMute"), nullptr);
    EXPECT_EQ(panel.findChild<QToolButton*>("timelineLaneSolo"), nullptr);
    EXPECT_EQ(panel.findChild<QDoubleSpinBox*>("timelinePosition"), nullptr);
    auto* column = panel.findChild<VNX::TimelineHeaderColumn*>();
    std::unique_ptr<QMenu> menu(column->createLaneMenu(0));
    for (auto* action : menu->actions()) {
        EXPECT_NE(action->text(), QStringLiteral("Mute"));
        EXPECT_NE(action->text(), QStringLiteral("Solo"));
    }
    ASSERT_TRUE(manager.saveProject());
    const QString path = manager.currentProject()->projectPath;
    manager.closeProject();
    ASSERT_TRUE(manager.loadProject(path));
    drainEvents();
    auto* restored = manager.findItemByGuid(pieceGuid);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->children[0]->guid, secondGuid);
    EXPECT_EQ(restored->children[1]->guid, firstGuid);
    EXPECT_EQ(restored->children[1]->comp<VNX::VTrack>()->clips, clips);
    EXPECT_EQ(panel.laneRect(1).height(), firstHeight);
    EXPECT_EQ(panel.laneRect(0).height(), secondHeight);
}
