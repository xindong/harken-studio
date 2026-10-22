#include "ActionManager.h"
#include "ActionStepEditing.h"
#include "ActionStepUtils.h"
#include "AudioEngine.h"
#include "AuthoringPolicy.h"
#include "Components/VAction.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "IPC/IPCServer.h"
#include "NameValidator.h"
#include "ProjectManager.h"
#include "RuntimeSyncCoordinator.h"
#include "Timeline/TimelineEditing.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <utility>

namespace VNX {
uint32_t itemShortID(const ProjectItem* item);
}

namespace {

using namespace VNX;

class AbsoluteAuthoringAcceptanceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) ASSERT_TRUE(manager.closeProject());
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        ASSERT_TRUE(directory_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("AbsoluteAuthoringAcceptance");
        config.location = directory_.path();
        ASSERT_TRUE(manager.createProject(config));
        server_.setProjectManager(&manager);
        server_.setAppState(IPCServer::AppState::Ready);
        piece_ = manager.createItem(nullptr, QStringLiteral("AlignedPiece"),
                                    ProjectItem::Type::Piece);
        ASSERT_NE(piece_, nullptr);
        track_ = manager.createItem(piece_, QStringLiteral("ArrangedTrack"),
                                    ProjectItem::Type::Track);
        sound_ = manager.createItem(track_, QStringLiteral("IndependentSound"),
                                    ProjectItem::Type::Sound);
        container_ = manager.createItem(nullptr, QStringLiteral("TransitionContainer"),
                                        ProjectItem::Type::Container);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(container_, nullptr);
        Clip clip{QUuid::createUuid(), sound_->guid, 80000000, 240000000};
        ASSERT_TRUE(TimelineEditing::addClip(&manager, track_, clip).success);
        Marker marker{QUuid::createUuid(), MarkerKind::Custom,
                QStringLiteral("Accent"), 240000000};
        ASSERT_TRUE(TimelineEditing::addMarker(&manager, piece_, marker).success);
        ASSERT_TRUE(manager.saveProject());
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
             {QStringLiteral("property"), property}, {QStringLiteral("value"), value}},
            ActionSource::GUI);
    }

    QJsonObject dispatchSet(ProjectItem* item, const QString& property,
                            const QJsonValue& value, bool unrestricted)
    {
        const QJsonObject args{
            {QStringLiteral("object_id"), static_cast<qint64>(itemShortID(item))},
            {QStringLiteral("property"), property}, {QStringLiteral("value"), value}};
        if (!unrestricted)
            return server_.dispatchCommand(QStringLiteral("set_item_property"), args);
        IPCServer::IPCDispatchContext context;
        context.source = ActionSource::Internal;
        context.origin = IPCServer::DispatchOrigin::AI;
        context.requestId = QStringLiteral("absolute-authoring-acceptance");
        context.permissionProfile = QStringLiteral("ask");
        context.unrestricted = true;
        return server_.dispatchCommand(QStringLiteral("set_item_property"), args, context);
    }

    QByteArray readFile(const QString& path)
    {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
        return file.readAll();
    }

    QTemporaryDir directory_;
    IPCServer server_{nullptr};
    ProjectItem* piece_ = nullptr;
    ProjectItem* track_ = nullptr;
    ProjectItem* sound_ = nullptr;
    ProjectItem* container_ = nullptr;
};

