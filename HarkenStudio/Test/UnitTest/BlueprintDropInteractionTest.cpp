#include "Core/Components/VSelection.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Blueprint/SignalPathEditing.h"
#include "Widgets/Shared/ProjectItemMime.h"
#include "Core/ActionManager.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QGraphicsScene>
#include <QMimeData>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using namespace VNX;
using T = ProjectItemType;
using Feedback = NodeItem::DropFeedback;

class DropCanvas final : public BlueprintCanvasWidget
{
public:
    using BlueprintCanvasWidget::dragEnterEvent;
    using BlueprintCanvasWidget::dragLeaveEvent;
    using BlueprintCanvasWidget::dragMoveEvent;
    using BlueprintCanvasWidget::dropEvent;
};

void drainDropEvents()
{
    QApplication::processEvents();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

class BlueprintDropInteractionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char name[] = "harken_editor_tests";
            static char* argv[] = {name, nullptr};
            static QApplication app(argc, argv);
        }
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        for (const auto* key : {"Blueprint/InteractionMode", "Blueprint/CanvasFlowDirection",
                 "Blueprint/GridSnapEnabled"})
            preferences.insert(QString::fromLatin1(key), QSettings().value(QString::fromLatin1(key)));
        QSettings().setValue("Blueprint/InteractionMode", "Select");
        QSettings().setValue("Blueprint/CanvasFlowDirection", "LR");
        QSettings().setValue("Blueprint/GridSnapEnabled", false);
        ASSERT_TRUE(directory.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("BlueprintDropInteraction");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({"Windows", "Android"});
        ASSERT_TRUE(manager.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, true);
        master = manager.getMasterBusItem();
        b1 = create("B1", T::Bus, master);
        b2 = create("B2", T::Bus, master);
        b3 = create("B3", T::Bus, master);
        layer = create("Layer", T::Layer);
        c1 = create("C1", T::Container, layer);
        c2 = create("C2", T::Container, layer);
        s1 = create("S1", T::Sound, c1);
        s2 = create("S2", T::Sound, c1);
        setOutput(c1, b1, -7.0f);
        setOutput(c2, b3, -2.0f);
        c1->fieldGroups.send.shared = {FieldGroupMode::Local,
            SendFieldGroup{{{toFieldGroupGuid(b2->guid), -6.0f}}}};
        UndoRedoManager::instance().clear();
        drainDropEvents();
    }

    void TearDown() override
    {
        canvas.reset();
        UndoRedoManager::instance().clear();
        ActionManager::instance().clearAllForTesting();
        ProjectManager::instance().closeProject();
        drainDropEvents();
        for (auto it = preferences.cbegin(); it != preferences.cend(); ++it) {
            if (it.value().isValid()) QSettings().setValue(it.key(), it.value());
            else QSettings().remove(it.key());
        }
        PlatformContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    ProjectItem* create(const char* name, T type, ProjectItem* parent = nullptr)
    {
        auto* item = ProjectManager::instance().createItem(parent, QString::fromLatin1(name), type);
        if (!item) throw std::runtime_error("Cannot create test object");
        return item;
    }

    void setOutput(ProjectItem* item, const ProjectItem* bus, float volume)
    {
        item->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(bus->guid), volume}};
    }

    void showCanvas()
    {
        canvas = std::make_unique<DropCanvas>();
        canvas->resize(1400, 900);
        canvas->show();
        canvas->zoomToFit();
        drainDropEvents();
    }

    SignalPathEndpoint endpoint(const ProjectItem* item) const
    {
        const auto& nodes = canvas->activeSignalPathSource()->graph().nodes;
        for (size_t index = 0; index < nodes.size(); ++index)
            if (nodes[index].key.objectGuid == item->guid) return {index, 0};
        throw std::runtime_error("Expected blueprint object is missing");
    }

    NodeItem* node(const ProjectItem* item) const
    {
        return canvas->signalPathNode(endpoint(item).occurrenceIndex);
    }

    NodeItem* gainNode() const
    {
        const auto& edges = canvas->activeSignalPathSource()->graph().edges;
        for (size_t index = 0; index < edges.size(); ++index)
            if (auto* gain = canvas->signalPathGainNode(index)) return gain;
        throw std::runtime_error("Expected route gain is missing");
    }

    QPoint body(const NodeItem* item) const
    {
        const auto bounds = item->shape().boundingRect();
        return canvas->mapFromScene(item->mapToScene(
            QPointF(bounds.center().x(), bounds.bottom() - 3.0)));
    }

    QPoint body(const ProjectItem* item) const { return body(node(item)); }

    QPoint blank() const
    {
        const auto area = canvas->viewport()->rect().adjusted(8, 8, -8, -8);
        for (const auto point : {area.topLeft(), area.topRight(), area.bottomLeft(), area.bottomRight()})
            if (!canvas->itemAt(point)) return point;
        throw std::runtime_error("Expected blank canvas area is missing");
    }

    std::unique_ptr<QMimeData> objects(const QList<uint32_t>& ids) const
    {
        auto mime = std::make_unique<QMimeData>();
        ProjectItemMime::writeObjectIDs(mime.get(), ids);
        return mime;
    }

    bool enter(const QMimeData* mime, const QPoint& position)
    {
        QDragEnterEvent event(position, Qt::CopyAction | Qt::MoveAction, mime,
            Qt::LeftButton, Qt::NoModifier);
        canvas->dragEnterEvent(&event);
        return event.isAccepted();
    }

    bool move(const QMimeData* mime, const QPoint& position)
    {
        QDragMoveEvent event(position, Qt::CopyAction | Qt::MoveAction, mime,
            Qt::LeftButton, Qt::NoModifier);
        canvas->dragMoveEvent(&event);
        return event.isAccepted();
    }

    Qt::DropAction drop(const QMimeData* mime, const QPoint& position)
    {
        QDropEvent event(QPointF(position), Qt::CopyAction | Qt::MoveAction, mime,
            Qt::LeftButton, Qt::NoModifier);
        canvas->dropEvent(&event);
        return event.isAccepted() ? event.dropAction() : Qt::IgnoreAction;
    }

    void leave()
    {
        QDragLeaveEvent event;
        canvas->dragLeaveEvent(&event);
    }

    EffectiveDefinitionFieldGroups effective(const ProjectItem* item) const
    {
        const auto snapshot = buildFieldGroupEffectiveSnapshot(
            *ProjectManager::instance().currentProject(), HarkenTest::FieldGroupPlatformIds::Windows);
        EXPECT_TRUE(snapshot.ok());
        return snapshot.definitions.at(toFieldGroupGuid(item->guid));
    }

    std::unique_ptr<QMimeData> audio()
    {
        const QString path = directory.filePath(QStringLiteral("external.wav"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create test WAV");
        const auto wav = QByteArray::fromHex(
            "524946462600000057415645666d7420100000000100010044ac0000885801000200100064617461020000000000");
        if (file.write(wav) != wav.size()) throw std::runtime_error("Cannot write test WAV");
        file.close();
        ActionDef action;
        action.id = QStringLiteral("import_audio_to_canvas");
        action.undoable = false;
        action.handler = [this](const QJsonObject& args) {
            imports.push_back(args);
            ActionResult result;
            result.success = true;
            return result;
        };
        ActionManager::instance().registerAction(action);
        auto mime = std::make_unique<QMimeData>();
        mime->setUrls({QUrl::fromLocalFile(path)});
        return mime;
    }

    QTemporaryDir directory;
    QMap<QString, QVariant> preferences;
    std::unique_ptr<DropCanvas> canvas;
    std::vector<QJsonObject> imports;
    ProjectItem* master = nullptr;
    ProjectItem* b1 = nullptr;
    ProjectItem* b2 = nullptr;
    ProjectItem* b3 = nullptr;
    ProjectItem* layer = nullptr;
    ProjectItem* c1 = nullptr;
    ProjectItem* c2 = nullptr;
    ProjectItem* s1 = nullptr;
    ProjectItem* s2 = nullptr;
};

TEST_F(BlueprintDropInteractionTest, ExistingObjectUsesNodeDropSemanticsAndPreservesIdentityWithUndo)
{
    showCanvas();
    const auto before = s1->fieldGroups;
    const auto occurrences = c1->comp<VNX::VSelection>()->selections.shared;
    const auto id = s1->objectID;
    const auto guid = s1->guid;
    int created = 0;
    QObject::connect(&ProjectManager::instance(), &ProjectManager::itemCreated, canvas.get(),
        [&created] { ++created; });
    const auto plan = prepareSignalPathNodeDrop(canvas->activeSignalPathSource()->graph(),
        {endpoint(s1)}, endpoint(c2));
    ASSERT_TRUE(plan.ok()) << plan.error.toStdString();
    auto mime = objects({id});
    const auto target = body(c2);
    ASSERT_TRUE(enter(mime.get(), target));
    ASSERT_TRUE(move(mime.get(), target));
    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::Valid);
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_EQ(drop(mime.get(), target), Qt::MoveAction);
    drainDropEvents();
    EXPECT_EQ(s1->parent, c2);
    EXPECT_EQ(s1->fieldGroups, before);
    EXPECT_EQ(effective(s1).mainOutput.mainOutputBusRef, toFieldGroupGuid(b3->guid));
    EXPECT_TRUE(effective(s1).send.sends.empty());
    EXPECT_EQ(ProjectManager::instance().findItemByObjectID(id), s1);
    EXPECT_EQ(s1->guid, guid);
    EXPECT_EQ(created, 0);
    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s1->fieldGroups, before);
    EXPECT_EQ(c1->comp<VNX::VSelection>()->selections.shared, occurrences);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, c2);
    EXPECT_EQ(created, 0);
}

