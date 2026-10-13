#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "ActionManager.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "ProjectManager.h"
#include "FieldGroupProjectAdapter.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <limits>

namespace VNX {
namespace {

class FieldGroupActionsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char arg0[] = "harken_editor_tests";
            static char* argv[] = {arg0, nullptr};
            static QApplication app(argc, argv);
        }
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(directory.isValid());
        ProjectConfig config;
        config.platforms = HarkenTest::platforms({"Windows", "Android", "macOS", "Linux", "iOS"});
        config.name = QStringLiteral("GroupActions");
        config.location = directory.path();
        ASSERT_TRUE(manager.createProject(config));
        auto* parent = manager.createItem(nullptr, QStringLiteral("GroupLayer"), ProjectItem::Type::Layer);
        ASSERT_NE(parent, nullptr);
        item = manager.createItem(parent, QStringLiteral("GroupSound"), ProjectItem::Type::Sound);
        ASSERT_NE(item, nullptr);
        auto& actions = ActionManager::instance();
        actions.clearAllForTesting();
        registerFieldGroupActions(actions, &manager);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ActionManager::instance().clearAllForTesting();
        ProjectManager::instance().closeProject();
    }

    QJsonObject args(const QString& group = QStringLiteral("mainOutput"),
                     FieldGroupScope scope = {}) const
    {
        QJsonObject result{{"definition_guid", item->guid.toString(QUuid::WithoutBraces)},
                           {"group", group}};
        if (scope) {
            const auto name = HarkenTest::fieldGroupPlatformIdString(*scope);
            result.insert("platform_ids", QJsonArray{QString::fromLatin1(name.data(), name.size())});
        } else {
            result.insert("scope", "Shared");
        }
        return result;
    }

    ActionResult local()
    {
        auto value = args();
        value.insert("mode", "Local");
        return ActionManager::instance().execute("set_field_group_mode", value);
    }

    QTemporaryDir directory;
    ProjectItem* item = nullptr;
};

TEST_F(FieldGroupActionsTest, LocalAndFieldUndoRestoreWholeGroupAndDerivedView)
{
    const auto initial = item->fieldGroups;
    auto activated = local();
    ASSERT_TRUE(activated.success) << activated.errorMessage.toStdString();
    ASSERT_TRUE(item->fieldGroups.mainOutput.shared.localHistory);
    auto edit = args();
    edit.insert("field", "outputBusVolume");
    edit.insert("value", -9.0);
    auto changed = ActionManager::instance().execute("set_field_group_field", edit);
    ASSERT_TRUE(changed.success) << changed.errorMessage.toStdString();
    EXPECT_FLOAT_EQ(item->comp<VMixNode>()->outputBusVolume, -9.0f);
    EXPECT_EQ(item->playablePropertyPresence.bits & kFieldGroupPropertyMask, 0u);
    ASSERT_TRUE(UndoRedoManager::instance().canUndo());
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FLOAT_EQ(item->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, 0.0f);
    EXPECT_FLOAT_EQ(item->comp<VMixNode>()->outputBusVolume, 0.0f);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(item->fieldGroups, initial);
    UndoRedoManager::instance().undoStack()->redo();
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_FLOAT_EQ(item->comp<VMixNode>()->outputBusVolume, -9.0f);
}

TEST_F(FieldGroupActionsTest, ReadOnlyUnknownScopesAndOccurrenceTargetsHaveNoUndo)
{
    const auto before = item->fieldGroups;
    auto edit = args();
    edit.insert("field", "outputBusVolume");
    edit.insert("value", -9.0);
    auto result = ActionManager::instance().execute("set_field_group_field", edit);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.undoFn);
    edit.remove("scope");
    edit.insert("platform_ids", QJsonArray{"Unknown"});
    EXPECT_FALSE(ActionManager::instance().execute("set_field_group_field", edit).success);
    edit = args();
    edit.insert("mode", "Local");
    edit.insert("occurrence_target", QJsonObject{});
    EXPECT_FALSE(ActionManager::instance().execute("set_field_group_mode", edit).success);
    EXPECT_EQ(item->fieldGroups, before);
}