TEST_F(AbsoluteAuthoringAcceptanceTest,
       GuiIpcAndUnrestrictedAiRejectMusicalWritesWithoutSideEffects)
{
    auto& manager = ProjectManager::instance();
    const QJsonObject pieceBefore = piece_->comp<VPiece>()->toJson();
    const QJsonObject trackBefore = track_->comp<VTrack>()->toJson();
    const auto revision = AudioEngine::instance().runtimeSyncCoordinator()
                              .currentAuthoringRevision();
    const int pending = manager.pendingResyncCount();
    const bool pieceDirty = piece_->isDirtyObject();
    const bool trackDirty = track_->isDirtyObject();
    int propertyChanges = 0;
    QObject observer;
    QObject::connect(&manager, &ProjectManager::itemPropertyChanged, &observer,
                     [&](ProjectItem*, const QString&, const QObject*) {
                         ++propertyChanges;
                     });

    for (const PieceTimeSpec& time : {PieceTimeSpec::musical(),
                                      PieceTimeSpec::musical(600000, 7, 8)}) {
        const ActionResult direct = TimelineEditing::replacePieceTime(
            &manager, piece_, time);
        ASSERT_FALSE(direct.success);
        EXPECT_EQ(direct.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
        const ActionResult gui = guiSet(piece_, QStringLiteral("pieceTime"), time.toJson());
        EXPECT_FALSE(gui.success);
        EXPECT_EQ(gui.errorCode, direct.errorCode);
        EXPECT_EQ(gui.errorMessage, direct.errorMessage);
        for (bool unrestricted : {false, true}) {
            const QJsonObject response = dispatchSet(
                piece_, QStringLiteral("pieceTime"), time.toJson(), unrestricted);
            EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
                      QStringLiteral("error"));
            EXPECT_EQ(response.value(QStringLiteral("code")).toString(), direct.errorCode);
            EXPECT_EQ(response.value(QStringLiteral("message")).toString(), direct.errorMessage);
        }
    }
    const QJsonObject musical = PieceTimeSpec::musical().toJson();
    for (const QString& map : {QStringLiteral("tempoMap"), QStringLiteral("meterMap")}) {
        QJsonObject disguised = PieceTimeSpec{}.toJson();
        disguised.insert(map, musical.value(map));
        const ActionResult gui = guiSet(piece_, QStringLiteral("pieceTime"), disguised);
        EXPECT_FALSE(gui.success);
        EXPECT_EQ(gui.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
        EXPECT_EQ(gui.errorMessage, AuthoringPolicy::musicalTimeRestriction());
        for (bool unrestricted : {false, true}) {
            const QJsonObject response = dispatchSet(
                piece_, QStringLiteral("pieceTime"), disguised, unrestricted);
            EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
                      QStringLiteral("error"));
            EXPECT_EQ(response.value(QStringLiteral("code")).toString(), gui.errorCode);
            EXPECT_EQ(response.value(QStringLiteral("message")).toString(), gui.errorMessage);
        }
    }
    EXPECT_EQ(piece_->comp<VPiece>()->toJson(), pieceBefore);
    EXPECT_EQ(track_->comp<VTrack>()->toJson(), trackBefore);
    EXPECT_EQ(piece_->isDirtyObject(), pieceDirty);
    EXPECT_EQ(track_->isDirtyObject(), trackDirty);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_EQ(manager.pendingResyncCount(), pending);
    EXPECT_EQ(AudioEngine::instance().runtimeSyncCoordinator().currentAuthoringRevision(), revision);
    EXPECT_EQ(propertyChanges, 0);
}

TEST_F(AbsoluteAuthoringAcceptanceTest,
       EveryCommandRouteRejectsNewBeatAndBarTransitionsWithoutMutation)
{
    const TransitionTableDef before = container_->comp<VSelection>()->transitions;
    const auto revision = AudioEngine::instance().runtimeSyncCoordinator()
                              .currentAuthoringRevision();
    const bool dirty = container_->isDirtyObject();
    for (TransitionExitTiming timing : {TransitionExitTiming::NextBeat,
                                        TransitionExitTiming::NextBar}) {
        TransitionTableDef replacement = before;
        ASSERT_FALSE(replacement.rules.empty());
        replacement.rules.front().exitTiming = timing;
        const ActionResult gui = guiSet(
            container_, QStringLiteral("transitions"), replacement.toJson());
        EXPECT_FALSE(gui.success);
        EXPECT_EQ(gui.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
        for (bool unrestricted : {false, true}) {
            const QJsonObject response = dispatchSet(
                container_, QStringLiteral("transitions"), replacement.toJson(), unrestricted);
            EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
                      QStringLiteral("error"));
            EXPECT_EQ(response.value(QStringLiteral("code")).toString(), gui.errorCode);
            EXPECT_EQ(response.value(QStringLiteral("message")).toString(), gui.errorMessage);
        }
    }
    EXPECT_EQ(container_->comp<VSelection>()->transitions, before);
    EXPECT_EQ(container_->isDirtyObject(), dirty);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_EQ(AudioEngine::instance().runtimeSyncCoordinator().currentAuthoringRevision(), revision);
}

