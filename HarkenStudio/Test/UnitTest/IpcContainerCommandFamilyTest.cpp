#include "IPC/IPCServer.h"
#include "ActionManager.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "PlayablePropertyAuthoringV1.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "Components/VSpatial.h"
#include "Timeline/TimelineEditing.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <utility>

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

int countItems(const ProjectItem* item)
{
    int n = 1;
    for (const auto& child : item->children)
        n += countItems(child.get());
    return n;
}

int totalProjectItems(ProjectData* project)
{
    int n = 0;
    for (const auto* binding : project->allFileBindings())
    {
        if (binding->rootItem)
            n += countItems(binding->rootItem.get());
    }
    return n;
}

QJsonObject pairArgs(const ProjectItem* container, const ProjectItem* child)
{
    return QJsonObject{
        {QStringLiteral("container_id"), static_cast<qint64>(itemShortID(container))},
        {QStringLiteral("child_id"), static_cast<qint64>(itemShortID(child))}
    };
}

const SelectionOccurrence* occurrenceFor(const ProjectItem* container,
                                        const QUuid& childGuid)
{
    if (!container) return nullptr;
    const auto* member = container->comp<VSelection>()->selections.shared.member(childGuid);
    return member && !member->occurrences.empty() ? &member->occurrences.front() : nullptr;
}

class IpcContainerCommandFamilyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(),
                               &ProjectManager::instance(), nullptr, {});
        registerContainerActions(ActionManager::instance(),
                                 &ProjectManager::instance());

        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ContainerFamilyProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        UndoRedoManager::instance().clear();

        auto& pm = ProjectManager::instance();
        layer_ = pm.createItem(nullptr, QStringLiteral("Fam_Layer"), ProjectItem::Type::Layer);
        ASSERT_NE(layer_, nullptr);
        container_ = pm.createItem(layer_, QStringLiteral("Fam_Container"), ProjectItem::Type::Container);
        track_ = pm.createItem(layer_, QStringLiteral("Fam_Track"), ProjectItem::Type::Track);
        piece_ = pm.createItem(layer_, QStringLiteral("Fam_Piece"), ProjectItem::Type::Piece);
        soundA_ = pm.createItem(layer_, QStringLiteral("Fam_SoundA"), ProjectItem::Type::Sound);
        soundB_ = pm.createItem(layer_, QStringLiteral("Fam_SoundB"), ProjectItem::Type::Sound);
        soundC_ = pm.createItem(layer_, QStringLiteral("Fam_SoundC"), ProjectItem::Type::Sound);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(track_, nullptr);
        ASSERT_NE(piece_, nullptr);
        ASSERT_NE(soundA_, nullptr);
        ASSERT_NE(soundB_, nullptr);
        ASSERT_NE(soundC_, nullptr);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ActionResult run(const QString& cmd, const QJsonObject& args)
    {
        return ActionManager::instance().execute(cmd, args, ActionSource::IPC);
    }

    QTemporaryDir tempDir_;
    ProjectItem* layer_ = nullptr;
    ProjectItem* container_ = nullptr;
    ProjectItem* track_ = nullptr;
    ProjectItem* piece_ = nullptr;
    ProjectItem* soundA_ = nullptr;
    ProjectItem* soundB_ = nullptr;
    ProjectItem* soundC_ = nullptr;
};

} // namespace