TEST_F(BlueprintDropInteractionTest, MovingLocalOutputObjectKeepsItsOwnRouting)
{
    setOutput(s1, master, -11.0f);
    const auto before = s1->fieldGroups;
    showCanvas();
    auto mime = objects({s1->objectID});
    ASSERT_TRUE(enter(mime.get(), body(c2)));
    ASSERT_TRUE(move(mime.get(), body(c2)));
    EXPECT_EQ(drop(mime.get(), body(c2)), Qt::MoveAction);
    EXPECT_EQ(s1->parent, c2);
    EXPECT_EQ(s1->fieldGroups, before);
    EXPECT_EQ(effective(s1).mainOutput.mainOutputBusRef, toFieldGroupGuid(master->guid));
    EXPECT_FLOAT_EQ(effective(s1).mainOutput.outputBusVolume, -11.0f);
}

TEST_F(BlueprintDropInteractionTest, ObjectToBusMovesToRootAndRedirectsOutput)
{
    showCanvas();
    const auto before = s1->fieldGroups;
    auto mime = objects({s1->objectID});
    ASSERT_TRUE(enter(mime.get(), body(b3)));
    ASSERT_TRUE(move(mime.get(), body(b3)));
    EXPECT_EQ(node(b3)->dropFeedback(), Feedback::Valid);
    EXPECT_EQ(drop(mime.get(), body(b3)), Qt::MoveAction);
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(effective(s1).mainOutput.mainOutputBusRef, toFieldGroupGuid(b3->guid));
    EXPECT_FLOAT_EQ(effective(s1).mainOutput.outputBusVolume, -7.0f);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s1->fieldGroups, before);
}