TEST_F(AbsoluteAuthoringAcceptanceTest,
       UnresolvedActionNamesCannotActivateMusicalDependenciesAfterRename)
{
    auto& manager = ProjectManager::instance();
    auto* action = manager.createItem(nullptr, QStringLiteral("NamedAction"),
                                      ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    const QJsonObject safeStep{
        {QStringLiteral("type"), QStringLiteral("Play")},
        {QStringLiteral("targetObjectID"), static_cast<qint64>(sound_->objectID)},
        {QStringLiteral("targetName"), sound_->name}};
    ASSERT_TRUE(guiSet(action, QStringLiteral("steps"), QJsonArray{safeStep}).success);
    piece_->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    piece_->markDirty();
    ASSERT_TRUE(manager.saveProject());
    UndoRedoManager::instance().clear();
    const auto before = ActionStepEditing::stepsToJson(action->comp<VAction>()->steps);
    const auto revision = AudioEngine::instance().runtimeSyncCoordinator()
                              .currentAuthoringRevision();
    const int pending = manager.pendingResyncCount();
    const bool dirty = action->isDirtyObject();
    int changes = 0;
    QObject observer;
    QObject::connect(&manager, &ProjectManager::itemPropertyChanged, &observer,
                     [&](ProjectItem*, const QString&, const QObject*) { ++changes; });
    const QString futureName = QStringLiteral("FutureMusical");
    for (bool reference : {false, true}) {
        for (bool existingName : {false, true}) {
            QJsonObject candidate = safeStep;
            const QString name = existingName ? piece_->name : futureName;
            if (reference) {
                candidate[QStringLiteral("quantize")] = QJsonObject{
                    {QStringLiteral("timing"), QStringLiteral("NextMarker")},
                    {QStringLiteral("referenceObjectID"), 0},
                    {QStringLiteral("referenceName"), name}};
            } else {
                candidate[QStringLiteral("targetObjectID")] = 0;
                candidate[QStringLiteral("targetName")] = name;
            }
            const QJsonArray replacement{safeStep, candidate};
            const auto gui = guiSet(action, QStringLiteral("steps"), replacement);
            ASSERT_FALSE(gui.success);
            EXPECT_EQ(gui.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
            for (bool unrestricted : {false, true}) {
                const auto response = dispatchSet(
                    action, QStringLiteral("steps"), replacement, unrestricted);
                EXPECT_EQ(response.value(QStringLiteral("status")).toString(),
                          QStringLiteral("error"));
                EXPECT_EQ(response.value(QStringLiteral("code")).toString(), gui.errorCode);
                EXPECT_EQ(response.value(QStringLiteral("message")).toString(), gui.errorMessage);
            }
        }
    }
    QJsonObject staleTarget = safeStep;
    staleTarget[QStringLiteral("targetObjectID")] = static_cast<qint64>(UINT32_MAX);
    staleTarget[QStringLiteral("targetName")] = piece_->name;
    const auto stale = guiSet(action, QStringLiteral("steps"), QJsonArray{staleTarget});
    EXPECT_FALSE(stale.success);
    EXPECT_EQ(stale.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    QJsonObject inactive = safeStep;
    inactive[QStringLiteral("quantize")] = QJsonObject{
        {QStringLiteral("timing"), QStringLiteral("Immediate")},
        {QStringLiteral("referenceObjectID"), 0},
        {QStringLiteral("referenceName"), futureName}};
    EXPECT_FALSE(guiSet(action, QStringLiteral("steps"), QJsonArray{inactive}).success);
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), before);
    EXPECT_EQ(action->isDirtyObject(), dirty);
    EXPECT_EQ(manager.pendingResyncCount(), pending);
    EXPECT_EQ(AudioEngine::instance().runtimeSyncCoordinator().currentAuthoringRevision(), revision);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_EQ(changes, 0);
    ASSERT_TRUE(manager.renameItem(piece_, futureName));
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), before);
    const auto lookup = EditorUtils::makeProjectTargetLookup(*manager.currentProject());
    const auto& retained = action->comp<VAction>()->steps.front();
    EXPECT_EQ(EditorUtils::resolveActionStepTarget(retained.type, retained.targetObjectID,
                  retained.targetName, lookup, action->objectID).objectID, sound_->objectID);
}