TEST_F(IpcContainerCommandFamilyTest, ContainerFamilyCommandsCoverNewTypesWithUndoRoundtrip)
{
    auto* undoStack = UndoRedoManager::instance().undoStack();

    ActionResult r = run(QStringLiteral("add_child_to_container"), pairArgs(container_, soundA_));
    ASSERT_TRUE(r.success) << r.errorCode.toStdString();
    EXPECT_EQ(soundA_->parent, container_);

    undoStack->undo();
    EXPECT_EQ(soundA_->parent, layer_);
    undoStack->redo();
    EXPECT_EQ(soundA_->parent, container_);

    r = run(QStringLiteral("add_child_to_container"), pairArgs(container_, soundB_));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(soundB_->parent, container_);
    ASSERT_NE(occurrenceFor(container_, soundA_->guid), nullptr);
    ASSERT_NE(occurrenceFor(container_, soundB_->guid), nullptr);
    const uint32_t soundAOrdinal =
        occurrenceFor(container_, soundA_->guid)->stableOccurrenceOrdinal;
    const uint32_t soundBOrdinal =
        occurrenceFor(container_, soundB_->guid)->stableOccurrenceOrdinal;

    r = run(QStringLiteral("add_child_to_container"), pairArgs(track_, soundC_));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(soundC_->parent, track_);

    r = run(QStringLiteral("add_child_to_container"), pairArgs(piece_, track_));
    ASSERT_TRUE(r.success);
    EXPECT_EQ(track_->parent, piece_);

    QJsonObject reorderArgs{
        {QStringLiteral("container_id"), static_cast<qint64>(itemShortID(container_))},
        {QStringLiteral("child_order_ids"), QJsonArray{
            static_cast<qint64>(itemShortID(soundB_)),
            static_cast<qint64>(itemShortID(soundA_))
        }}
    };
    r = run(QStringLiteral("reorder_container_children"), reorderArgs);
    ASSERT_TRUE(r.success);
    ASSERT_EQ(container_->children.size(), 2u);
    EXPECT_EQ(container_->children[0].get(), soundB_);
    EXPECT_EQ(container_->children[1].get(), soundA_);
    EXPECT_EQ(occurrenceFor(container_, soundA_->guid)->stableOccurrenceOrdinal,
              soundAOrdinal);
    EXPECT_EQ(occurrenceFor(container_, soundB_->guid)->stableOccurrenceOrdinal,
              soundBOrdinal);

    undoStack->undo();
    EXPECT_EQ(container_->children[0].get(), soundA_);
    EXPECT_EQ(container_->children[1].get(), soundB_);
    EXPECT_EQ(occurrenceFor(container_, soundA_->guid)->stableOccurrenceOrdinal,
              soundAOrdinal);
    EXPECT_EQ(occurrenceFor(container_, soundB_->guid)->stableOccurrenceOrdinal,
              soundBOrdinal);
    undoStack->redo();
    EXPECT_EQ(container_->children[0].get(), soundB_);
    EXPECT_EQ(container_->children[1].get(), soundA_);
    EXPECT_EQ(occurrenceFor(container_, soundA_->guid)->stableOccurrenceOrdinal,
              soundAOrdinal);
    EXPECT_EQ(occurrenceFor(container_, soundB_->guid)->stableOccurrenceOrdinal,
              soundBOrdinal);

    QJsonObject propArgs{
        {QStringLiteral("container_id"), static_cast<qint64>(itemShortID(container_))},
        {QStringLiteral("property"), QStringLiteral("volume")},
        {QStringLiteral("value"), 0.5}
    };
    r = run(QStringLiteral("set_container_property"), propArgs);
    ASSERT_TRUE(r.success) << r.errorCode.toStdString();
    auto* mix = container_->comp<VMixNode>();
    ASSERT_NE(mix, nullptr);
    const float beforeUndo = mix->volume;
    EXPECT_FLOAT_EQ(beforeUndo, 0.5f);

    undoStack->undo();
    EXPECT_NE(mix->volume, 0.5f);
    undoStack->redo();
    EXPECT_FLOAT_EQ(mix->volume, 0.5f);

    r = run(QStringLiteral("remove_child_from_container"), pairArgs(container_, soundA_));
    ASSERT_TRUE(r.success) << r.errorCode.toStdString();
    EXPECT_EQ(soundA_->parent, layer_);

    undoStack->undo();
    EXPECT_EQ(soundA_->parent, container_);
    ASSERT_NE(occurrenceFor(container_, soundA_->guid), nullptr);
    EXPECT_EQ(occurrenceFor(container_, soundA_->guid)->stableOccurrenceOrdinal,
              soundAOrdinal);
    undoStack->redo();
    EXPECT_EQ(soundA_->parent, layer_);
    EXPECT_EQ(occurrenceFor(container_, soundA_->guid), nullptr);
}

