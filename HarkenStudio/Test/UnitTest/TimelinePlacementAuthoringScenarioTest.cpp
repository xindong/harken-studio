#include "ActionManager.h"
#include "Components/VMixNode.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "ProjectManager.h"
#include "Timeline/TimelineEditing.h"
#include "UndoCommands.h"
#include "Widgets/Blueprint/BlueprintEdgeKind.h"
#include "Widgets/Blueprint/SignalChainSource.h"
#include "Widgets/Timeline/TimelineEditorPanel.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <vector>

using namespace VNX;

namespace {

constexpr int64_t kSecond = 1000000000;
constexpr std::array<bool, 3> kAllBlueprintLayers{true, true, true};

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

Clip clipFor(ProjectItem* target, int64_t start, int64_t duration)
{
    return {QUuid::createUuid(), target->guid, start, duration};
}

ClipLedger makeClipLedger(std::initializer_list<Clip> clips)
{
    ClipLedger ledger;
    for (Clip clip : clips) {
        EXPECT_TRUE(ledger.mintOccurrenceOrdinal(&clip));
        ledger.items.push_back(std::move(clip));
    }
    return ledger;
}

Marker markerAt(MarkerKind kind, int64_t position, const QString& name = {})
{
    return {QUuid::createUuid(), kind, name, position};
}

const BlueprintEdgeDesc* findItemEdge(
    const std::vector<BlueprintEdgeDesc>& edges,
    uint32_t fromId, uint32_t toId)
{
    for (const BlueprintEdgeDesc& edge : edges) {
        if (edge.fromKind == BlueprintNodeKind::ProjectItem
            && edge.fromId == fromId
            && edge.toKind == BlueprintNodeKind::ProjectItem
            && edge.toId == toId) {
            return &edge;
        }
    }
    return nullptr;
}

class TimelinePlacementAuthoringScenarioTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("TimelinePlacementAuthoringScenarioProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
};

}