TEST_F(AbsoluteAuthoringAcceptanceTest,
       ResolvedActionNamesStayAnchoredAcrossUndoAndNameReuse)
{
    auto& manager = ProjectManager::instance();
    auto* action = manager.createItem(nullptr, QStringLiteral("AnchoredAction"),
                                      ProjectItem::Type::Action);
    auto* legacyPiece = manager.createItem(nullptr, QStringLiteral("LegacyPiece"),
                                           ProjectItem::Type::Piece);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(legacyPiece, nullptr);
    auto* legacyTrack = manager.createItem(legacyPiece, QStringLiteral("LegacyTrack"),
                                           ProjectItem::Type::Track);
    ASSERT_NE(legacyTrack, nullptr);
    legacyPiece->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    UndoRedoManager::instance().clear();
    const QString targetName = sound_->name;
    const QString referenceName = piece_->name;
    const QJsonObject step{
        {QStringLiteral("type"), QStringLiteral("Play")},
        {QStringLiteral("targetName"), targetName},
        {QStringLiteral("quantize"), QJsonObject{
            {QStringLiteral("timing"), QStringLiteral("NextMarker")},
            {QStringLiteral("referenceName"), referenceName}}}};
    const auto result = guiSet(action, QStringLiteral("steps"), QJsonArray{step});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    auto* undo = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(undo->count(), 1);
    undo->undo();
    EXPECT_TRUE(action->comp<VAction>()->steps.empty());
    undo->redo();
    ASSERT_EQ(action->comp<VAction>()->steps.size(), 1u);
    EXPECT_EQ(action->comp<VAction>()->steps.front().targetObjectID, sound_->objectID);
    EXPECT_EQ(action->comp<VAction>()->steps.front().quantize.referenceObjectID, piece_->objectID);
    ASSERT_TRUE(manager.renameItem(sound_, QStringLiteral("RenamedSound")));
    ASSERT_TRUE(manager.renameItem(piece_, QStringLiteral("RenamedPiece")));
    ASSERT_TRUE(manager.renameItem(legacyPiece, targetName));
    ASSERT_TRUE(manager.renameItem(legacyTrack, referenceName));
    const auto& anchored = action->comp<VAction>()->steps.front();
    EXPECT_EQ(anchored.targetObjectID, sound_->objectID);
    EXPECT_EQ(anchored.targetName, sound_->name);
    EXPECT_EQ(anchored.quantize.referenceObjectID, piece_->objectID);
    EXPECT_EQ(anchored.quantize.referenceName, piece_->name);
    EXPECT_TRUE(AuthoringPolicy::validateNewSubtree(*action, nullptr,
        [&](const QUuid& guid) { return manager.findItemByGuid(guid); },
        [&](uint32_t id) { return manager.findItemByObjectID(id); }));
}

TEST_F(AbsoluteAuthoringAcceptanceTest,
       LegacyUnresolvedStepsRemainEditableButCannotBecomeNewDependencies)
{
    auto& manager = ProjectManager::instance();
    auto* legacy = manager.createItem(nullptr, QStringLiteral("LegacyAction"),
                                      ProjectItem::Type::Action);
    auto* wrapper = manager.createItem(nullptr, QStringLiteral("WrapperAction"),
                                       ProjectItem::Type::Action);
    ASSERT_NE(legacy, nullptr);
    ASSERT_NE(wrapper, nullptr);
    VActionStep unresolved;
    unresolved.type = VActionStep::Type::Play;
    unresolved.targetName = QStringLiteral("MissingLegacyTarget");
    VActionStep musical = unresolved;
    musical.targetName = piece_->name;
    piece_->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    legacy->comp<VAction>()->steps = {unresolved, musical};
    const auto original = ActionStepEditing::stepsToJson(legacy->comp<VAction>()->steps);
    UndoRedoManager::instance().clear();
    const QJsonObject safeStep{
        {QStringLiteral("type"), QStringLiteral("Play")},
        {QStringLiteral("targetObjectID"), static_cast<qint64>(sound_->objectID)},
        {QStringLiteral("targetName"), sound_->name}};
    QJsonArray replacement = original;
    replacement.append(safeStep);
    auto result = guiSet(legacy, QStringLiteral("steps"), replacement);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    auto* undo = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(undo->count(), 1);
    undo->undo();
    EXPECT_EQ(ActionStepEditing::stepsToJson(legacy->comp<VAction>()->steps), original);
    undo->redo();
    const auto retained = ActionStepEditing::stepsToJson(legacy->comp<VAction>()->steps);
    EXPECT_EQ(retained.first(), original.first());
    EXPECT_EQ(retained.at(1), original.at(1));
    QJsonArray duplicated = retained;
    duplicated.append(original.first());
    result = guiSet(legacy, QStringLiteral("steps"), duplicated);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    EXPECT_EQ(ActionStepEditing::stepsToJson(legacy->comp<VAction>()->steps), retained);
    EXPECT_EQ(undo->count(), 1);
    int creations = 0;
    QObject observer;
    QObject::connect(&manager, &ProjectManager::itemCreated, &observer,
                     [&](ProjectItem*) { ++creations; });
    EXPECT_EQ(manager.duplicateItem(legacy), nullptr);
    EXPECT_EQ(creations, 0);
    EXPECT_EQ(undo->count(), 1);
    const QJsonObject post{
        {QStringLiteral("type"), QStringLiteral("PostAction")},
        {QStringLiteral("targetObjectID"), static_cast<qint64>(legacy->objectID)},
        {QStringLiteral("targetName"), legacy->name}};
    result = guiSet(wrapper, QStringLiteral("steps"), QJsonArray{post});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    EXPECT_TRUE(wrapper->comp<VAction>()->steps.empty());
    EXPECT_EQ(undo->count(), 1);
    result = guiSet(legacy, QStringLiteral("steps"), QJsonArray{safeStep});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    result = guiSet(wrapper, QStringLiteral("steps"), QJsonArray{post});
    EXPECT_TRUE(result.success) << result.errorMessage.toStdString();
}

