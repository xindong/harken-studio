#include "ActionManager.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "Timeline/TimelineEditing.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QTemporaryDir>

namespace VNX {
uint32_t itemShortID(const ProjectItem* item);
}

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class TimelinePropertyRouteTest : public ::testing::Test
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
        config.name = QStringLiteral("TimelinePropertyRouteProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        server_.setProjectManager(&manager);

        layer_ = manager.createItem(nullptr, QStringLiteral("RouteLayer"),
                                    ProjectItem::Type::Layer);
        track_ = manager.createItem(layer_, QStringLiteral("RouteTrack"),
                                    ProjectItem::Type::Track);
        sound_ = manager.createItem(track_, QStringLiteral("RouteSound"),
                                    ProjectItem::Type::Sound);
        piece_ = manager.createItem(layer_, QStringLiteral("RoutePiece"),
                                    ProjectItem::Type::Piece);
        queue_ = manager.createItem(layer_, QStringLiteral("RouteQueue"),
                                    ProjectItem::Type::Container);
        ASSERT_NE(layer_, nullptr);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(piece_, nullptr);
        ASSERT_NE(queue_, nullptr);
        queue_->comp<VSelection>()->mode = SelectionMode::Sequence;
        queue_->comp<VSelection>()->playType = SelectionPlayType::Queue;
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ActionResult guiSet(ProjectItem* item, const QString& property,
                        const QJsonValue& value)
    {
        return ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
             {QStringLiteral("property"), property},
             {QStringLiteral("value"), value}},
            ActionSource::GUI);
    }

    QJsonObject ipcSet(ProjectItem* item, const QString& property,
                       const QJsonValue& value)
    {
        return server_.dispatchCommand(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))},
             {QStringLiteral("property"), property},
             {QStringLiteral("value"), value}});
    }

    QJsonObject ipcGet(ProjectItem* item)
    {
        return server_.dispatchCommand(
            QStringLiteral("get_item_info"),
            {{QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))}});
    }

    QTemporaryDir tempDir_;
    IPCServer server_{nullptr};
    ProjectItem* layer_ = nullptr;
    ProjectItem* track_ = nullptr;
    ProjectItem* sound_ = nullptr;
    ProjectItem* piece_ = nullptr;
    ProjectItem* queue_ = nullptr;
};

}

TEST_F(TimelinePropertyRouteTest, GuiAndIpcClipsUseTheSameJsonAndUndoRoute)
{
    const ClipLedger original = track_->comp<VTrack>()->clips;
    Clip clip{QUuid::createUuid(), sound_->guid, -15, 90};
    ClipLedger authored;
    ASSERT_TRUE(authored.mintOccurrenceOrdinal(&clip));
    authored.items.push_back(clip);
    const QJsonObject clips = authored.toJsonObject();

    const ActionResult gui = guiSet(track_, QStringLiteral("clips"), clips);
    ASSERT_TRUE(gui.success) << gui.errorMessage.toStdString();
    authored = track_->comp<VTrack>()->clips;
    EXPECT_EQ(gui.data.value(QStringLiteral("value")).toObject(),
              authored.toJsonObject());
    EXPECT_EQ(track_->comp<VTrack>()->clips, authored);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(track_->comp<VTrack>()->clips, original);
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(track_->comp<VTrack>()->clips, authored);

    UndoRedoManager::instance().undoStack()->undo();
    const QJsonObject ipc = ipcSet(
        track_, QStringLiteral("clips"), authored.toJsonObject());
    ASSERT_EQ(ipc.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(ipc.value(QStringLiteral("result")).toObject(), gui.data);
    EXPECT_EQ(track_->comp<VTrack>()->clips, authored);
    const QJsonObject info = ipcGet(track_);
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(info.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("components")).toObject()
                  .value(QStringLiteral("tracktimeline")).toObject()
                  .value(QStringLiteral("clips")).toArray(),
              authored.toJson());
    EXPECT_EQ(info.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("components")).toObject()
                  .value(QStringLiteral("tracktimeline")).toObject()
                  .value(QStringLiteral("occurrenceOrdinalHighWater")).toInteger(),
              1);
}