TEST_F(IpcContainerCommandFamilyTest, TransactionRollbackRestoresPlaybackReferencesAndRowOrder)
{
    auto& pm = ProjectManager::instance();
    IPCServer server(nullptr);
    server.setProjectManager(&pm);
    server.setAppState(IPCServer::AppState::Ready);
    auto& actions = ActionManager::instance();
    const auto authoredPlatform = pm.currentProject()->platforms.front().id;
    const auto volumeProperty = harkenplayableproperties::PlayablePropertyIdV1::Volume;
    const auto definition = PlayablePropertyAuthoringV1::definitionLocator(*soundA_);
    ASSERT_TRUE(definition.has_value());
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*soundA_, *definition,
        static_cast<uint8_t>(volumeProperty), -8.0, authoredPlatform));
    const auto platformOverrides = soundA_->platformOverrides;
    for (const auto& [owner, mode] : {
             std::pair{track_, SelectionMode::Random},
             std::pair{container_, SelectionMode::Match},
             std::pair{container_, SelectionMode::Blend}}) {
        SCOPED_TRACE(static_cast<int>(mode));
        if (soundA_->parent != owner) ASSERT_TRUE(pm.moveItem(soundA_, owner));
        if (auto* timeline = owner->comp<VTrack>()) {
            for (int64_t start : {0, 20}) {
                Clip clip{QUuid::createUuid(), soundA_->guid, start, 10};
                ASSERT_TRUE(TimelineEditing::addClip(&pm, owner, clip).success);
            }
        }
        if (auto* selection = owner->comp<VNX::VSelection>()) {
            selection->mode = mode;
            selection->matchResponse = VNX::VariableResponse::AtStart;
            selection->blendResponse = VNX::VariableResponse::Live;
            EXPECT_TRUE(selection->selections.shared.setMatchGrid({}));
            EXPECT_TRUE(selection->selections.shared.setMixing({}));
            ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*owner, soundA_->guid));
            auto reorderedSelections = owner->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
            std::reverse(reorderedSelections.begin(), reorderedSelections.end());
            EXPECT_TRUE(owner->comp<VNX::VSelection>()->selections.shared.setOccurrenceRows(reorderedSelections));
            selection->selections.shared.find(owner->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().stableOccurrenceOrdinal)->weight = 37;
            if (mode == SelectionMode::Match) {
                auto grid = selection->matchGrid();
                ASSERT_TRUE(grid.addRow(soundA_->guid));
                ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
                auto rule = selection->transitions.rules.front();
                rule.source = soundA_->guid;
                ASSERT_TRUE(selection->transitions.addRule(rule));
            } else {
                {
                    auto mixing = selection->mixing();
                    mixing.rows.push_back({QUuid::createUuid(),
                        {VNX::MixingChildDef::unity(soundA_->guid, 0.0f, 100.0f)}});
                    EXPECT_TRUE(selection->selections.shared.setMixing(mixing));
                }
            }
        }
        const auto children = owner->children;
        const auto selection = owner->comp<VNX::VSelection>() ? owner->comp<VNX::VSelection>()->toJson() : QJsonObject{};
        const auto clips = owner->comp<VTrack>() ? owner->comp<VTrack>()->clips : ClipLedger{};
        const QString command = owner == track_ ? QStringLiteral("add_child_to_container")
                                                : QStringLiteral("remove_child_from_container");
        const QJsonObject arguments = pairArgs(container_, soundA_);
        const QJsonObject action{{"cmd", command}, {"args", arguments}};
        const auto preview = actions.preview({{"actions", QJsonArray{action}}});
        ASSERT_EQ(preview.value("status").toString(), QStringLiteral("ok"))
            << QJsonDocument(preview).toJson().toStdString();
        const QJsonObject transaction{{"tx_id", preview.value("result").toObject().value("tx_id")}};
        ASSERT_EQ(actions.confirm(transaction).value("status").toString(), QStringLiteral("ok"));
        const auto applied = actions.apply(transaction);
        ASSERT_EQ(applied.value("status").toString(), QStringLiteral("ok"))
            << QJsonDocument(applied).toJson().toStdString();
        const auto rolledBack = actions.rollback(transaction);
        ASSERT_EQ(rolledBack.value("status").toString(), QStringLiteral("ok"))
            << QJsonDocument(rolledBack).toJson().toStdString();
        EXPECT_EQ(soundA_->parent, owner);
        EXPECT_EQ(soundA_->platformOverrides, platformOverrides);
        EXPECT_EQ(owner->children, children);
        if (owner->comp<VNX::VSelection>()) EXPECT_EQ(owner->comp<VNX::VSelection>()->toJson(), selection);
        if (owner->comp<VTrack>()) EXPECT_EQ(owner->comp<VTrack>()->clips, clips);
        UndoRedoManager::instance().clear();
    }
}