TEST_F(AbsoluteAuthoringAcceptanceTest,
       DeletedActionTargetsCannotRebindAndUndoRestoresTheirIdentity)
{
    auto& manager = ProjectManager::instance();
    auto* action = manager.createItem(nullptr, QStringLiteral("DeleteTargetAction"),
                                      ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    const QJsonObject play{
        {QStringLiteral("type"), QStringLiteral("Play")},
        {QStringLiteral("targetName"), sound_->name},
        {QStringLiteral("delay"), 12.0}};
    QJsonObject stop = play;
    stop[QStringLiteral("type")] = QStringLiteral("Stop");
    stop[QStringLiteral("fadeTime")] = 24.0;
    stop[QStringLiteral("quantize")] = QJsonObject{
        {QStringLiteral("timing"), QStringLiteral("NextMarker")},
        {QStringLiteral("referenceName"), container_->name},
        {QStringLiteral("ifSilent"), QStringLiteral("Drop")}};
    ASSERT_TRUE(guiSet(action, QStringLiteral("steps"), QJsonArray{play, stop}).success);
    piece_->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    piece_->markDirty();
    ASSERT_TRUE(manager.saveProject());
    UndoRedoManager::instance().clear();
    const auto original = action->comp<VAction>()->steps;
    auto cleared = original;
    for (auto& step : cleared) {
        step.targetObjectID = 0;
        step.targetName.clear();
    }
    const auto before = ActionStepEditing::stepsToJson(original);
    const auto after = ActionStepEditing::stepsToJson(cleared);
    const QUuid actionGuid = action->guid;
    const QUuid soundGuid = sound_->guid;
    const QUuid containerGuid = container_->guid;
    const QString targetName = sound_->name;
    const QString projectPath = manager.currentProject()->projectPath;
    int stepChanges = 0;
    bool clearedBeforeDeletionSignal = false;
    QObject observer;
    QObject::connect(&manager, &ProjectManager::itemPropertyChanged, &observer,
        [&](ProjectItem* item, const QString& property, const QObject*) {
            if (item == action && property == QStringLiteral("steps")) ++stepChanges;
        });
    QObject::connect(&manager, &ProjectManager::itemDeleted, &observer,
        [&](ProjectItem* item) {
            if (item->guid == soundGuid)
                clearedBeforeDeletionSignal =
                    ActionStepEditing::stepsToJson(action->comp<VAction>()->steps) == after;
        });
    ASSERT_TRUE(manager.deleteItem(sound_));
    EXPECT_EQ(manager.findItemByGuid(soundGuid), nullptr);
    EXPECT_TRUE(clearedBeforeDeletionSignal);
    EXPECT_TRUE(action->isDirtyObject());
    EXPECT_EQ(stepChanges, 1);
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), after);
    auto* undo = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(undo->count(), 1);
    undo->undo();
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), before);
    EXPECT_EQ(manager.findItemByGuid(soundGuid), sound_);
    undo->redo();
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), after);
    ASSERT_TRUE(manager.renameItem(piece_, targetName));
    const auto lookup = EditorUtils::makeProjectTargetLookup(*manager.currentProject());
    for (const auto& step : action->comp<VAction>()->steps)
        EXPECT_EQ(EditorUtils::resolveActionStepTarget(step.type, step.targetObjectID,
                      step.targetName, lookup, action->objectID).state,
                  EditorUtils::ActionTargetState::Empty);
    ASSERT_TRUE(manager.saveProject());
    QObject::disconnect(&manager, nullptr, &observer, nullptr);
    ASSERT_TRUE(manager.closeProject());
    QString error;
    ASSERT_TRUE(manager.loadProject(projectPath, false, &error)) << error.toStdString();
    action = manager.findItemByGuid(actionGuid);
    ASSERT_NE(action, nullptr);
    ASSERT_EQ(action->comp<VAction>()->steps.size(), 2u);
    for (const auto& step : action->comp<VAction>()->steps) {
        EXPECT_EQ(step.targetObjectID, 0u);
        EXPECT_TRUE(step.targetName.isEmpty());
    }
    EXPECT_EQ(manager.findItemByGuid(soundGuid), nullptr);
    auto* clock = manager.findItemByGuid(containerGuid);
    ASSERT_NE(clock, nullptr);
    EXPECT_EQ(action->comp<VAction>()->steps[1].quantize.referenceObjectID, clock->objectID);
    EXPECT_FLOAT_EQ(action->comp<VAction>()->steps[1].fadeTime, 24.0f);
}