TEST_F(BlueprintDropInteractionTest, BusToBusReparentsAndRedirectsOutput)
{
    showCanvas();
    auto mime = objects({b1->objectID});
    ASSERT_TRUE(enter(mime.get(), body(b3)));
    ASSERT_TRUE(move(mime.get(), body(b3)));
    EXPECT_EQ(node(b3)->dropFeedback(), Feedback::Valid);
    EXPECT_EQ(drop(mime.get(), body(b3)), Qt::MoveAction);
    EXPECT_EQ(b1->parent, b3);
    drainDropEvents();
    const auto& graph = canvas->activeSignalPathSource()->graph();
    const auto from = endpoint(b1);
    const auto to = endpoint(b3);
    EXPECT_TRUE(std::any_of(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
        return edge.kind == SignalPathEdgeKind::Output && edge.from == from && edge.to == to;
    }));
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(b1->parent, master);
}

TEST_F(BlueprintDropInteractionTest, MultipleObjectsMoveAsOneTransactionWithoutCloning)
{
    showCanvas();
    const auto occurrences = c1->comp<VNX::VSelection>()->selections.shared;
    const auto firstBefore = s1->fieldGroups;
    const auto secondBefore = s2->fieldGroups;
    int created = 0;
    QObject::connect(&ProjectManager::instance(), &ProjectManager::itemCreated, canvas.get(),
        [&created] { ++created; });
    auto mime = objects({s1->objectID, s2->objectID});
    ASSERT_TRUE(enter(mime.get(), body(c2)));
    ASSERT_TRUE(move(mime.get(), body(c2)));
    EXPECT_EQ(drop(mime.get(), body(c2)), Qt::MoveAction);
    EXPECT_EQ(s1->parent, c2);
    EXPECT_EQ(s2->parent, c2);
    EXPECT_EQ(s1->fieldGroups, firstBefore);
    EXPECT_EQ(s2->fieldGroups, secondBefore);
    EXPECT_EQ(created, 0);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s2->parent, c1);
    EXPECT_EQ(c1->comp<VNX::VSelection>()->selections.shared, occurrences);
}