TEST_F(FieldGroupActionsTest, PlatformLocalDoesNotWriteSharedAndDuplicatePlatformsReject)
{
    auto value = args();
    value.remove("scope");
    value.insert("platform_ids", QJsonArray{HarkenTest::platformIdString("Windows"), HarkenTest::platformIdString("Android")});
    value.insert("mode", "Local");
    auto result = ActionManager::instance().execute("set_field_group_mode", value);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(item->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
    EXPECT_FALSE(item->fieldGroups.mainOutput.shared.localHistory);
    EXPECT_EQ(item->fieldGroups.mainOutput.platforms.size(), 2u);
    const auto before = item->fieldGroups;
    value.insert("platform_ids", QJsonArray{HarkenTest::platformIdString("Windows"), HarkenTest::platformIdString("Windows")});
    EXPECT_FALSE(ActionManager::instance().execute("set_field_group_mode", value).success);
    EXPECT_EQ(item->fieldGroups, before);
}

TEST_F(FieldGroupActionsTest, RootCannotChangeInheritanceOrRebuildInSharedOrPlatformScopes)
{
    auto* root = item->parent;
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->parent, nullptr);
    const auto before = root->fieldGroups;
    root->clearDirty();
    auto* stack = UndoRedoManager::instance().undoStack();
    for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
        for (const auto scope : {FieldGroupScope{}, FieldGroupScope{HarkenTest::FieldGroupPlatformIds::Windows}}) {
            for (const auto* operation : {"Inherit", "Local", "Rebuild"}) {
                auto request = args(QLatin1String(group), scope);
                request.insert("definition_guid", root->guid.toString(QUuid::WithoutBraces));
                const bool rebuild = QLatin1String(operation) == QLatin1String("Rebuild");
                if (!rebuild) request.insert("mode", QLatin1String(operation));
                const auto result = ActionManager::instance().execute(
                    rebuild ? "rebuild_field_group_local" : "set_field_group_mode", request);
                EXPECT_FALSE(result.success);
                EXPECT_FALSE(result.undoFn);
                EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_REJECTED"));
                EXPECT_EQ(root->fieldGroups, before);
                EXPECT_FALSE(root->dirtyBit);
                EXPECT_EQ(stack->count(), 0);
            }
        }
    }
}

TEST_F(FieldGroupActionsTest, ClearOutputSelectsMasterAndKeepsVolume)
{
    ASSERT_TRUE(local().success);
    auto edit = args();
    edit.insert("field", "outputBusVolume");
    edit.insert("value", -5.0);
    ASSERT_TRUE(ActionManager::instance().execute("set_field_group_field", edit).success);
    edit.insert("field", "mainOutputBusRef");
    edit.insert("value", QJsonValue(QJsonValue::Null));
    auto result = ActionManager::instance().execute("set_field_group_field", edit);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    const auto& state = item->fieldGroups.mainOutput.shared;
    EXPECT_EQ(state.mode, FieldGroupMode::Local);
    EXPECT_FLOAT_EQ(state.localHistory->outputBusVolume, -5.0f);
    EXPECT_EQ(state.localHistory->mainOutputBusRef,
              toFieldGroupGuid(ProjectManager::instance().currentProject()->busses.front().rootItem->guid));
}