TEST_F(AbsoluteAuthoringAcceptanceTest,
       FolderDeletionRestoresReferencesAndIdentityAcrossSaves)
{
    auto& manager = ProjectManager::instance();
    auto* folder = manager.createFolder(nullptr, QStringLiteral("DeletedTargets"), QStringLiteral("Actions"));
    ASSERT_NE(folder, nullptr);
    const QString folderPath = manager.currentProject()->folderRelPath(folder);
    auto* layer = manager.createItem(nullptr, QStringLiteral("DeletedLayer"),
                                     ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* nested = manager.createItem(layer, QStringLiteral("NestedTarget"),
                                      ProjectItem::Type::Sound);
    auto* root = manager.createItem(nullptr, QStringLiteral("RootTarget"),
                                    ProjectItem::Type::Action, folderPath);
    auto* otherRoot = manager.createItem(nullptr, QStringLiteral("OtherRootTarget"),
                                         ProjectItem::Type::Action, folderPath);
    auto* action = manager.createItem(nullptr, QStringLiteral("FolderDeleteAction"),
                                      ProjectItem::Type::Action);
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(otherRoot, nullptr);
    ASSERT_NE(action, nullptr);
    QJsonArray steps;
    for (const auto* target : {nested, sound_, root, otherRoot, nested})
        steps.append(QJsonObject{
            {QStringLiteral("type"), target->type == ProjectItem::Type::Action
                                        ? QStringLiteral("PostAction") : QStringLiteral("Play")},
            {QStringLiteral("targetObjectID"), static_cast<qint64>(target->objectID)},
            {QStringLiteral("targetName"), target->name}});
    ASSERT_TRUE(guiSet(action, QStringLiteral("steps"), steps).success);
    const auto before = ActionStepEditing::stepsToJson(action->comp<VAction>()->steps);
    auto cleared = action->comp<VAction>()->steps;
    for (size_t index : {0u, 2u, 3u, 4u}) {
        cleared[index].targetObjectID = 0;
        cleared[index].targetName.clear();
    }
    const auto after = ActionStepEditing::stepsToJson(cleared);
    const QUuid nestedGuid = nested->guid;
    const QUuid rootGuid = root->guid;
    const QUuid otherRootGuid = otherRoot->guid;
    const QUuid actionGuid = action->guid;
    const QUuid survivingSoundGuid = sound_->guid;
    const QString projectPath = manager.currentProject()->projectPath;
    const QString absoluteFolderPath = QDir(QFileInfo(projectPath).absolutePath())
        .filePath(QStringLiteral("Actions/") + folderPath);
    ASSERT_TRUE(manager.saveProject());
    UndoRedoManager::instance().clear();
    ASSERT_TRUE(manager.deleteEntries({layer}, {}));
    EXPECT_EQ(manager.findItemByGuid(nestedGuid), nullptr);
    EXPECT_EQ(action->comp<VAction>()->steps[0].targetObjectID, 0u);
    EXPECT_EQ(action->comp<VAction>()->steps[4].targetObjectID, 0u);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(manager.findItemByGuid(nestedGuid), nested);
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), before);
    ASSERT_TRUE(manager.deleteEntries({layer}, {folder}));
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), after);
    EXPECT_EQ(manager.findItemByGuid(nestedGuid), nullptr);
    EXPECT_EQ(manager.findItemByGuid(rootGuid), nullptr);
    EXPECT_EQ(manager.findItemByGuid(otherRootGuid), nullptr);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_TRUE(QFileInfo(absoluteFolderPath).isDir());
    ASSERT_TRUE(manager.saveProject());
    EXPECT_FALSE(QFileInfo::exists(absoluteFolderPath));
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(manager.findItemByGuid(nestedGuid), nested);
    EXPECT_EQ(manager.findItemByGuid(rootGuid), root);
    EXPECT_EQ(manager.findItemByGuid(otherRootGuid), otherRoot);
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), before);
    EXPECT_FALSE(QFileInfo::exists(absoluteFolderPath));
    ASSERT_TRUE(manager.saveProject());
    EXPECT_TRUE(QFileInfo(absoluteFolderPath).isDir());
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), after);
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    action = manager.findItemByGuid(actionGuid);
    auto* survivingSound = manager.findItemByGuid(survivingSoundGuid);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(survivingSound, nullptr);
    ASSERT_EQ(action->comp<VAction>()->steps.size(), 5u);
    for (size_t index : {0u, 2u, 3u, 4u}) {
        EXPECT_EQ(action->comp<VAction>()->steps[index].targetObjectID, 0u);
        EXPECT_TRUE(action->comp<VAction>()->steps[index].targetName.isEmpty());
    }
    EXPECT_EQ(action->comp<VAction>()->steps[1].targetObjectID, survivingSound->objectID);
}

