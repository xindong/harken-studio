#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "BindingEditing.h"
#include "Components/VMixNode.h"
#include "PropertyEditSession.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "harken_playable_properties.g.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

}

class BindingEditingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerBindingActions(VNX::ActionManager::instance());
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(temp_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("BindingEditing");
        config.location = temp_.path();
        ASSERT_TRUE(manager.createProject(config));
        item_ = manager.createItem(nullptr, QStringLiteral("BoundSound"),
                                   VNX::ProjectItem::Type::Sound);
        variable_ = manager.createItem(nullptr, QStringLiteral("Drive"),
                                       VNX::ProjectItem::Type::Variable);
        ASSERT_NE(item_, nullptr);
        ASSERT_NE(variable_, nullptr);
        VNX::UndoRedoManager::instance().clear();
        item_->dirtyBit = false;
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir temp_;
    VNX::ProjectItem* item_ = nullptr;
    VNX::ProjectItem* variable_ = nullptr;
};

TEST_F(BindingEditingTest, UpsertUndoRedoAndRemoveUseOneLedgerPath)
{
    auto& manager = VNX::ProjectManager::instance();
    int changes = 0;
    QString changedProperty;
    const QMetaObject::Connection connection = QObject::connect(
        &manager, &VNX::ProjectManager::itemPropertyChanged,
        [&changes, &changedProperty](VNX::ProjectItem*, const QString& property) {
            ++changes;
            changedProperty = property;
        });
    const auto address = VNX::BindingSemantics::volumeAddress();
    auto binding = VNX::BindingSemantics::defaultBinding(address, variable_->guid);

    const auto upserted = VNX::ActionManager::instance().execute(
        QStringLiteral("set_binding"),
        VNX::BindingEditing::upsertArgs(item_, binding, QStringLiteral("Bind Volume")),
        VNX::ActionSource::GUI);
    ASSERT_TRUE(upserted.success);
    EXPECT_TRUE(item_->isDirtyObject());
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_NE(item_->comp<VNX::VMixNode>()->bindings.findFirst(address), nullptr);
    EXPECT_GT(changes, 0);
    EXPECT_EQ(changedProperty, VNX::BindingEditing::changeKey(address));

    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(item_->comp<VNX::VMixNode>()->bindings.findFirst(address), nullptr);
    VNX::UndoRedoManager::instance().undoStack()->redo();
    EXPECT_NE(item_->comp<VNX::VMixNode>()->bindings.findFirst(address), nullptr);

    const harkenstudio::ObjectId stableId =
        item_->comp<VNX::VMixNode>()->bindings.findFirst(address)->runtimeId();
    const auto removed = VNX::ActionManager::instance().execute(
        QStringLiteral("remove_binding"),
        VNX::BindingEditing::removeArgs(item_, stableId, QStringLiteral("Unbind Volume")),
        VNX::ActionSource::GUI);
    ASSERT_TRUE(removed.success);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 2);
    EXPECT_EQ(item_->comp<VNX::VMixNode>()->bindings.findFirst(address), nullptr);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_NE(item_->comp<VNX::VMixNode>()->bindings.findFirst(address), nullptr);
    QObject::disconnect(connection);
}


TEST_F(BindingEditingTest, TransitionOnlyEditIsDirtyAndUndoRedoRestoresExactDuration)
{
    const auto address = VNX::BindingSemantics::volumeAddress();
    auto binding = VNX::BindingSemantics::defaultBinding(address, variable_->guid);
    auto& actions = VNX::ActionManager::instance();
    ASSERT_TRUE(actions.execute(QStringLiteral("set_binding"),
        VNX::BindingEditing::upsertArgs(item_, binding, QStringLiteral("Bind Volume")),
        VNX::ActionSource::GUI).success);
    auto& ledger = item_->comp<VNX::VMixNode>()->bindings;
    ASSERT_NE(ledger.findFirst(address), nullptr);
    binding = *ledger.findFirst(address);
    EXPECT_EQ(binding.transitionMs, 20u);
    const auto id = binding.runtimeId();
    item_->dirtyBit = false;
    const int count = VNX::UndoRedoManager::instance().undoStack()->count();
    binding.transitionMs = 137;
    ASSERT_TRUE(actions.execute(QStringLiteral("set_binding"),
        VNX::BindingEditing::upsertArgs(item_, binding, QStringLiteral("Binding Response")),
        VNX::ActionSource::GUI).success);
    EXPECT_TRUE(item_->isDirtyObject());
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), count + 1);
    EXPECT_EQ(ledger.find(id)->transitionMs, 137u);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(ledger.find(id)->transitionMs, 20u);
    VNX::UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(ledger.find(id)->transitionMs, 137u);
}