TEST_F(FieldGroupActionsTest, DeletedDormantTargetIsPreservedAndRequiresExplicitRebuild)
{
    auto& manager = ProjectManager::instance();
    auto* master = manager.currentProject()->busses.front().rootItem.get();
    auto* bus = manager.createItem(master, QStringLiteral("DormantBus"), ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    ASSERT_TRUE(local().success);
    auto edit = args();
    edit.insert("field", "mainOutputBusRef");
    edit.insert("value", bus->guid.toString(QUuid::WithoutBraces));
    ASSERT_TRUE(ActionManager::instance().execute("set_field_group_field", edit).success);
    auto mode = args();
    mode.insert("mode", "Inherit");
    ASSERT_TRUE(ActionManager::instance().execute("set_field_group_mode", mode).success);
    const auto dormant = item->fieldGroups;
    auto busRef = *std::find_if(master->children.begin(), master->children.end(),
                               [bus](const auto& child) { return child.get() == bus; });
    ASSERT_TRUE(manager.deleteItem(bus));
    EXPECT_EQ(item->fieldGroups, dormant);
    EXPECT_FALSE(local().success);
    auto rebuilt = ActionManager::instance().execute("rebuild_field_group_local", args());
    ASSERT_TRUE(rebuilt.success) << rebuilt.errorMessage.toStdString();
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(item->fieldGroups, dormant);
}

TEST_F(FieldGroupActionsTest, ActiveDeleteAndUndoRestoreOnlyLocalProviders)
{
    auto& manager = ProjectManager::instance();
    auto* master = manager.currentProject()->busses.front().rootItem.get();
    auto* bus = manager.createItem(master, QStringLiteral("ActiveBus"), ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    ASSERT_TRUE(local().success);
    auto edit = args();
    edit.insert("field", "mainOutputBusRef");
    edit.insert("value", bus->guid.toString(QUuid::WithoutBraces));
    ASSERT_TRUE(ActionManager::instance().execute("set_field_group_field", edit).success);
    const auto before = item->fieldGroups;
    ASSERT_TRUE(manager.deleteItem(bus));
    EXPECT_EQ(item->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
              toFieldGroupGuid(master->guid));
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(item->fieldGroups, before);
}

TEST_F(FieldGroupActionsTest, RootInheritBusDeletionAndUndoPreserveDormantChildAndPlatformHistory)
{
    auto& manager = ProjectManager::instance();
    auto* master = manager.getMasterBusItem();
    auto* outputBus = manager.createItem(master, QStringLiteral("RootOutput"), ProjectItem::Type::Bus);
    auto* sendBus = manager.createItem(master, QStringLiteral("RootSend"), ProjectItem::Type::Bus);
    ASSERT_NE(outputBus, nullptr);
    ASSERT_NE(sendBus, nullptr);
    const auto outputGuid = toFieldGroupGuid(outputBus->guid);
    const auto sendGuid = toFieldGroupGuid(sendBus->guid);
    const auto masterGuid = toFieldGroupGuid(master->guid);
    auto* root = item->parent;
    auto rootGroups = root->fieldGroups;
    rootGroups.mainOutput.shared.localHistory = MainOutputFieldGroup{outputGuid, -7.0f};
    rootGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Inherit, MainOutputFieldGroup{outputGuid, -5.0f}};
    rootGroups.send.shared.localHistory = SendFieldGroup{{{sendGuid, -9.0f}}};
    rootGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::UseShared, SendFieldGroup{}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(root, rootGroups));
    auto dormant = item->fieldGroups;
    dormant.mainOutput.shared.localHistory = MainOutputFieldGroup{outputGuid, -11.0f};
    dormant.send.shared.localHistory = SendFieldGroup{{{sendGuid, -15.0f}}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(item, dormant));
    UndoRedoManager::instance().clear();
    auto* stack = UndoRedoManager::instance().undoStack();

    ASSERT_TRUE(manager.deleteItem(outputBus));
    EXPECT_EQ(root->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
    EXPECT_EQ(root->fieldGroups.mainOutput.shared.localHistory,
              (MainOutputFieldGroup{masterGuid, -7.0f}));
    EXPECT_EQ(root->fieldGroups.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory,
              (MainOutputFieldGroup{masterGuid, -5.0f}));
    EXPECT_EQ(root->fieldGroups.send, rootGroups.send);
    EXPECT_EQ(item->fieldGroups, dormant);
    stack->undo();
    EXPECT_EQ(root->fieldGroups, rootGroups);
    EXPECT_EQ(item->fieldGroups, dormant);

    ASSERT_TRUE(manager.deleteItem(sendBus));
    EXPECT_EQ(root->fieldGroups.send.shared.mode, FieldGroupMode::Inherit);
    EXPECT_EQ(root->fieldGroups.send.shared.localHistory, SendFieldGroup{});
    EXPECT_EQ(root->fieldGroups.send.platforms, rootGroups.send.platforms);
    EXPECT_EQ(root->fieldGroups.mainOutput, rootGroups.mainOutput);
    EXPECT_EQ(item->fieldGroups, dormant);
    stack->undo();
    EXPECT_EQ(root->fieldGroups, rootGroups);
    EXPECT_EQ(item->fieldGroups, dormant);
}

TEST_F(FieldGroupActionsTest, DeleteCannotCreateMainOutputSendConflict)
{
    auto& manager = ProjectManager::instance();
    auto* master = manager.currentProject()->busses.front().rootItem.get();
    auto* bus = manager.createItem(master, QStringLiteral("ConflictBus"), ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    ASSERT_TRUE(local().success);
    auto edit = args();
    edit.insert("field", "mainOutputBusRef");
    edit.insert("value", bus->guid.toString(QUuid::WithoutBraces));
    ASSERT_TRUE(ActionManager::instance().execute("set_field_group_field", edit).success);
    auto send = args();
    send.insert("group", "send");
    send.insert("mode", "Local");
    ASSERT_TRUE(ActionManager::instance().execute("set_field_group_mode", send).success);
    send.insert("sends", QJsonArray{QJsonObject{
        {"targetBusID", master->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -12.0}}});
    ASSERT_TRUE(ActionManager::instance().execute("replace_field_group_sends", send).success);
    const auto before = item->fieldGroups;
    EXPECT_FALSE(manager.deleteItem(bus));
    EXPECT_EQ(item->fieldGroups, before);
    EXPECT_NE(manager.findItemByGuid(bus->guid), nullptr);
}

TEST_F(FieldGroupActionsTest, SavedDormantHistoriesRestoreAllGroupsAfterCopyAndUndo)
{
    using Mode = FieldGroupMode;
    using Platform = harkenprojectschema::AuthoringGuid;
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    auto* master = manager.getMasterBusItem();
    auto* outputBus = manager.createItem(master, QStringLiteral("HistoryOutput"), ProjectItem::Type::Bus);
    auto* sendBus = manager.createItem(master, QStringLiteral("HistorySend"), ProjectItem::Type::Bus);
    auto* attenuation = manager.createItem(nullptr, QStringLiteral("HistoryAttenuation"),
                                          ProjectItem::Type::Attenuation);
    ASSERT_NE(outputBus, nullptr);
    ASSERT_NE(sendBus, nullptr);
    ASSERT_NE(attenuation, nullptr);
    const auto outputGuid = toFieldGroupGuid(outputBus->guid);
    const auto sendGuid = toFieldGroupGuid(sendBus->guid);
    const auto attenuationGuid = toFieldGroupGuid(attenuation->guid);
    DefinitionFieldGroups authored;
    authored.mainOutput.shared = {Mode::Local, MainOutputFieldGroup{outputGuid, -3.0f}};
    authored.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Local, MainOutputFieldGroup{outputGuid, -5.0f}};
    authored.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        {Mode::Local, MainOutputFieldGroup{outputGuid, -7.0f}};
    authored.send.shared = {Mode::Local, SendFieldGroup{{{sendGuid, -9.0f}}}};
    authored.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Local, SendFieldGroup{{{sendGuid, -11.0f}}}};
    authored.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {Mode::Local, SendFieldGroup{}};
    authored.spatialRating.shared =
        {Mode::Local, SpatialRatingFieldGroup{-0.3f, true, attenuationGuid, 2.0f}};
    authored.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Local, SpatialRatingFieldGroup{0.3f, true, attenuationGuid, 3.0f}};
    authored.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        {Mode::Local, SpatialRatingFieldGroup{0.7f, false, {}, 4.0f}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(item, authored));
    const std::array<FieldGroupScope, 3> scopes{
        std::nullopt, HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android};
    for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
        for (const auto scope : scopes) {
            auto edit = args(QLatin1String(group), scope);
            edit.insert("mode", scope == HarkenTest::FieldGroupPlatformIds::Windows ? "UseShared" : "Inherit");
            const auto result = actions.execute("set_field_group_mode", edit);
            ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
        }
    }
    const auto dormant = item->fieldGroups;
    const auto sourceGuid = item->guid;
    const QString path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(path));
    item = manager.findItemByGuid(sourceGuid);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->fieldGroups, dormant);
    for (const auto scope : scopes) {
        const auto snapshot = buildFieldGroupEffectiveSnapshot(*manager.currentProject(), scope);
        ASSERT_TRUE(snapshot.ok());
        const auto& effective = snapshot.definitions.at(toFieldGroupGuid(sourceGuid));
        EXPECT_EQ(effective.mainOutput,
                  (MainOutputFieldGroup{toFieldGroupGuid(manager.getMasterBusItem()->guid), 0.0f}));
        EXPECT_EQ(effective.send, SendFieldGroup{});
        EXPECT_EQ(effective.spatialRating, SpatialRatingFieldGroup{});
    }
    UndoRedoManager::instance().clear();
    item = manager.duplicateItem(item);
    ASSERT_NE(item, nullptr);
    const auto copyGuid = item->guid;
    EXPECT_NE(copyGuid, sourceGuid);
    EXPECT_EQ(item->fieldGroups, dormant);
    auto* stack = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(stack->count(), 1);
    for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
        for (const auto scope : scopes) {
            auto edit = args(QLatin1String(group), scope);
            edit.insert("mode", "Local");
            const auto result = actions.execute("set_field_group_mode", edit);
            ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
        }
    }
    const auto expectOwnValues = [&] {
        for (const auto scope : scopes) {
            const auto& groups = item->fieldGroups;
            EXPECT_EQ(ResolveFieldGroupLocal(groups.mainOutput, scope), ResolveFieldGroupLocal(authored.mainOutput, scope));
            EXPECT_EQ(ResolveFieldGroupLocal(groups.send, scope), ResolveFieldGroupLocal(authored.send, scope));
            EXPECT_EQ(ResolveFieldGroupLocal(groups.spatialRating, scope), ResolveFieldGroupLocal(authored.spatialRating, scope));
        }
    };
    expectOwnValues();
    EXPECT_EQ(manager.findItemByGuid(sourceGuid)->fieldGroups, dormant);
    ASSERT_EQ(stack->count(), 4);
    while (stack->index() > 1) stack->undo();
    EXPECT_EQ(item->fieldGroups, dormant);
    stack->undo();
    EXPECT_EQ(manager.findItemByGuid(copyGuid), nullptr);
    stack->redo();
    item = manager.findItemByGuid(copyGuid);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->fieldGroups, dormant);
    while (stack->canRedo()) stack->redo();
    expectOwnValues();
    EXPECT_EQ(manager.findItemByGuid(sourceGuid)->fieldGroups, dormant);
}