TEST_F(AbsoluteAuthoringAcceptanceTest,
       BatchDeletionRestoresActionAndEveryTargetTypeInEitherOrder)
{
    auto& manager = ProjectManager::instance();
    const std::vector<std::pair<ProjectItem::Type, QString>> cases{
        {ProjectItem::Type::Sound, QStringLiteral("Play")},
        {ProjectItem::Type::Track, QStringLiteral("Play")},
        {ProjectItem::Type::Piece, QStringLiteral("Play")},
        {ProjectItem::Type::Variable, QStringLiteral("SetVariable")},
        {ProjectItem::Type::Action, QStringLiteral("PostAction")},
        {ProjectItem::Type::Container, QStringLiteral("Reset")}};
    for (const auto& [type, verb] : cases) {
        for (bool actionFirst : {false, true}) {
            const QString suffix = QString::number(static_cast<int>(type)) + verb
                + (actionFirst ? QStringLiteral("First") : QStringLiteral("Last"));
            auto* target = manager.createItem(nullptr, QStringLiteral("BatchTarget") + suffix, type);
            auto* action = manager.createItem(nullptr, QStringLiteral("BatchOwner") + suffix,
                                              ProjectItem::Type::Action);
            ASSERT_NE(target, nullptr);
            ASSERT_NE(action, nullptr);
            const QJsonObject step{
                {QStringLiteral("type"), verb},
                {QStringLiteral("targetObjectID"), static_cast<qint64>(target->objectID)},
                {QStringLiteral("targetName"), target->name}};
            ASSERT_TRUE(guiSet(action, QStringLiteral("steps"), QJsonArray{step}).success);
            const auto before = ActionStepEditing::stepsToJson(action->comp<VAction>()->steps);
            UndoRedoManager::instance().clear();
            const QList<ProjectItem*> order = actionFirst
                ? QList<ProjectItem*>{action, target} : QList<ProjectItem*>{target, action};
            ASSERT_EQ(manager.deleteItems(order), 2);
            EXPECT_EQ(manager.findItemByGuid(target->guid), nullptr);
            EXPECT_EQ(manager.findItemByGuid(action->guid), nullptr);
            auto* undo = UndoRedoManager::instance().undoStack();
            ASSERT_EQ(undo->count(), 1);
            for (int iteration = 0; iteration < 2; ++iteration) {
                undo->undo();
                EXPECT_EQ(manager.findItemByGuid(target->guid), target);
                EXPECT_EQ(manager.findItemByGuid(action->guid), action);
                EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), before);
                undo->redo();
                EXPECT_EQ(manager.findItemByGuid(target->guid), nullptr);
                EXPECT_EQ(manager.findItemByGuid(action->guid), nullptr);
            }
        }
    }
}

TEST_F(AbsoluteAuthoringAcceptanceTest,
       DeletionPreservesLegacyFallbackAndRestoresExplicitQuantizeReferenceOnUndo)
{
    auto& manager = ProjectManager::instance();
    auto* bus = manager.createItem(manager.getMasterBusItem(), QStringLiteral("DeletedBus"),
                                   ProjectItem::Type::Bus);
    auto* action = manager.createItem(nullptr, QStringLiteral("LegacyDeleteAction"),
                                      ProjectItem::Type::Action);
    ASSERT_NE(bus, nullptr);
    ASSERT_NE(action, nullptr);
    VActionStep nameOnly;
    nameOnly.targetName = sound_->name;
    VActionStep incompatible = nameOnly;
    incompatible.targetObjectID = bus->objectID;
    VActionStep bound = nameOnly;
    bound.targetObjectID = sound_->objectID;
    bound.quantize.referenceObjectID = sound_->objectID;
    bound.quantize.referenceName = sound_->name;
    bound.quantize.timing = QuantizeTiming::NextMarker;
    action->comp<VAction>()->steps = {nameOnly, incompatible, bound};
    const auto before = ActionStepEditing::stepsToJson(action->comp<VAction>()->steps);
    action->dirtyBit = false;
    UndoRedoManager::instance().clear();
    ASSERT_TRUE(manager.deleteItem(bus));
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), before);
    EXPECT_FALSE(action->isDirtyObject());
    const auto lookup = EditorUtils::makeProjectTargetLookup(*manager.currentProject());
    EXPECT_EQ(EditorUtils::resolveActionStepTarget(incompatible.type,
                  incompatible.targetObjectID, incompatible.targetName, lookup).state,
              EditorUtils::ActionTargetState::Dangling);
    ASSERT_TRUE(manager.deleteItem(sound_));
    const auto after = ActionStepEditing::stepsToJson(action->comp<VAction>()->steps);
    EXPECT_EQ(after.at(0), before.at(0));
    EXPECT_EQ(after.at(1), before.at(1));
    const auto& cleared = action->comp<VAction>()->steps[2];
    EXPECT_EQ(cleared.targetObjectID, 0u);
    EXPECT_TRUE(cleared.targetName.isEmpty());
    EXPECT_EQ(cleared.quantize.referenceObjectID, 0u);
    EXPECT_TRUE(cleared.quantize.referenceName.isEmpty());
    EXPECT_EQ(cleared.quantize.timing, bound.quantize.timing);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), before);
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), after);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(ActionStepEditing::stepsToJson(action->comp<VAction>()->steps), before);
}