TEST_F(TimelinePlacementAuthoringScenarioTest,
       PieceAndBareTrackCoverDualLedgerCascadeUndoAndMarkerSemantics)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* layer = manager.createItem(
        nullptr, QStringLiteral("World"), ProjectItem::Type::Layer);
    ProjectItem* external = manager.createItem(
        layer, QStringLiteral("External"), ProjectItem::Type::Sound);
    ProjectItem* queue = manager.createItem(
        layer, QStringLiteral("Queue"), ProjectItem::Type::Container);
    ProjectItem* piece = manager.createItem(
        queue, QStringLiteral("Piece"), ProjectItem::Type::Piece);
    ProjectItem* firstTrack = manager.createItem(
        piece, QStringLiteral("FirstTrack"), ProjectItem::Type::Track);
    ProjectItem* secondTrack = manager.createItem(
        piece, QStringLiteral("SecondTrack"), ProjectItem::Type::Track);
    ProjectItem* localPieceSound = manager.createItem(
        secondTrack, QStringLiteral("LocalPieceSound"), ProjectItem::Type::Sound);
    ProjectItem* firstOwned = manager.createItem(
        firstTrack, QStringLiteral("FirstOwned"), ProjectItem::Type::Sound);
    ProjectItem* bareTrack = manager.createItem(
        layer, QStringLiteral("BareTrack"), ProjectItem::Type::Track);
    ProjectItem* ownedPlaced = manager.createItem(
        bareTrack, QStringLiteral("OwnedPlaced"), ProjectItem::Type::Sound);
    ProjectItem* ownedShelved = manager.createItem(
        bareTrack, QStringLiteral("OwnedShelved"), ProjectItem::Type::Sound);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(external, nullptr);
    ASSERT_NE(queue, nullptr);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(firstTrack, nullptr);
    ASSERT_NE(secondTrack, nullptr);
    ASSERT_NE(localPieceSound, nullptr);
    ASSERT_NE(firstOwned, nullptr);
    ASSERT_NE(bareTrack, nullptr);
    ASSERT_NE(ownedPlaced, nullptr);
    ASSERT_NE(ownedShelved, nullptr);

    const Clip firstOwnedClip = clipFor(firstOwned, -2 * kSecond, 8 * kSecond);
    const Clip localPieceClip = clipFor(localPieceSound, kSecond, 2 * kSecond);
    const Clip localBareClip = clipFor(ownedPlaced, 0, kSecond);
    const Clip secondBareClip = clipFor(ownedShelved, 2 * kSecond, kSecond);
    ClipLedger firstOriginal = makeClipLedger({firstOwnedClip});
    ClipLedger secondOriginal = makeClipLedger({localPieceClip});
    ClipLedger bareOriginal = makeClipLedger(
        {localBareClip, secondBareClip});
    ClipLedger bareSurvivor = bareOriginal;
    bareSurvivor.items.pop_back();
    ASSERT_TRUE(TimelineEditing::replaceClips(
                    &manager, firstTrack, firstOriginal).success);
    ASSERT_TRUE(TimelineEditing::replaceClips(
                    &manager, secondTrack, secondOriginal).success);
    ASSERT_TRUE(TimelineEditing::replaceClips(
                    &manager, bareTrack, bareOriginal).success);
    firstOriginal = firstTrack->comp<VTrack>()->clips;
    secondOriginal = secondTrack->comp<VTrack>()->clips;
    bareOriginal = bareTrack->comp<VTrack>()->clips;
    bareSurvivor = bareOriginal;
    bareSurvivor.items.pop_back();

    const MarkerLedger markers{{
        markerAt(MarkerKind::Entry, 0),
        markerAt(MarkerKind::Custom, 2 * kSecond, QStringLiteral("Accent")),
        markerAt(MarkerKind::Exit, 4 * kSecond),
    }};
    ASSERT_TRUE(TimelineEditing::replaceMarkers(&manager, piece, markers).success);

    TimelineEditorPanel piecePanel;
    piecePanel.bindItem(piece);
    ASSERT_EQ(piecePanel.laneCount(), 2);
    EXPECT_EQ(piecePanel.laneGuid(0), firstTrack->guid);
    EXPECT_EQ(piecePanel.laneGuid(1), secondTrack->guid);
    const TimelineSemanticsResult semantics = piecePanel.timelineSemantics();
    ASSERT_TRUE(semantics) << semantics.validation.message.toStdString();
    EXPECT_EQ(semantics.content,
              (TimelineSpan{-2 * kSecond, 6 * kSecond}));
    EXPECT_EQ(semantics.nominal, (TimelineSpan{0, 4 * kSecond}));
    EXPECT_EQ(semantics.pickup, 2 * kSecond);
    EXPECT_EQ(semantics.tail, 2 * kSecond);

    TimelineEditorPanel barePanel;
    barePanel.bindItem(bareTrack);
    EXPECT_EQ(barePanel.boundItem(), bareTrack);
    ASSERT_EQ(barePanel.laneCount(), 1);
    EXPECT_EQ(barePanel.laneGuid(0), bareTrack->guid);
    EXPECT_EQ(barePanel.clipCount(0), bareOriginal.items.size());
    EXPECT_EQ(barePanel.viewKind(), TimelineEditorPanel::ViewKind::Track);
    EXPECT_EQ(bareTrack->comp<VTrack>()->clips, bareOriginal);

    const QUuid firstOwnedGuid = firstOwned->guid;
    UndoRedoManager::instance().clear();
    ASSERT_TRUE(manager.deleteItem(firstOwned));
    EXPECT_EQ(manager.findItemByGuid(firstOwnedGuid), nullptr);
    EXPECT_TRUE(firstTrack->comp<VTrack>()->clips.empty());
    EXPECT_EQ(secondTrack->comp<VTrack>()->clips, secondOriginal);
    EXPECT_EQ(bareTrack->comp<VTrack>()->clips, bareOriginal);
    EXPECT_NE(manager.findItemByGuid(ownedPlaced->guid), nullptr);
    EXPECT_NE(manager.findItemByGuid(ownedShelved->guid), nullptr);

    QUndoStack* undo = UndoRedoManager::instance().undoStack();
    ASSERT_TRUE(undo->canUndo());
    undo->undo();
    EXPECT_NE(manager.findItemByGuid(firstOwnedGuid), nullptr);
    EXPECT_EQ(firstTrack->comp<VTrack>()->clips, firstOriginal);
    EXPECT_EQ(secondTrack->comp<VTrack>()->clips, secondOriginal);
    EXPECT_EQ(bareTrack->comp<VTrack>()->clips, bareOriginal);

    ASSERT_TRUE(undo->canRedo());
    undo->redo();
    EXPECT_TRUE(firstTrack->comp<VTrack>()->clips.empty());
    EXPECT_EQ(bareTrack->comp<VTrack>()->clips, bareOriginal);
    undo->undo();
    EXPECT_EQ(firstTrack->comp<VTrack>()->clips, firstOriginal);
    EXPECT_EQ(bareTrack->comp<VTrack>()->clips, bareOriginal);
}