TEST_F(FieldGroupActionsTest, LoadedMissingHistoriesRejectAtomicActivationAndRebuildUndoably)
{
    using Mode = FieldGroupMode;
    using Platform = harkenprojectschema::AuthoringGuid;
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    auto* master = manager.getMasterBusItem();
    auto* busA = manager.createItem(master, QStringLiteral("RecoverySendA"), ProjectItem::Type::Bus);
    auto* busB = manager.createItem(master, QStringLiteral("RecoverySendB"), ProjectItem::Type::Bus);
    auto* attenuation = manager.createItem(nullptr, QStringLiteral("RecoveryAttenuation"),
                                          ProjectItem::Type::Attenuation);
    auto* parent = manager.createItem(nullptr, QStringLiteral("RecoveryParent"), ProjectItem::Type::Layer);
    ASSERT_NE(busA, nullptr);
    ASSERT_NE(busB, nullptr);
    ASSERT_NE(attenuation, nullptr);
    ASSERT_NE(parent, nullptr);
    ASSERT_TRUE(manager.moveItem(item, parent));
    const auto masterGuid = toFieldGroupGuid(master->guid);
    const auto busAGuid = toFieldGroupGuid(busA->guid);
    const auto busBGuid = toFieldGroupGuid(busB->guid);
    const auto attenuationGuid = toFieldGroupGuid(attenuation->guid);
    auto provider = MakeRootFieldGroups(masterGuid);
    provider.spatialRating.shared = {Mode::Local, SpatialRatingFieldGroup{}};
    provider.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Local, MainOutputFieldGroup{masterGuid, -2.0f}};
    provider.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        {Mode::Local, MainOutputFieldGroup{masterGuid, -6.0f}};
    provider.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Local, SendFieldGroup{{{busAGuid, -4.0f}}}};
    provider.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        {Mode::Local, SendFieldGroup{{{busBGuid, -9.0f}}}};
    provider.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Local, SpatialRatingFieldGroup{-0.2f, true, attenuationGuid, 2.0f}};
    provider.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        {Mode::Local, SpatialRatingFieldGroup{0.4f, false, attenuationGuid, 3.0f}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(parent, provider));
    const harkenprojectschema::AuthoringGuid missingOutput{
        0x123456789abcdef0ULL, std::numeric_limits<uint64_t>::max()};
    const harkenprojectschema::AuthoringGuid missingSend{
        0x223456789abcdef0ULL, std::numeric_limits<uint64_t>::max()};
    const harkenprojectschema::AuthoringGuid missingAttenuation{
        0x323456789abcdef0ULL, std::numeric_limits<uint64_t>::max()};
    DefinitionFieldGroups dormant;
    dormant.mainOutput.shared = {Mode::Inherit, MainOutputFieldGroup{missingOutput, -12.0f}};
    dormant.send.shared = {Mode::Inherit, SendFieldGroup{{{missingSend, -13.0f}}}};
    dormant.spatialRating.shared =
        {Mode::Inherit, SpatialRatingFieldGroup{0.6f, true, missingAttenuation, 4.0f}};
    dormant.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Inherit, MainOutputFieldGroup{busBGuid, -15.0f}};
    dormant.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Inherit, SendFieldGroup{{{busBGuid, -17.0f}}}};
    dormant.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::Inherit, SpatialRatingFieldGroup{0.2f, true, attenuationGuid, 5.0f}};
    dormant.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        {Mode::UseShared, dormant.mainOutput.shared.localHistory};
    dormant.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        {Mode::UseShared, dormant.send.shared.localHistory};
    dormant.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        {Mode::UseShared, dormant.spatialRating.shared.localHistory};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(item, dormant));
    const auto sourceGuid = item->guid;
    const auto parentGuid = parent->guid;
    const QString path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(path));
    parent = manager.findItemByGuid(parentGuid);
    item = manager.findItemByGuid(sourceGuid);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->fieldGroups, dormant);
    ASSERT_EQ(manager.setClipboard({item}, ProjectManager::ClipboardOp::Copy), 1);
    const auto copies = manager.pasteClipboard(parent);
    ASSERT_EQ(copies.size(), 1u);
    item = copies.front();
    EXPECT_EQ(item->fieldGroups, dormant);
    const auto windows = buildFieldGroupEffectiveSnapshot(*manager.currentProject(), HarkenTest::FieldGroupPlatformIds::Windows);
    const auto android = buildFieldGroupEffectiveSnapshot(*manager.currentProject(), HarkenTest::FieldGroupPlatformIds::Android);
    ASSERT_TRUE(windows.ok());
    ASSERT_TRUE(android.ok());
    const auto& effectiveWindows = windows.definitions.at(toFieldGroupGuid(item->guid));
    const auto& effectiveAndroid = android.definitions.at(toFieldGroupGuid(item->guid));
    EXPECT_EQ(effectiveWindows.mainOutput, provider.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localHistory);
    EXPECT_EQ(effectiveWindows.send, provider.send.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localHistory);
    EXPECT_EQ(effectiveWindows.spatialRating, provider.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localHistory);
    EXPECT_EQ(effectiveAndroid.mainOutput, provider.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory);
    EXPECT_EQ(effectiveAndroid.send, provider.send.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory);
    EXPECT_EQ(effectiveAndroid.spatialRating, provider.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::Android).localHistory);
    UndoRedoManager::instance().clear();
    auto marker = args(QStringLiteral("spatialRating"));
    marker.insert("definition_guid", parentGuid.toString(QUuid::WithoutBraces));
    marker.insert("field", "spatial.panning");
    marker.insert("value", 0.8);
    ASSERT_TRUE(actions.execute("set_field_group_field", marker).success);
    auto* stack = UndoRedoManager::instance().undoStack();
    stack->undo();
    QObject receiver;
    int propertyChanges = 0;
    QObject::connect(&manager, &ProjectManager::itemPropertyChanged, &receiver,
                     [&] { ++propertyChanges; });
    for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
        SCOPED_TRACE(group);
        item->clearDirty();
        parent->clearDirty();
        propertyChanges = 0;
        auto activate = args(QLatin1String(group));
        activate.remove("scope");
        activate.insert("platform_ids", QJsonArray{HarkenTest::platformIdString("Windows"), HarkenTest::platformIdString("Android")});
        activate.insert("mode", "Local");
        const auto result = actions.execute("set_field_group_mode", activate);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_REJECTED"));
        EXPECT_FALSE(result.undoFn);
        EXPECT_EQ(item->fieldGroups, dormant);
        EXPECT_EQ(parent->fieldGroups, provider);
        EXPECT_FALSE(item->dirtyBit);
        EXPECT_FALSE(parent->dirtyBit);
        EXPECT_EQ(propertyChanges, 0);
        EXPECT_EQ(stack->count(), 1);
        EXPECT_EQ(stack->index(), 0);
        EXPECT_TRUE(stack->canRedo());
        const auto diagnostics = result.data.value(QStringLiteral("diagnostics")).toArray();
        EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& value) {
            return value.toObject().value(QStringLiteral("scope")) == HarkenTest::platformIdString("Android");
        }));
        activate.remove("mode");
        const auto rebuilt = actions.execute("rebuild_field_group_local", activate);
        ASSERT_TRUE(rebuilt.success) << rebuilt.errorMessage.toStdString();
        auto expected = dormant;
        if (QLatin1String(group) == QStringLiteral("mainOutput")) {
            expected.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {Mode::Local, effectiveWindows.mainOutput};
            expected.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {Mode::Local, effectiveAndroid.mainOutput};
        } else if (QLatin1String(group) == QStringLiteral("send")) {
            expected.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {Mode::Local, effectiveWindows.send};
            expected.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {Mode::Local, effectiveAndroid.send};
        } else {
            expected.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {Mode::Local, effectiveWindows.spatialRating};
            expected.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {Mode::Local, effectiveAndroid.spatialRating};
        }
        for (const auto scope : {FieldGroupScope{}, FieldGroupScope{HarkenTest::FieldGroupPlatformIds::Windows},
                                  FieldGroupScope{HarkenTest::FieldGroupPlatformIds::Android}}) {
            EXPECT_EQ(ResolveFieldGroupLocal(item->fieldGroups.mainOutput, scope), ResolveFieldGroupLocal(expected.mainOutput, scope));
            EXPECT_EQ(ResolveFieldGroupLocal(item->fieldGroups.send, scope), ResolveFieldGroupLocal(expected.send, scope));
            EXPECT_EQ(ResolveFieldGroupLocal(item->fieldGroups.spatialRating, scope), ResolveFieldGroupLocal(expected.spatialRating, scope));
        }
        EXPECT_EQ(stack->count(), 1);
        EXPECT_EQ(stack->index(), 1);
        stack->undo();
        EXPECT_EQ(item->fieldGroups, dormant);
        EXPECT_EQ(stack->index(), 0);
        EXPECT_TRUE(stack->canRedo());
    }
    EXPECT_EQ(manager.findItemByGuid(sourceGuid)->fieldGroups, dormant);
}

TEST(FieldGroupComponentEntry, CreatedComponentsBindTheirOwnerBeforeFieldWrites)
{
    ProjectItem sound(QStringLiteral("Sound"), ProjectItem::Type::Sound);
    auto* mix = sound.compOrCreate<VMixNode>();
    auto* spatial = sound.compOrCreate<VSpatial>();
    EXPECT_EQ(mix->hostItem(), &sound);
    EXPECT_EQ(spatial->hostItem(), &sound);
    EXPECT_FALSE(mix->setField(QStringLiteral("outputBusVolume"), -4.0f));
    EXPECT_FALSE(spatial->setField(QStringLiteral("is3DEnabled"), true));
    EXPECT_TRUE(mix->setField(QStringLiteral("volume"), -4.0f));
    EXPECT_EQ(sound.fieldGroups, DefinitionFieldGroups{});
}

}
}