TEST_F(TimelinePropertyRouteTest, MusicalTimeIsRejectedWhileQueueMarkerSpliceRemainsEditable)
{
    const PieceTimeSpec musical = PieceTimeSpec::musical(500000, 4, 4);
    ActionResult gui = guiSet(piece_, QStringLiteral("pieceTime"), musical.toJson());
    EXPECT_FALSE(gui.success);
    EXPECT_EQ(gui.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    EXPECT_EQ(piece_->comp<VPiece>()->pieceTime, PieceTimeSpec{});
    QJsonObject ipc = ipcSet(piece_, QStringLiteral("pieceTime"), musical.toJson());
    EXPECT_EQ(ipc.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(ipc.value(QStringLiteral("code")).toString(), gui.errorCode);
    EXPECT_EQ(ipc.value(QStringLiteral("message")).toString(), gui.errorMessage);
    EXPECT_EQ(piece_->comp<VPiece>()->pieceTime, PieceTimeSpec{});
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);

    gui = guiSet(queue_, QStringLiteral("spliceByMarker"), true);
    ASSERT_TRUE(gui.success) << gui.errorMessage.toStdString();
    EXPECT_TRUE(queue_->comp<VSelection>()->spliceByMarker);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FALSE(queue_->comp<VSelection>()->spliceByMarker);

    ipc = ipcSet(queue_, QStringLiteral("spliceByMarker"), true);
    ASSERT_EQ(ipc.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_TRUE(queue_->comp<VSelection>()->spliceByMarker);
    const QJsonObject info = ipcGet(queue_);
    EXPECT_TRUE(info.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("components")).toObject()
                    .value(QStringLiteral("selection")).toObject()
                    .value(QStringLiteral("spliceByMarker")).toBool());
}

TEST_F(TimelinePropertyRouteTest, PlayTypeChangesHideWithoutClearingSpliceValue)
{
    ASSERT_TRUE(guiSet(queue_, QStringLiteral("spliceByMarker"), true).success);
    ASSERT_TRUE(guiSet(queue_, QStringLiteral("playType"),
                       static_cast<int>(SelectionPlayType::Single)).success);
    EXPECT_TRUE(queue_->comp<VSelection>()->spliceByMarker);
    EXPECT_EQ(queue_->comp<VSelection>()->mode, SelectionMode::Sequence);
    EXPECT_FALSE(queue_->comp<VSelection>()->isQueue());

    ASSERT_TRUE(guiSet(queue_, QStringLiteral("playType"),
                       static_cast<int>(SelectionPlayType::Queue)).success);
    EXPECT_TRUE(queue_->comp<VSelection>()->spliceByMarker);
    EXPECT_TRUE(queue_->comp<VSelection>()->isQueue());

    const QJsonObject json = queue_->comp<VSelection>()->toJson();
    EXPECT_TRUE(json.value(QStringLiteral("spliceByMarker")).toBool());
    VSelection restored;
    restored.fromJson(json);
    EXPECT_TRUE(restored.spliceByMarker);
}

TEST_F(TimelinePropertyRouteTest, InvalidAtomicValuesAreRejectedWithoutMutationOrUndo)
{
    Clip original{QUuid::createUuid(), sound_->guid, 5, 25};
    ClipLedger authored;
    ASSERT_TRUE(authored.mintOccurrenceOrdinal(&original));
    authored.items.push_back(original);
    ASSERT_TRUE(guiSet(track_, QStringLiteral("clips"),
                       authored.toJsonObject()).success);
    authored = track_->comp<VTrack>()->clips;
    const ClipLedger before = track_->comp<VTrack>()->clips;
    const int undoCount = UndoRedoManager::instance().undoStack()->count();

    QJsonObject invalidClip = original.toJson();
    invalidClip[QStringLiteral("duration")] = 0;
    QJsonObject invalidLedger = authored.toJsonObject();
    invalidLedger[QStringLiteral("rows")] = QJsonArray{invalidClip};
    ActionResult gui = guiSet(track_, QStringLiteral("clips"),
                              invalidLedger);
    EXPECT_FALSE(gui.success);
    EXPECT_EQ(gui.errorCode, QStringLiteral("INVALID_ARGS"));
    EXPECT_EQ(track_->comp<VTrack>()->clips, before);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);

    const QJsonObject sameInvalid = ipcSet(track_, QStringLiteral("clips"),
                                           invalidLedger);
    EXPECT_EQ(sameInvalid.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(sameInvalid.value(QStringLiteral("code")).toString(), gui.errorCode);
    EXPECT_EQ(sameInvalid.value(QStringLiteral("message")).toString(), gui.errorMessage);
    EXPECT_EQ(track_->comp<VTrack>()->clips, before);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);

    invalidClip = original.toJson();
    invalidClip[QStringLiteral("targetGuid")] = track_->guid.toString(QUuid::WithoutBraces);
    invalidLedger[QStringLiteral("rows")] = QJsonArray{invalidClip};
    const QJsonObject ipc = ipcSet(track_, QStringLiteral("clips"),
                                   invalidLedger);
    EXPECT_EQ(ipc.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(track_->comp<VTrack>()->clips, before);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);

    QJsonObject missingHighWater = authored.toJsonObject();
    missingHighWater.remove(QStringLiteral("occurrenceOrdinalHighWater"));
    EXPECT_FALSE(guiSet(track_, QStringLiteral("clips"), missingHighWater).success);
    EXPECT_FALSE(guiSet(track_, QStringLiteral("clips"), authored.toJson()).success);
    EXPECT_EQ(track_->comp<VTrack>()->clips, before);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);

    QJsonObject invalidTime = PieceTimeSpec::musical(500000, 4, 4).toJson();
    invalidTime[QStringLiteral("tempoMap")] = QJsonArray{};
    EXPECT_FALSE(guiSet(piece_, QStringLiteral("pieceTime"), invalidTime).success);
    EXPECT_EQ(piece_->comp<VPiece>()->pieceTime, PieceTimeSpec{});

    queue_->comp<VSelection>()->mode = SelectionMode::Sequence;
    queue_->comp<VSelection>()->playType = SelectionPlayType::Single;
    EXPECT_FALSE(guiSet(queue_, QStringLiteral("spliceByMarker"), true).success);
    EXPECT_FALSE(queue_->comp<VSelection>()->spliceByMarker);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_F(TimelinePropertyRouteTest,
       DeleteAllReplacementUndoRedoAndNextMintPreserveExactHighWater)
{
    Clip original{QUuid::createUuid(), sound_->guid, 5, 25};
    ClipLedger authored;
    ASSERT_TRUE(authored.mintOccurrenceOrdinal(&original));
    authored.items.push_back(original);
    ASSERT_TRUE(guiSet(track_, QStringLiteral("clips"),
                       authored.toJsonObject()).success);
    authored = track_->comp<VTrack>()->clips;

    ClipLedger emptyAfterDelete = authored;
    emptyAfterDelete.items.clear();
    const ActionResult replacement = guiSet(
        track_, QStringLiteral("clips"), emptyAfterDelete.toJsonObject());
    ASSERT_TRUE(replacement.success) << replacement.errorMessage.toStdString();
    EXPECT_EQ(track_->comp<VTrack>()->clips, emptyAfterDelete);
    EXPECT_EQ(replacement.data.value(QStringLiteral("value")).toObject(),
              emptyAfterDelete.toJsonObject());

    QUndoStack* undo = UndoRedoManager::instance().undoStack();
    undo->undo();
    EXPECT_EQ(track_->comp<VTrack>()->clips, authored);
    undo->redo();
    EXPECT_EQ(track_->comp<VTrack>()->clips, emptyAfterDelete);

    Clip fresh{QUuid::createUuid(), sound_->guid, 30, 10};
    ASSERT_TRUE(TimelineEditing::addClip(
        &ProjectManager::instance(), track_, fresh).success);
    const ClipLedger& result = track_->comp<VTrack>()->clips;
    ASSERT_EQ(result.items.size(), 1u);
    EXPECT_EQ(result.items[0].stableOccurrenceOrdinal, 1u);
    EXPECT_EQ(result.occurrenceOrdinalHighWater, 2u);
}