TEST_F(BlueprintDropInteractionTest, InvalidMemberRejectsEntireObjectBatch)
{
    auto* variable = create("Variable", T::Variable);
    showCanvas();
    const auto before = s1->fieldGroups;
    const auto undoCount = UndoRedoManager::instance().undoStack()->count();
    const std::vector<QList<uint32_t>> batches{
        {s1->objectID, std::numeric_limits<uint32_t>::max()},
        {s1->objectID, variable->objectID},
        {s1->objectID, b1->objectID}};
    for (const auto& ids : batches) {
        SCOPED_TRACE(ids.back());
        auto mime = objects(ids);
        enter(mime.get(), body(c2));
        EXPECT_FALSE(move(mime.get(), body(c2)));
        EXPECT_EQ(drop(mime.get(), body(c2)), Qt::IgnoreAction);
        EXPECT_EQ(s1->parent, c1);
        EXPECT_EQ(b1->parent, master);
        EXPECT_EQ(s1->fieldGroups, before);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
        leave();
        EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
    }
}

TEST_F(BlueprintDropInteractionTest, SelfCycleMasterAndGainTargetsCannotBeMovedInto)
{
    showCanvas();
    const std::vector<std::pair<ProjectItem*, ProjectItem*>> invalid{
        {s1, s1}, {layer, c1}, {master, b1}};
    for (const auto& [source, target] : invalid) {
        auto mime = objects({source->objectID});
        enter(mime.get(), body(target));
        EXPECT_FALSE(move(mime.get(), body(target)));
        EXPECT_EQ(node(target)->dropFeedback(), Feedback::Invalid);
        EXPECT_EQ(drop(mime.get(), body(target)), Qt::IgnoreAction);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
        leave();
    }
    auto mime = objects({s1->objectID});
    auto* gain = gainNode();
    enter(mime.get(), body(gain));
    EXPECT_FALSE(move(mime.get(), body(gain)));
    EXPECT_EQ(gain->dropFeedback(), Feedback::None);
    EXPECT_EQ(drop(mime.get(), body(gain)), Qt::IgnoreAction);
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(BlueprintDropInteractionTest, BlankObjectDropDoesNotReparentOrReposition)
{
    showCanvas();
    const auto position = node(s1)->pos();
    const auto before = s1->fieldGroups;
    auto mime = objects({s1->objectID});
    ASSERT_TRUE(enter(mime.get(), body(c2)));
    ASSERT_TRUE(move(mime.get(), body(c2)));
    const auto empty = blank();
    EXPECT_FALSE(move(mime.get(), empty));
    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
    EXPECT_EQ(drop(mime.get(), empty), Qt::IgnoreAction);
    drainDropEvents();
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(s1->fieldGroups, before);
    EXPECT_EQ(node(s1)->pos(), position);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(BlueprintDropInteractionTest, ObjectDropResolvesReleasePositionInsteadOfPreviousHover)
{
    showCanvas();
    auto mime = objects({s1->objectID});
    ASSERT_TRUE(enter(mime.get(), body(c2)));
    ASSERT_TRUE(move(mime.get(), body(c2)));
    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::Valid);
    EXPECT_EQ(drop(mime.get(), body(b3)), Qt::MoveAction);
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(effective(s1).mainOutput.mainOutputBusRef, toFieldGroupGuid(b3->guid));
    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(BlueprintDropInteractionTest, HoverKeepsNodesAliveUntilLeaveAndResumesRefresh)
{
    showCanvas();
    auto mime = objects({s1->objectID});
    auto* target = node(c2);
    ASSERT_TRUE(enter(mime.get(), body(target)));
    ASSERT_TRUE(move(mime.get(), body(target)));
    emit ProjectManager::instance().itemPropertyChanged(c2, QStringLiteral("volume"));
    drainDropEvents();
    ASSERT_EQ(node(c2), target);
    EXPECT_EQ(target->dropFeedback(), Feedback::Valid);
    leave();
    EXPECT_EQ(target->dropFeedback(), Feedback::None);
    drainDropEvents();
    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(BlueprintDropInteractionTest, ParentChangeDuringDragIsRevalidatedBeforeDrop)
{
    showCanvas();
    auto mime = objects({c1->objectID});
    const auto target = body(c2);
    ASSERT_TRUE(enter(mime.get(), target));
    ASSERT_TRUE(move(mime.get(), target));
    ASSERT_TRUE(ProjectManager::instance().moveItem(c2, c1));
    const auto undoCount = UndoRedoManager::instance().undoStack()->count();
    drainDropEvents();
    EXPECT_EQ(drop(mime.get(), target), Qt::IgnoreAction);
    EXPECT_EQ(c1->parent, layer);
    EXPECT_EQ(c2->parent, c1);
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_F(BlueprintDropInteractionTest, LeaveAndClearRejectLateDropEvents)
{
    showCanvas();
    auto mime = objects({s1->objectID});
    const auto target = body(c2);
    ASSERT_TRUE(enter(mime.get(), target));
    ASSERT_TRUE(move(mime.get(), target));
    leave();
    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
    EXPECT_EQ(drop(mime.get(), target), Qt::IgnoreAction);
    EXPECT_EQ(s1->parent, c1);
    ASSERT_TRUE(enter(mime.get(), target));
    ASSERT_TRUE(move(mime.get(), target));
    canvas->clear();
    EXPECT_EQ(drop(mime.get(), target), Qt::IgnoreAction);
    EXPECT_EQ(s1->parent, c1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(BlueprintDropInteractionTest, AudioUsesValidFeedbackAndImportsAtTheReleaseTarget)
{
    showCanvas();
    auto mime = audio();
    ASSERT_TRUE(enter(mime.get(), body(c1)));
    ASSERT_TRUE(move(mime.get(), body(c1)));
    EXPECT_EQ(node(c1)->dropFeedback(), Feedback::Valid);
    EXPECT_NE(drop(mime.get(), body(c2)), Qt::IgnoreAction);
    ASSERT_EQ(imports.size(), 1u);
    EXPECT_EQ(imports.front().value(QStringLiteral("target_container_id")).toInteger(), c2->objectID);
    EXPECT_EQ(imports.front().value(QStringLiteral("files")).toArray(),
        QJsonArray{directory.filePath(QStringLiteral("external.wav"))});
    EXPECT_EQ(node(c1)->dropFeedback(), Feedback::None);
    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
}

TEST_F(BlueprintDropInteractionTest, AudioInvalidNodeNeverFallsBackToRootOrStaleHover)
{
    showCanvas();
    auto mime = audio();
    auto* target = gainNode();
    ASSERT_TRUE(enter(mime.get(), body(c1)));
    ASSERT_TRUE(move(mime.get(), body(c1)));
    EXPECT_EQ(drop(mime.get(), body(target)), Qt::IgnoreAction);
    EXPECT_TRUE(imports.empty());
    enter(mime.get(), body(target));
    EXPECT_FALSE(move(mime.get(), body(target)));
    EXPECT_EQ(target->dropFeedback(), Feedback::None);
    leave();
    EXPECT_EQ(target->dropFeedback(), Feedback::None);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(BlueprintDropInteractionTest, AudioBlankDropUsesLayersRootInEveryCanvas)
{
    showCanvas();
    auto mime = audio();
    auto empty = blank();
    ASSERT_TRUE(enter(mime.get(), empty));
    EXPECT_TRUE(move(mime.get(), empty));
    EXPECT_EQ(drop(mime.get(), empty), Qt::CopyAction);
    ASSERT_EQ(imports.size(), 1u);
    EXPECT_FALSE(imports.back().contains(QStringLiteral("target_container_id")));
    EXPECT_FALSE(imports.back().contains(QStringLiteral("canvas_root_id")));
    canvas->useSignalChain(c1);
    canvas->zoomToFit();
    drainDropEvents();
    empty = blank();
    ASSERT_TRUE(enter(mime.get(), empty));
    ASSERT_TRUE(move(mime.get(), empty));
    EXPECT_EQ(drop(mime.get(), empty), Qt::CopyAction);
    ASSERT_EQ(imports.size(), 2u);
    EXPECT_FALSE(imports.back().contains(QStringLiteral("target_container_id")));
    EXPECT_FALSE(imports.back().contains(QStringLiteral("canvas_root_id")));
    EXPECT_EQ(imports.back().value(QStringLiteral("files")).toArray().size(), 1);
}

TEST_F(BlueprintDropInteractionTest, AudioAcceptsSoundSourcesAndBusRoutingTargets)
{
    showCanvas();
    auto mime = audio();
    for (auto* target : {s1, b3, master}) {
        ASSERT_TRUE(enter(mime.get(), body(target)));
        ASSERT_TRUE(move(mime.get(), body(target)));
        EXPECT_EQ(node(target)->dropFeedback(), Feedback::Valid);
        EXPECT_EQ(drop(mime.get(), body(target)), Qt::CopyAction);
        ASSERT_FALSE(imports.empty());
        EXPECT_EQ(imports.back().value(QStringLiteral("target_container_id")).toInteger(), target->objectID);
        EXPECT_EQ(node(target)->dropFeedback(), Feedback::None);
    }
    EXPECT_EQ(imports.size(), 3u);
}

TEST_F(BlueprintDropInteractionTest, RealAudioDropCreatesSelectedNodeAtDropPositionAndPreservesItAcrossUndo)
{
    showCanvas();
    auto mime = audio();
    auto& manager = ProjectManager::instance();
    registerAssetActions(ActionManager::instance(), nullptr, &manager);
    QUuid createdGuid;
    QObject::connect(&manager, &ProjectManager::itemCreated, canvas.get(),
        [&createdGuid](ProjectItem* item) {
            if (item->type == T::Sound) createdGuid = item->guid;
        });
    const auto target = blank();
    const auto position = canvas->mapToScene(target);
    ASSERT_TRUE(enter(mime.get(), target));
    ASSERT_TRUE(move(mime.get(), target));
    ASSERT_EQ(drop(mime.get(), target), Qt::CopyAction);
    drainDropEvents();
    ASSERT_FALSE(createdGuid.isNull());
    auto* sound = manager.findItemByGuid(createdGuid);
    ASSERT_NE(sound, nullptr);
    EXPECT_EQ(sound->type, T::Sound);
    EXPECT_EQ(sound->parent, nullptr);
    EXPECT_NE(sound->sourceID, 0u);
    EXPECT_EQ(node(sound)->pos(), position);
    EXPECT_TRUE(node(sound)->isSelected());
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainDropEvents();
    EXPECT_EQ(manager.findItemByGuid(createdGuid), nullptr);
    const auto& nodes = canvas->activeSignalPathSource()->graph().nodes;
    EXPECT_TRUE(std::none_of(nodes.begin(), nodes.end(), [&](const auto& entry) {
        return entry.key.objectGuid == createdGuid;
    }));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainDropEvents();
    sound = manager.findItemByGuid(createdGuid);
    ASSERT_NE(sound, nullptr);
    EXPECT_EQ(sound->parent, nullptr);
    EXPECT_EQ(node(sound)->pos(), position);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

TEST_F(BlueprintDropInteractionTest, ObjectAndAudioDropRejectPinsWithoutNodeFeedbackAndRevalidateRelease)
{
    showCanvas();
    auto objectMime = objects({s1->objectID});
    auto audioMime = audio();
    for (auto* mime : {objectMime.get(), audioMime.get()}) {
        for (const bool input : {true, false}) {
            for (const bool preview : {true, false}) {
                const auto original = node(s1)->pos();
                auto* pin = input ? node(c2)->inputPin() : node(c2)->outputPin();
                const auto target = canvas->mapFromScene(pin->scenePos());
                ASSERT_TRUE(enter(mime, body(c2)));
                ASSERT_TRUE(move(mime, body(c2)));
                EXPECT_EQ(node(c2)->dropFeedback(), Feedback::Valid);
                if (preview) {
                    EXPECT_FALSE(move(mime, target));
                    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
                    EXPECT_EQ(pin->connectState(), NodePin::ConnectState::None);
                    EXPECT_TRUE(move(mime, body(c2)));
                    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::Valid);
                    EXPECT_FALSE(move(mime, target));
                    EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
                }
                EXPECT_EQ(drop(mime, target), Qt::IgnoreAction);
                EXPECT_EQ(node(c2)->dropFeedback(), Feedback::None);
                EXPECT_EQ(node(s1)->pos(), original);
                EXPECT_EQ(s1->parent, c1);
                EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
                EXPECT_TRUE(imports.empty());
            }
        }
    }
}

}