TEST_F(IpcContainerCommandFamilyTest, SpatializationMixLegacyEditAndRollbackPreserveInheritance)
{
    auto* spatial = container_->comp<VSpatial>();
    ASSERT_NE(spatial, nullptr);
    spatial->spatializationMixPresent = false;
    const auto groups = container_->fieldGroups;
    const QJsonObject args{{"container_id", static_cast<qint64>(itemShortID(container_))},
                          {"property", "spatializationMix"}, {"value", 0.5}};
    ASSERT_TRUE(run("set_container_property", args).success);
    EXPECT_TRUE(spatial->spatializationMixPresent);
    EXPECT_FLOAT_EQ(spatial->spatializationMix, 0.5f);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FALSE(spatial->spatializationMixPresent);
    auto invalid = args;
    invalid["value"] = 1.01;
    EXPECT_FALSE(run("set_container_property", invalid).success);
    EXPECT_FALSE(spatial->spatializationMixPresent);

    IPCServer server(nullptr);
    server.setProjectManager(&ProjectManager::instance());
    server.setAppState(IPCServer::AppState::Ready);
    auto& actions = ActionManager::instance();
    const auto preview = actions.preview({{"actions", QJsonArray{
        QJsonObject{{"cmd", "set_container_property"}, {"args", args}}}}});
    ASSERT_EQ(preview.value("status").toString(), QStringLiteral("ok"))
        << QJsonDocument(preview).toJson().toStdString();
    const QJsonObject transaction{{"tx_id", preview.value("result").toObject().value("tx_id")}};
    ASSERT_EQ(actions.confirm(transaction).value("status").toString(), QStringLiteral("ok"));
    ASSERT_EQ(actions.apply(transaction).value("status").toString(), QStringLiteral("ok"));
    EXPECT_TRUE(spatial->spatializationMixPresent);
    EXPECT_FLOAT_EQ(spatial->spatializationMix, 0.5f);
    ASSERT_EQ(actions.rollback(transaction).value("status").toString(), QStringLiteral("ok"));
    EXPECT_FALSE(spatial->spatializationMixPresent);
    EXPECT_EQ(container_->fieldGroups, groups);
}

TEST_F(IpcContainerCommandFamilyTest, IllegalPairsAreRejectedWithStableCodes)
{
    ActionResult r = run(QStringLiteral("add_child_to_container"), pairArgs(soundA_, soundB_));
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("NOT_A_CONTAINER"));

    r = run(QStringLiteral("add_child_to_container"), pairArgs(piece_, soundA_));
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("NOT_A_CONTAINER"));

    ProjectItem* master = ProjectManager::instance().getMasterBusItem();
    ASSERT_NE(master, nullptr);
    r = run(QStringLiteral("add_child_to_container"), pairArgs(master, soundA_));
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("USE_BUS_API"));
}