TEST_F(AbsoluteAuthoringAcceptanceTest,
       LegacyMusicalDocumentsSurviveUnrelatedEditingAndTwoReopensExactly)
{
    auto& manager = ProjectManager::instance();
    auto* pieceTimeline = piece_->comp<VPiece>();
    auto* trackTimeline = track_->comp<VTrack>();
    pieceTimeline->pieceTime = PieceTimeSpec::musical(600000, 7, 8);
    pieceTimeline->markers.items.front().position = 1920;
    trackTimeline->clips.items.front().start = 960;
    trackTimeline->clips.items.front().duration = 2880;
    container_->comp<VSelection>()->transitions.rules.front().exitTiming =
        TransitionExitTiming::NextBar;
    piece_->markDirty();
    track_->markDirty();
    container_->markDirty();
    ASSERT_TRUE(manager.saveProject());

    const QJsonObject expectedPiece = pieceTimeline->toJson();
    const QJsonObject expectedTrack = trackTimeline->toJson();
    const TransitionTableDef expectedTransitions = container_->comp<VSelection>()->transitions;
    const QUuid pieceGuid = piece_->guid;
    const QUuid trackGuid = track_->guid;
    const QUuid soundGuid = sound_->guid;
    const QUuid containerGuid = container_->guid;
    const QString projectPath = manager.currentProject()->projectPath;
    const QString piecePath = QDir(QFileInfo(projectPath).absolutePath())
                                  .filePath(QStringLiteral("Layers/") + NameValidator::objectFileName(piece_->name, pieceGuid));
    const QString transitionPath = QDir(QFileInfo(projectPath).absolutePath())
                                       .filePath(QStringLiteral("Layers/") + NameValidator::objectFileName(container_->name, containerGuid));
    const QByteArray originalDocument = readFile(piecePath);
    const QByteArray originalTransitionDocument = readFile(transitionPath);
    ASSERT_FALSE(originalDocument.isEmpty());
    ASSERT_FALSE(originalTransitionDocument.isEmpty());

    for (int iteration = 0; iteration < 2; ++iteration) {
        ASSERT_TRUE(manager.closeProject());
        QString error;
        ASSERT_TRUE(manager.loadProject(projectPath, false, &error)) << error.toStdString();
        piece_ = manager.findItemByGuid(pieceGuid);
        track_ = manager.findItemByGuid(trackGuid);
        sound_ = manager.findItemByGuid(soundGuid);
        container_ = manager.findItemByGuid(containerGuid);
        ASSERT_NE(piece_, nullptr);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(container_, nullptr);
        EXPECT_EQ(piece_->comp<VPiece>()->toJson(), expectedPiece);
        EXPECT_EQ(track_->comp<VTrack>()->toJson(), expectedTrack);
        EXPECT_EQ(container_->comp<VSelection>()->transitions, expectedTransitions);
        const ActionResult edit = guiSet(sound_, QStringLiteral("volume"), -3.0 - iteration);
        ASSERT_TRUE(edit.success) << edit.errorMessage.toStdString();
        const ActionResult prohibited = TimelineEditing::moveClip(
            &manager, track_, track_->comp<VTrack>()->clips.items.front().clipGuid, 1920);
        EXPECT_FALSE(prohibited.success);
        EXPECT_EQ(prohibited.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
        EXPECT_FALSE(TimelineEditing::replacePieceTime(&manager, piece_, PieceTimeSpec{}).success);
        ASSERT_TRUE(manager.saveProject());
        EXPECT_EQ(readFile(piecePath), originalDocument);
        EXPECT_EQ(readFile(transitionPath), originalTransitionDocument);
        EXPECT_EQ(piece_->comp<VPiece>()->toJson(), expectedPiece);
        EXPECT_EQ(track_->comp<VTrack>()->toJson(), expectedTrack);
        EXPECT_EQ(container_->comp<VSelection>()->transitions, expectedTransitions);
    }
}

}