TEST_F(BindingEditingTest, IdenticalBindingDoesNotAddUndoStep)
{
    const auto address = VNX::BindingSemantics::pitchAddress();
    const auto binding = VNX::BindingSemantics::defaultBinding(
        address, variable_->guid);
    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_binding"),
        VNX::BindingEditing::upsertArgs(item_, binding, QStringLiteral("Bind Pitch")),
        VNX::ActionSource::GUI).success);
    const int count = VNX::UndoRedoManager::instance().undoStack()->count();
    const auto canonical = *item_->comp<VNX::VMixNode>()->bindings.findFirst(address);
    EXPECT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_binding"),
        VNX::BindingEditing::upsertArgs(item_, canonical, QStringLiteral("Bind Pitch")),
        VNX::ActionSource::GUI).success);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), count);
}

TEST_F(BindingEditingTest, ReorderUndoRestoresLocalOrder)
{
    const auto address = VNX::BindingSemantics::volumeAddress();
    auto first = VNX::BindingSemantics::defaultBinding(address, variable_->guid);
    auto second = VNX::BindingSemantics::defaultBinding(
        address, QUuid::createUuid());
    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_binding"),
        VNX::BindingEditing::upsertArgs(item_, first, QStringLiteral("Add 1")),
        VNX::ActionSource::GUI).success);
    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_binding"),
        VNX::BindingEditing::upsertArgs(item_, second, QStringLiteral("Add 2")),
        VNX::ActionSource::GUI).success);
    const auto before = item_->comp<VNX::VMixNode>()->bindings.findAll(address);
    ASSERT_EQ(before.size(), 2u);
    const harkenstudio::ObjectId firstId = before[0]->runtimeId();
    const harkenstudio::ObjectId secondId = before[1]->runtimeId();
    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("reorder_bindings"),
        VNX::BindingEditing::reorderArgs(
            item_, address, 0, 1, QStringLiteral("Reorder")),
        VNX::ActionSource::GUI).success);
    const auto after = item_->comp<VNX::VMixNode>()->bindings.findAll(address);
    EXPECT_EQ(after[0]->runtimeId(), secondId);
    EXPECT_EQ(after[1]->runtimeId(), firstId);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    const auto undone = item_->comp<VNX::VMixNode>()->bindings.findAll(address);
    EXPECT_EQ(undone[0]->runtimeId(), firstId);
    EXPECT_EQ(undone[1]->runtimeId(), secondId);
}

TEST_F(BindingEditingTest, HostClassFollowsItemType)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* bus = manager.createItem(
        manager.getMasterBusItem(), QStringLiteral("BoundBus"),
        VNX::ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    EXPECT_EQ(VNX::BindingEditing::hostClassOf(item_), VNX::BindingHostClass::Voice);
    EXPECT_EQ(VNX::BindingEditing::hostClassOf(bus), VNX::BindingHostClass::Bus);
}

TEST_F(BindingEditingTest, GatedAddressIsRejectedByUpsert)
{
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable_->guid);
    binding.address.slotId = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::MakeUpGain);
    EXPECT_FALSE(VNX::BindingEditing::upsert(item_, binding));
    EXPECT_EQ(item_->comp<VNX::VMixNode>()->bindings.size(), 0u);
    EXPECT_FALSE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_binding"),
        VNX::BindingEditing::upsertArgs(item_, binding, QStringLiteral("Gate")),
        VNX::ActionSource::GUI).success);
}