TEST_F(IpcContainerCommandFamilyTest, SetOccurrencesRejectsTargetsOutsideContainer)
{
    ASSERT_TRUE(run(QStringLiteral("add_child_to_container"),
                    pairArgs(container_, soundA_)).success);
    SelectionData selections;
    EXPECT_TRUE(selections.shared.setOccurrenceRows({{soundB_->guid, 0}}));
    selections.shared.occurrenceOrdinalHighWater = 1;
    const ActionResult r = run(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(container_->objectID)},
         {QStringLiteral("property"), QStringLiteral("selections")},
         {QStringLiteral("value"), selections.toJson()}});
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("INVALID_CHILD_REFERENCE"));
    EXPECT_NE(occurrenceFor(container_, soundA_->guid), nullptr);
    EXPECT_EQ(occurrenceFor(container_, soundB_->guid), nullptr);
}

TEST_F(IpcContainerCommandFamilyTest, PreviewVerdictMatchesExecutionVerdict)
{
    IPCServer server(nullptr);

    const auto previewStatus = [&](const QJsonObject& args) {
        QJsonObject previewArgs = args;
        previewArgs[QStringLiteral("preview")] = true;
        return server.dispatchCommand(QStringLiteral("add_child_to_container"), previewArgs)
            .value(QStringLiteral("status")).toString();
    };
    const auto executeStatus = [&](const QJsonObject& args) {
        return server.dispatchCommand(QStringLiteral("add_child_to_container"), args)
            .value(QStringLiteral("status")).toString();
    };

    ProjectItem* master = ProjectManager::instance().getMasterBusItem();
    ASSERT_NE(master, nullptr);

    const struct { const ProjectItem* container; const ProjectItem* child; bool allowed; } cases[] = {
        { container_, soundA_, true },
        { track_, soundB_, true },
        { piece_, track_, true },
        { piece_, soundC_, false },
        { soundC_, soundA_, false },
        { master, soundC_, false },
    };

    for (const auto& c : cases)
    {
        const QJsonObject args = pairArgs(c.container, c.child);
        const QString expected = c.allowed ? QStringLiteral("ok") : QStringLiteral("error");
        EXPECT_EQ(previewStatus(args), expected)
            << c.container->name.toStdString() << " <- " << c.child->name.toStdString();
        EXPECT_EQ(executeStatus(args), expected)
            << c.container->name.toStdString() << " <- " << c.child->name.toStdString();
    }
}

TEST_F(IpcContainerCommandFamilyTest, CreateItemUnknownTypeReportsInvalidTypeWithoutSideEffect)
{
    ProjectData* project = ProjectManager::instance().currentProject();
    ASSERT_NE(project, nullptr);
    const int itemsBefore = totalProjectItems(project);
    const size_t bindingsBefore = project->allFileBindings().size();

    const ActionResult r = run(QStringLiteral("create_item"), QJsonObject{
        {QStringLiteral("parent"), QString()},
        {QStringLiteral("name"), QStringLiteral("Fam_Mistyped")},
        {QStringLiteral("type"), QStringLiteral("Sund")}
    });

    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorCode, QStringLiteral("INVALID_TYPE"));
    EXPECT_EQ(totalProjectItems(project), itemsBefore);
    EXPECT_EQ(project->allFileBindings().size(), bindingsBefore);
}

TEST_F(IpcContainerCommandFamilyTest, BusClipboardCommandsRejectWithoutSideEffects)
{
    ProjectItem* master = ProjectManager::instance().getMasterBusItem();
    ASSERT_NE(master, nullptr);
    ProjectManager::instance().clearClipboard();

    const QJsonObject args{
        {QStringLiteral("object_id"), static_cast<qint64>(itemShortID(master))}
    };
    for (const QString& command : {
             QStringLiteral("copy_item"), QStringLiteral("cut_item")}) {
        const ActionResult result = run(command, args);
        EXPECT_FALSE(result.success) << command.toStdString();
        EXPECT_EQ(result.errorCode, QStringLiteral("NOT_ELIGIBLE"));
        EXPECT_TRUE(ProjectManager::instance().clipboardEmpty());
    }
}