TEST_F(TimelinePlacementAuthoringScenarioTest,
       QueueSpliceAndBlueprintUseTheSameAuthoredPlacement)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* master = manager.getMasterBusItem();
    ProjectItem* placementBus = manager.createItem(
        master, QStringLiteral("PlacementBus"), ProjectItem::Type::Bus);
    ProjectItem* organizationBus = manager.createItem(
        master, QStringLiteral("OrganizationBus"), ProjectItem::Type::Bus);
    ProjectItem* layer = manager.createItem(
        nullptr, QStringLiteral("OrganizationLayer"), ProjectItem::Type::Layer);
    ProjectItem* sound = manager.createItem(
        layer, QStringLiteral("PlacedSound"), ProjectItem::Type::Sound);
    ProjectItem* queue = manager.createItem(
        layer, QStringLiteral("SequenceQueue"), ProjectItem::Type::Container);
    ProjectItem* absolute = manager.createItem(
        queue, QStringLiteral("AbsolutePiece"), ProjectItem::Type::Piece);
    ProjectItem* absoluteTrack = manager.createItem(
        absolute, QStringLiteral("AbsoluteTrack"), ProjectItem::Type::Track);
    ProjectItem* secondary = manager.createItem(
        queue, QStringLiteral("SecondaryPiece"), ProjectItem::Type::Piece);
    ProjectItem* secondaryTrack = manager.createItem(
        secondary, QStringLiteral("SecondaryTrack"), ProjectItem::Type::Track);
    ProjectItem* absoluteSound = manager.createItem(
        absoluteTrack, QStringLiteral("AbsoluteSound"), ProjectItem::Type::Sound);
    ProjectItem* secondarySound = manager.createItem(
        secondaryTrack, QStringLiteral("SecondarySound"), ProjectItem::Type::Sound);
    ASSERT_NE(master, nullptr);
    ASSERT_NE(placementBus, nullptr);
    ASSERT_NE(organizationBus, nullptr);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(queue, nullptr);
    ASSERT_NE(absolute, nullptr);
    ASSERT_NE(absoluteTrack, nullptr);
    ASSERT_NE(secondary, nullptr);
    ASSERT_NE(secondaryTrack, nullptr);
    ASSERT_NE(absoluteSound, nullptr);
    ASSERT_NE(secondarySound, nullptr);

    queue->comp<VSelection>()->mode = SelectionMode::Sequence;
    queue->comp<VSelection>()->playType = SelectionPlayType::Queue;
    const Clip absoluteClip = clipFor(absoluteSound, -kSecond, 5 * kSecond);
    const Clip secondaryClip = clipFor(secondarySound, -500000000, 3500000000LL);
    ASSERT_TRUE(TimelineEditing::replaceClips(
                    &manager, absoluteTrack,
                    makeClipLedger({absoluteClip})).success);
    ASSERT_TRUE(TimelineEditing::replaceClips(
                    &manager, secondaryTrack,
                    makeClipLedger({secondaryClip})).success);
    ASSERT_TRUE(TimelineEditing::replaceMarkers(
                    &manager, absolute,
                    {{markerAt(MarkerKind::Entry, 0),
                      markerAt(MarkerKind::Exit, 3 * kSecond)}}).success);
    ASSERT_TRUE(TimelineEditing::replaceMarkers(
                    &manager, secondary,
                    {{markerAt(MarkerKind::Entry, 0),
                      markerAt(MarkerKind::Exit, 2 * kSecond)}}).success);

    const auto resolve = [&manager](const QUuid& guid) {
        return manager.findItemByGuid(guid);
    };
    const QueuePreviewResult butt = TimelineSemantics::queuePreview(*queue, resolve, false);
    ASSERT_TRUE(butt) << butt.validation.message.toStdString();
    ASSERT_EQ(butt.items.size(), 2u);
    EXPECT_EQ(butt.items[0].contentNanoseconds,
              (TimelineSpan{0, 5 * kSecond}));
    EXPECT_EQ(butt.items[0].nominalNanoseconds,
              (TimelineSpan{kSecond, 4 * kSecond}));
    EXPECT_EQ(butt.items[1].contentNanoseconds,
              (TimelineSpan{5 * kSecond, 8500000000LL}));
    EXPECT_EQ(butt.items[1].nominalNanoseconds,
              (TimelineSpan{5500000000LL, 7500000000LL}));
    EXPECT_EQ(butt.endNanoseconds, 8500000000LL);

    const QueuePreviewResult splice = TimelineSemantics::queuePreview(*queue, resolve, true);
    ASSERT_TRUE(splice) << splice.validation.message.toStdString();
    ASSERT_EQ(splice.items.size(), 2u);
    EXPECT_EQ(splice.items[0].contentNanoseconds,
              (TimelineSpan{-kSecond, 4 * kSecond}));
    EXPECT_EQ(splice.items[0].nominalNanoseconds,
              (TimelineSpan{0, 3 * kSecond}));
    EXPECT_EQ(splice.items[1].contentNanoseconds,
              (TimelineSpan{2500000000LL, 6 * kSecond}));
    EXPECT_EQ(splice.items[1].nominalNanoseconds,
              (TimelineSpan{3 * kSecond, 5 * kSecond}));
    EXPECT_EQ(splice.items[1].pickupNanoseconds, 500000000);
    EXPECT_EQ(splice.items[1].tailNanoseconds, kSecond);
    EXPECT_TRUE(splice.items[0].entryAnchor);
    EXPECT_TRUE(splice.items[1].entryAnchor);
    EXPECT_EQ(splice.endNanoseconds, 5 * kSecond);

    auto& actions = ActionManager::instance();
    for (const auto& [owner, bus] : std::array{
             std::pair{absoluteTrack, placementBus}, std::pair{layer, organizationBus}}) {
        QJsonObject edit{{"definition_guid", owner->guid.toString(QUuid::WithoutBraces)},
                         {"group", "mainOutput"}, {"scope", "Shared"}, {"mode", "Local"}};
        if (owner == absoluteTrack)
            ASSERT_TRUE(actions.execute("set_field_group_mode", edit).success);
        edit.insert("field", "mainOutputBusRef");
        edit.insert("value", bus->guid.toString(QUuid::WithoutBraces));
        ASSERT_TRUE(actions.execute("set_field_group_field", edit).success);
    }
    TimelineEditorPanel absolutePanel;
    absolutePanel.bindItem(absolute);
    const PlacementContext context = absolutePanel.placementContextForClip(
        0, absoluteClip.clipGuid);
    const SignalChainView view = manager.buildSignalChainView(absoluteSound, context);
    ASSERT_TRUE(view.placementContextValid)
        << view.placementContextError.toStdString();
    ASSERT_EQ(view.placementChain.size(), 2u);
    EXPECT_EQ(view.placementChain[0].objectID, absoluteTrack->objectID);
    EXPECT_EQ(view.placementChain[1].objectID, absolute->objectID);
    ASSERT_EQ(view.organizationChain.size(), 2u);
    EXPECT_EQ(view.organizationChain[0].objectID, queue->objectID);
    EXPECT_EQ(view.organizationChain[1].objectID, layer->objectID);
    EXPECT_EQ(view.busSlotOwnerObjectID, absoluteTrack->objectID);
    ASSERT_FALSE(view.busChain.empty());
    EXPECT_EQ(view.busChain.front().busID, placementBus->objectID);
    ASSERT_FALSE(view.slotSources.empty());
    EXPECT_EQ(view.slotSources.front().source, QStringLiteral("organization"));
    EXPECT_EQ(view.slotSources.front().via,
              QStringLiteral("via organization AbsoluteTrack"));

    const auto edges = SignalChainSource::edgesFromView(
        view, kAllBlueprintLayers);
    const BlueprintEdgeDesc* placementEdge = findItemEdge(
        edges, absoluteSound->objectID, absoluteTrack->objectID);
    ASSERT_NE(placementEdge, nullptr);
    EXPECT_EQ(placementEdge->edgeKind,
              static_cast<uint8_t>(BlueprintEdgeKind::Placement));
    EXPECT_EQ(findItemEdge(edges, absoluteSound->objectID, layer->objectID),
              nullptr);
    EXPECT_EQ(findItemEdge(edges, absoluteTrack->objectID, layer->objectID),
              nullptr);

    const SignalChainView secondaryView = manager.buildSignalChainView(
        secondarySound, PlacementContext::fromOccurrence(
            {secondaryTrack->guid, secondaryClip.clipGuid, secondarySound->guid}));
    ASSERT_TRUE(secondaryView.placementContextValid);
    ASSERT_FALSE(secondaryView.placementChain.empty());
    EXPECT_EQ(secondaryView.placementChain.front().objectID, secondaryTrack->objectID);
    EXPECT_EQ(secondaryView.busSlotOwnerObjectID, layer->objectID);
    ASSERT_FALSE(secondaryView.busChain.empty());
    EXPECT_EQ(secondaryView.busChain.front().busID, organizationBus->objectID);

    const ClipLedger originalClips = absoluteTrack->comp<VTrack>()->clips;
    UndoRedoManager::instance().clear();
    ASSERT_TRUE(manager.moveItem(sound, absoluteTrack));
    EXPECT_EQ(sound->parent, absoluteTrack);
    const auto& movedClips = absoluteTrack->comp<VTrack>()->clips.items;
    ASSERT_EQ(movedClips.size(), originalClips.items.size() + 1);
    EXPECT_EQ(movedClips.back().targetGuid, sound->guid);
    EXPECT_EQ(movedClips.back().start, 0);
    const SignalChainView movedView = manager.buildSignalChainView(sound, {});
    EXPECT_EQ(movedView.busSlotOwnerObjectID, absoluteTrack->objectID);
    ASSERT_FALSE(movedView.busChain.empty());
    EXPECT_EQ(movedView.busChain.front().busID, placementBus->objectID);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(sound->parent, layer);
    EXPECT_EQ(absoluteTrack->comp<VTrack>()->clips.items, originalClips.items);
    const SignalChainView restoredView = manager.buildSignalChainView(sound, {});
    EXPECT_EQ(restoredView.busSlotOwnerObjectID, layer->objectID);
    ASSERT_FALSE(restoredView.busChain.empty());
    EXPECT_EQ(restoredView.busChain.front().busID, organizationBus->objectID);
}