TEST_F(BindingEditingTest, ReplaceRejectsMixedHostClasses)
{
    auto voice = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Sound),
        variable_->guid);
    voice.guid = VNX::authoringUuid(harkentest::StudioObjectId(1));
    auto bus = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus),
        variable_->guid);
    bus.guid = VNX::authoringUuid(harkentest::StudioObjectId(2));
    VNX::BindingLedger ledger;
    EXPECT_FALSE(ledger.replace({voice, bus}));
    EXPECT_TRUE(ledger.replace({voice}));
}

TEST_F(BindingEditingTest, FieldValuesAndOrderedRelationsSurviveSavingAndReopening)
{
    using namespace VNX;
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    registerProjectActions(actions, &manager, nullptr, {});
    ProjectItem* secondVariable = manager.createItem(
        nullptr, QStringLiteral("Distance"), ProjectItem::Type::Variable);
    ProjectItem* bus = manager.createItem(
        manager.getMasterBusItem(), QStringLiteral("BoundBus"), ProjectItem::Type::Bus);
    ASSERT_NE(secondVariable, nullptr);
    ASSERT_NE(bus, nullptr);
    const QUuid soundGuid = item_->guid;
    const QUuid busGuid = bus->guid;
    const QUuid firstVariableGuid = variable_->guid;
    const QUuid secondVariableGuid = secondVariable->guid;
    const auto setField = [&](ProjectItem* item, const QString& field, double value) {
        return actions.execute(QStringLiteral("set_item_property"),
            QJsonObject{
                {QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
                {QStringLiteral("property"), field},
                {QStringLiteral("override_operation"), QStringLiteral("set_override")},
                {QStringLiteral("value"), value}}, ActionSource::GUI).success;
    };
    ASSERT_TRUE(setField(item_, QStringLiteral("volume"), -7.5));
    ASSERT_TRUE(setField(item_, QStringLiteral("pitch"), 2.5));
    ASSERT_TRUE(setField(bus, QStringLiteral("volume"), -11.0));
    for (ProjectItem* host : {item_, bus}) {
        const auto address = BindingSemantics::volumeAddress(host->type);
        auto first = BindingSemantics::defaultBinding(address, firstVariableGuid);
        first.curve = {{0.0f, -18.0f}, {0.5f, -6.0f}, {1.0f, 0.0f}};
        first.handles = {{0.1f, -16.0f, 0.3f, -8.0f},
                         {0.7f, -3.0f, 0.9f, -1.0f}};
        first.outputMin = -24.0f;
        first.outputMax = 6.0f;
        auto second = BindingSemantics::defaultBinding(address, secondVariableGuid);
        second.curve = {{0.0f, 6.0f}, {1.0f, -6.0f}};
        second.handles = {{0.2f, 5.0f, 0.8f, -5.0f}};
        second.outputMin = -6.0f;
        second.outputMax = 6.0f;
        for (const auto& relation : {first, second}) {
            ASSERT_TRUE(actions.execute(QStringLiteral("set_binding"),
                BindingEditing::upsertArgs(host, relation, QStringLiteral("Bind Volume")),
                ActionSource::GUI).success);
        }
        ASSERT_TRUE(actions.execute(QStringLiteral("reorder_bindings"),
            BindingEditing::reorderArgs(host, address, 0, 1, QStringLiteral("Reorder")),
            ActionSource::GUI).success);
        const auto ordered = host->comp<VMixNode>()->bindings.findAll(address);
        ASSERT_EQ(ordered.size(), 2u);
        EXPECT_EQ(ordered[0]->variable, secondVariableGuid);
        EXPECT_EQ(ordered[1]->variable, firstVariableGuid);
    }
    const auto pitch = BindingSemantics::defaultBinding(
        BindingSemantics::pitchAddress(), firstVariableGuid);
    ASSERT_TRUE(actions.execute(QStringLiteral("set_binding"),
        BindingEditing::upsertArgs(item_, pitch, QStringLiteral("Bind Pitch")),
        ActionSource::GUI).success);
    const QJsonArray soundRelations = item_->comp<VMixNode>()->bindings.toJson();
    const QJsonArray busRelations = bus->comp<VMixNode>()->bindings.toJson();
    ASSERT_EQ(soundRelations.size(), 3);
    ASSERT_EQ(busRelations.size(), 2);
    const QString projectPath = manager.currentProject()->projectPath;

    const auto verifySavedDocument = [&](const QUuid& guid,
                                          const QJsonArray& expectedRelations,
                                          double expectedVolume) {
        QDirIterator files(QFileInfo(projectPath).absolutePath(),
                           {QStringLiteral("*.vp")}, QDir::Files,
                           QDirIterator::Subdirectories);
        bool found = false;
        while (files.hasNext()) {
            QFile file(files.next());
            ASSERT_TRUE(file.open(QIODevice::ReadOnly));
            const QByteArray bytes = file.readAll();
            EXPECT_FALSE(bytes.contains("bindingAttachmentRef"));
            const QJsonObject document = QJsonDocument::fromJson(bytes).object();
            if (document.value(QStringLiteral("harkenDocument")).toObject()
                    .value(QStringLiteral("id")).toString()
                != guid.toString(QUuid::WithoutBraces)) continue;
            found = true;
            const QJsonObject content = document.value(QStringLiteral("content")).toObject();
            const QJsonObject properties =
                content.value(QStringLiteral("playableProperties")).toObject();
            EXPECT_DOUBLE_EQ(properties.value(QStringLiteral("volume")).toDouble(),
                             expectedVolume);
            EXPECT_FALSE(properties.contains(QStringLiteral("bindings")));
            bool presenceValid = false;
            const uint64_t presence = properties.value(QStringLiteral("presence"))
                                          .toString().toULongLong(&presenceValid, 16);
            EXPECT_TRUE(presenceValid);
            EXPECT_EQ(presence & (UINT64_C(1) << 14), 0u);
            EXPECT_EQ(content.value(QStringLiteral("components")).toObject()
                          .value(QStringLiteral("mixnode")).toObject()
                          .value(QStringLiteral("bindings")).toArray(), expectedRelations);
        }
        EXPECT_TRUE(found);
    };

    for (int pass = 0; pass < 2; ++pass) {
        SCOPED_TRACE(pass);
        const float soundVolume = pass == 0 ? -7.5f : -3.0f;
        const float busVolume = pass == 0 ? -11.0f : -4.0f;
        if (pass == 1) {
            PropertyEditSession soundEdit(item_, QStringLiteral("volume"), QVariant(-7.5f));
            PropertyEditSession busEdit(bus, QStringLiteral("volume"), QVariant(-11.0f));
            soundEdit.preview(QVariant(soundVolume));
            busEdit.preview(QVariant(busVolume));
            ASSERT_TRUE(manager.saveProject());
            EXPECT_FALSE(soundEdit.isActive());
            EXPECT_FALSE(busEdit.isActive());
        } else {
            ASSERT_TRUE(manager.saveProject());
        }
        ASSERT_NO_FATAL_FAILURE(verifySavedDocument(soundGuid, soundRelations, soundVolume));
        ASSERT_NO_FATAL_FAILURE(verifySavedDocument(busGuid, busRelations, busVolume));
        ASSERT_TRUE(manager.closeProject());
        item_ = nullptr;
        variable_ = nullptr;
        ASSERT_TRUE(manager.loadProject(projectPath));
        item_ = manager.findItemByGuid(soundGuid);
        bus = manager.findItemByGuid(busGuid);
        variable_ = manager.findItemByGuid(firstVariableGuid);
        ASSERT_NE(item_, nullptr);
        ASSERT_NE(bus, nullptr);
        ASSERT_NE(variable_, nullptr);
        ASSERT_NE(manager.findItemByGuid(secondVariableGuid), nullptr);
        EXPECT_FLOAT_EQ(item_->comp<VMixNode>()->volume, soundVolume);
        EXPECT_FLOAT_EQ(item_->comp<VMixNode>()->pitch, 2.5f);
        EXPECT_FLOAT_EQ(bus->comp<VMixNode>()->volume, busVolume);
        EXPECT_EQ(item_->comp<VMixNode>()->bindings.toJson(), soundRelations);
        EXPECT_EQ(bus->comp<VMixNode>()->bindings.toJson(), busRelations);
        EXPECT_EQ(item_->playablePropertyPresence.bits & (UINT64_C(1) << 14), 0u);
        EXPECT_EQ(bus->playablePropertyPresence.bits & (UINT64_C(1) << 14), 0u);
    }
}
