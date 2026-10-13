#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "ActionManager.h"
#include "AttenuationStore.h"
#include "Components/VSpatial.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>

namespace {

constexpr auto kAttenuationProperty =
    harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef;

harkenprojectschema::AuthoringGuid authoringGuid(const QUuid& guid)
{
    const auto bytes = guid.toRfc4122();
    return {qFromBigEndian<quint64>(bytes.constData()),
            qFromBigEndian<quint64>(bytes.constData() + 8)};
}

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class AttenuationBindingUndoTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerSpatialActions(VNX::ActionManager::instance(), nullptr,
                                    &VNX::ProjectManager::instance());

        ASSERT_TRUE(tempDir_.isValid());
        VNX::ProjectConfig config;
        config.platforms = HarkenTest::platforms({"Windows", "Android"});
        config.name = QStringLiteral("BindingUndoProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(VNX::ProjectManager::instance().createProject(config));

        const auto created = VNX::ActionManager::instance().execute(
            QStringLiteral("create_attenuation"),
            QJsonObject{{"name", QStringLiteral("TestAtt")}});
        ASSERT_TRUE(created.success);
        attenuationGuid_ = QUuid::fromString(
            created.data.value(QStringLiteral("guid")).toString());
        ASSERT_FALSE(attenuationGuid_.isNull());
        auto* project = VNX::ProjectManager::instance().currentProject();
        const auto* attenuation = VNX::AttenuationStore::findByGuid(project, attenuationGuid_);
        ASSERT_NE(attenuation, nullptr);
        attenuationStableId_ = VNX::AttenuationStore::runtimeId(*project, *attenuation);
        ASSERT_NE(attenuationStableId_, harkentest::StudioObjectId(0u));

        auto* layer = VNX::ProjectManager::instance().createItem(
            nullptr, QStringLiteral("TestLayer"), VNX::ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        item_ = VNX::ProjectManager::instance().createItem(
            layer, QStringLiteral("TestSound"), VNX::ProjectItem::Type::Sound);
        ASSERT_NE(item_, nullptr);
        ASSERT_NE(item_->objectID, 0u);

        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ProjectManager::instance().closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    VNX::ActionResult bindTestAtt()
    {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("bind_attenuation_to_object"),
            QJsonObject{
                {"object_id", static_cast<qint64>(item_->objectID)},
                {"attenuation", QStringLiteral("TestAtt")}
            });
    }

    VNX::ActionResult unbind()
    {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("unbind_attenuation_from_object"),
            QJsonObject{{"object_id", static_cast<qint64>(item_->objectID)}});
    }

    const VNX::VSpatial* spatial() const { return item_->comp<VNX::VSpatial>(); }

    QUndoStack* stack() { return VNX::UndoRedoManager::instance().undoStack(); }

    QTemporaryDir tempDir_;
    VNX::ProjectItem* item_ = nullptr;
    QUuid attenuationGuid_;
    harkenstudio::ObjectId attenuationStableId_ = harkentest::StudioObjectId(0);
};

}

TEST_F(AttenuationBindingUndoTest, LegacyUnbindRejectsLocalRatingWithoutUndo)
{
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = authoringGuid(attenuationGuid_);
    item_->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    const auto before = item_->fieldGroups;
    const auto oldReference = spatial()->attenuationRef;
    const int base = stack()->count();
    const auto result = unbind();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_COMMAND_REQUIRED"));
    EXPECT_EQ(item_->fieldGroups, before);
    EXPECT_EQ(spatial()->attenuationRef, oldReference);
    EXPECT_FALSE(item_->playablePropertyPresence.Has(kAttenuationProperty));
    EXPECT_EQ(stack()->count(), base);
}

TEST_F(AttenuationBindingUndoTest, LegacyBindDoesNotActivateOrReplaceInheritedHistory)
{
    VNX::SpatialRatingFieldGroup history;
    history.spatialAttenuationDistanceScaling = 2.3f;
    item_->fieldGroups.spatialRating.shared.localHistory = history;
    const auto before = item_->fieldGroups;
    const auto result = bindTestAtt();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_COMMAND_REQUIRED"));
    EXPECT_EQ(item_->fieldGroups, before);
    EXPECT_EQ(spatial()->attenuationRef, harkentest::StudioObjectId(0u));
    EXPECT_FALSE(item_->playablePropertyPresence.Has(kAttenuationProperty));
    EXPECT_EQ(stack()->count(), 0);
}

TEST_F(AttenuationBindingUndoTest, RenamePreservesReferenceAndUndoIdentity)
{
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = authoringGuid(attenuationGuid_);
    item_->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    const auto before = item_->fieldGroups;
    ASSERT_TRUE(VNX::ProjectManager::instance().saveProject());
    const auto renamed = VNX::ActionManager::instance().execute(
        QStringLiteral("rename_attenuation"),
        QJsonObject{{"old_name", QStringLiteral("TestAtt")},
                    {"new_name", QStringLiteral("RenamedAtt")}});
    ASSERT_TRUE(renamed.success) << renamed.errorMessage.toStdString();
    auto* project = VNX::ProjectManager::instance().currentProject();
    const auto* attenuation = VNX::AttenuationStore::findByStableId(project, attenuationStableId_);
    ASSERT_NE(attenuation, nullptr);
    EXPECT_EQ(attenuation->hostItem()->name, QStringLiteral("RenamedAtt"));
    EXPECT_EQ(item_->fieldGroups, before);
    EXPECT_FALSE(item_->dirtyBit);
    stack()->undo();
    EXPECT_EQ(attenuation->hostItem()->name, QStringLiteral("TestAtt"));
    EXPECT_EQ(item_->fieldGroups, before);
}

TEST_F(AttenuationBindingUndoTest, CreateRedoPreservesIdentityAfterLegacyBindIsRejected)
{
    auto& actions = VNX::ActionManager::instance();
    ASSERT_TRUE(actions.execute(QStringLiteral("create_attenuation"),
                                QJsonObject{{"name", QStringLiteral("CreatedAtt")}}).success);
    auto* project = VNX::ProjectManager::instance().currentProject();
    const auto* attenuation = VNX::AttenuationStore::find(project, QStringLiteral("CreatedAtt"));
    ASSERT_NE(attenuation, nullptr);
    const harkenstudio::ObjectId stableId = VNX::AttenuationStore::runtimeId(*project, *attenuation);
    const auto before = item_->fieldGroups;
    const auto binding = actions.execute(
        QStringLiteral("bind_attenuation_to_object"),
        QJsonObject{{"object_id", static_cast<qint64>(item_->objectID)},
                     {"attenuation", QStringLiteral("CreatedAtt")}});
    EXPECT_FALSE(binding.success);
    EXPECT_EQ(binding.errorCode, QStringLiteral("FIELD_GROUP_COMMAND_REQUIRED"));

    stack()->undo();
    EXPECT_EQ(VNX::AttenuationStore::findByStableId(project, stableId), nullptr);
    stack()->redo();
    ASSERT_NE(VNX::AttenuationStore::findByStableId(project, stableId), nullptr);
    EXPECT_EQ(item_->fieldGroups, before);
}

TEST_F(AttenuationBindingUndoTest, DeleteUndoRestoresStableIdentity)
{
    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("delete_attenuation"),
        QJsonObject{{"name", QStringLiteral("TestAtt")}});
    ASSERT_TRUE(result.success);
    auto* project = VNX::ProjectManager::instance().currentProject();
    EXPECT_EQ(VNX::AttenuationStore::findByStableId(project, attenuationStableId_), nullptr);
    stack()->undo();
    const auto* restored = VNX::AttenuationStore::findByStableId(project, attenuationStableId_);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->hostItem()->guid, attenuationGuid_);
}

TEST_F(AttenuationBindingUndoTest, DeleteClearsActiveAndDormantPlatformReferences)
{
    auto* project = VNX::ProjectManager::instance().currentProject();
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = authoringGuid(attenuationGuid_);
    item_->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {VNX::FieldGroupMode::Local, rating};
    const auto before = item_->fieldGroups;
    item_->clearDirty();
    auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("delete_attenuation"),
        QJsonObject{{"name", QStringLiteral("TestAtt")}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(VNX::AttenuationStore::findByStableId(project, attenuationStableId_), nullptr);
    EXPECT_TRUE(harkenprojectschema::IsNull(item_->fieldGroups.spatialRating.platforms.at(
        HarkenTest::FieldGroupPlatformIds::Windows).localHistory->spatialAttenuationRef));
    EXPECT_TRUE(item_->dirtyBit);
    stack()->undo();
    EXPECT_EQ(item_->fieldGroups, before);

    item_->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows].mode =
        VNX::FieldGroupMode::Inherit;
    const auto* attenuation = VNX::AttenuationStore::findByStableId(project, attenuationStableId_);
    ASSERT_NE(attenuation, nullptr);
    EXPECT_EQ(VNX::AttenuationStore::referenceCount(project, *attenuation->hostItem()), 0);
    result = VNX::ActionManager::instance().execute(
        QStringLiteral("delete_attenuation"),
        QJsonObject{{"name", QStringLiteral("TestAtt")}});
    EXPECT_TRUE(result.success);
    EXPECT_EQ(VNX::AttenuationStore::findByStableId(project, attenuationStableId_), nullptr);
    const auto& history = item_->fieldGroups.spatialRating.platforms.at(
        HarkenTest::FieldGroupPlatformIds::Windows).localHistory;
    ASSERT_TRUE(history);
    EXPECT_TRUE(harkenprojectschema::IsNull(history->spatialAttenuationRef));
    EXPECT_EQ(item_->fieldGroups.spatialRating.platforms.at(
        HarkenTest::FieldGroupPlatformIds::Windows).mode, VNX::FieldGroupMode::Inherit);
    stack()->undo();
    EXPECT_EQ(item_->fieldGroups.spatialRating.platforms.at(
        HarkenTest::FieldGroupPlatformIds::Windows).localHistory->spatialAttenuationRef,
        authoringGuid(attenuationGuid_));
}

TEST_F(AttenuationBindingUndoTest, RootInheritedRatingClearsAndUndoRestoresAcrossSave)
{
    auto& manager = VNX::ProjectManager::instance();
    auto* project = manager.currentProject();
    auto* root = item_->parent;
    ASSERT_NE(root, nullptr);
    auto& rating = root->fieldGroups.spatialRating.shared;
    ASSERT_EQ(rating.mode, VNX::FieldGroupMode::Inherit);
    ASSERT_TRUE(rating.localHistory);
    rating.localHistory->spatialAttenuationRef = authoringGuid(attenuationGuid_);
    const auto before = root->fieldGroups;
    const auto* attenuation = VNX::AttenuationStore::findByGuid(project, attenuationGuid_);
    ASSERT_NE(attenuation, nullptr);
    QString error;
    const auto references = VNX::AttenuationStore::references(project, *attenuation->hostItem(), &error);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(references.size(), 1u);
    EXPECT_EQ(references.front().item, root);

    auto* shell = manager.findItemByGuid(attenuationGuid_);
    ASSERT_NE(shell, nullptr);
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.deleteItem(shell));
    EXPECT_EQ(manager.findItemByGuid(attenuationGuid_), nullptr);
    EXPECT_TRUE(harkenprojectschema::IsNull(root->fieldGroups.spatialRating.shared.localHistory->spatialAttenuationRef));
    EXPECT_TRUE(root->dirtyBit);
    EXPECT_EQ(spatial()->attenuationRef, harkentest::StudioObjectId(0u));
    ASSERT_TRUE(manager.saveProject());
    stack()->undo();
    EXPECT_EQ(manager.findItemByGuid(attenuationGuid_), shell);
    EXPECT_EQ(root->fieldGroups, before);
    EXPECT_TRUE(root->dirtyBit);
    EXPECT_EQ(spatial()->attenuationRef, attenuationStableId_);
    stack()->redo();
    EXPECT_FALSE(root->dirtyBit);
    root->notes = QStringLiteral("Keep pending notes");
    root->markDirty();
    emit manager.itemPropertyChanged(root, QStringLiteral("notes"));
    stack()->undo();
    EXPECT_EQ(root->fieldGroups, before);
    stack()->redo();
    EXPECT_TRUE(root->dirtyBit);
    EXPECT_EQ(root->notes, QStringLiteral("Keep pending notes"));
    stack()->undo();
    ASSERT_TRUE(manager.saveProject());
    const auto rootGuid = root->guid;
    const auto projectPath = project->projectPath;
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    EXPECT_EQ(manager.findItemByGuid(rootGuid)->fieldGroups, before);
    EXPECT_NE(manager.findItemByGuid(attenuationGuid_), nullptr);
}

TEST_F(AttenuationBindingUndoTest, RootInheritFlagDoesNotHideItsOwnReferenceFields)
{
    VNX::registerProjectActions(VNX::ActionManager::instance(), &VNX::ProjectManager::instance(), nullptr, {});
    auto* project = VNX::ProjectManager::instance().currentProject();
    auto* root = item_->parent;
    ASSERT_NE(root, nullptr);
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = authoringGuid(attenuationGuid_);
    root->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {VNX::FieldGroupMode::Inherit, rating};
    const auto before = root->fieldGroups;
    const auto* attenuation = VNX::AttenuationStore::findByGuid(project, attenuationGuid_);
    ASSERT_NE(attenuation, nullptr);
    QString error;
    EXPECT_EQ(VNX::AttenuationStore::referenceCount(project, *attenuation->hostItem(), &error), 1);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("delete_item"),
        QJsonObject{{"object_id", static_cast<qint64>(attenuation->hostItem()->objectID)}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_TRUE(harkenprojectschema::IsNull(root->fieldGroups.spatialRating.platforms.at(
        HarkenTest::FieldGroupPlatformIds::Windows).localHistory->spatialAttenuationRef));
    stack()->undo();
    EXPECT_EQ(root->fieldGroups, before);

    auto& platform = root->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows];
    platform.mode = VNX::FieldGroupMode::UseShared;
    EXPECT_EQ(VNX::AttenuationStore::referenceCount(project, *attenuation->hostItem(), &error), 1);
    platform.localHistory.reset();
    EXPECT_EQ(VNX::AttenuationStore::referenceCount(project, *attenuation->hostItem(), &error), 0);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    result = VNX::ActionManager::instance().execute(
        QStringLiteral("delete_attenuation"),
        QJsonObject{{"name", QStringLiteral("TestAtt")}});
    EXPECT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(VNX::AttenuationStore::findByGuid(project, attenuationGuid_), nullptr);
    EXPECT_FALSE(platform.localHistory);
}

TEST_F(AttenuationBindingUndoTest, DeleteClearsChildInheritedHistoryAndUndoRestoresAllParameters)
{
    auto* project = VNX::ProjectManager::instance().currentProject();
    VNX::SpatialRatingFieldGroup history;
    history.spatialAttenuationRef = authoringGuid(attenuationGuid_);
    history.spatialAttenuationDistanceScaling = 2.3f;
    item_->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Inherit, history};
    item_->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        VNX::FieldGroupMode::Inherit, history};
    item_->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        VNX::FieldGroupMode::UseShared, history};
    const auto before = item_->fieldGroups;
    auto expected = before;
    expected.spatialRating.shared.localHistory->spatialAttenuationRef = {};
    for (auto& [platform, state] : expected.spatialRating.platforms)
        state.localHistory->spatialAttenuationRef = {};
    item_->clearDirty();
    const auto* attenuation = VNX::AttenuationStore::findByGuid(project, attenuationGuid_);
    ASSERT_NE(attenuation, nullptr);
    QString error;
    EXPECT_EQ(VNX::AttenuationStore::referenceCount(project, *attenuation->hostItem(), &error), 0);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("delete_attenuation"),
        QJsonObject{{"name", QStringLiteral("TestAtt")}});
    EXPECT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(VNX::AttenuationStore::findByGuid(project, attenuationGuid_), nullptr);
    EXPECT_EQ(item_->fieldGroups, expected);
    EXPECT_TRUE(item_->dirtyBit);
    ASSERT_TRUE(VNX::ProjectManager::instance().saveProject());
    stack()->undo();
    EXPECT_EQ(item_->fieldGroups, before);
    EXPECT_TRUE(item_->dirtyBit);
    EXPECT_NE(VNX::AttenuationStore::findByGuid(project, attenuationGuid_), nullptr);
}

TEST_F(AttenuationBindingUndoTest, OwnershipEdgesKeepChildHistoryDormantWithoutABackPointer)
{
    auto* project = VNX::ProjectManager::instance().currentProject();
    VNX::SpatialRatingFieldGroup history;
    history.spatialAttenuationRef = authoringGuid(attenuationGuid_);
    item_->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Inherit, history};
    const auto* attenuation = VNX::AttenuationStore::findByGuid(project, attenuationGuid_);
    ASSERT_NE(attenuation, nullptr);
    auto* parent = item_->parent;
    item_->parent = nullptr;
    QString error;
    const auto references = VNX::AttenuationStore::references(project, *attenuation->hostItem(), &error);
    item_->parent = parent;
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_TRUE(references.empty());
    EXPECT_EQ(item_->fieldGroups.spatialRating.shared.localHistory, history);
}

TEST_F(AttenuationBindingUndoTest, InvalidOwnershipRejectsReferenceQueriesAndDeletionWithoutMutation)
{
    auto& manager = VNX::ProjectManager::instance();
    auto& actions = VNX::ActionManager::instance();
    VNX::registerProjectActions(actions, &manager, nullptr, {});
    auto* project = manager.currentProject();
    auto* parent = item_->parent;
    ASSERT_NE(parent, nullptr);
    const auto parentType = parent->type;
    const auto parentBefore = parent->fieldGroups;
    const auto itemBefore = item_->fieldGroups;
    parent->clearDirty();
    const auto* attenuation = VNX::AttenuationStore::findByGuid(project, attenuationGuid_);
    ASSERT_NE(attenuation, nullptr);
    const auto& identity = *attenuation->hostItem();
    for (const bool illegalKind : {false, true}) {
        SCOPED_TRACE(illegalKind);
        const auto corrupt = [&] {
            if (illegalKind) parent->type = VNX::ProjectItem::Type::Piece;
            else item_->parent = manager.getMasterBusItem();
        };
        const auto restore = [&] {
            parent->type = parentType;
            item_->parent = parent;
        };
        corrupt();
        QString error;
        EXPECT_EQ(VNX::AttenuationStore::referenceCount(project, identity, &error), -1);
        EXPECT_FALSE(error.isEmpty());
        error.clear();
        EXPECT_TRUE(VNX::AttenuationStore::referencesJson(project, identity, &error).isEmpty());
        EXPECT_FALSE(error.isEmpty());
        const auto spatialDelete = actions.execute(QStringLiteral("delete_attenuation"),
            QJsonObject{{"name", QStringLiteral("TestAtt")}});
        restore();
        EXPECT_FALSE(spatialDelete.success);
        EXPECT_EQ(spatialDelete.errorCode, QStringLiteral("INVALID_PROJECT"));
        auto* shell = manager.findItemByGuid(attenuationGuid_);
        ASSERT_NE(shell, nullptr);

        corrupt();
        const auto itemDelete = actions.execute(QStringLiteral("delete_item"),
            QJsonObject{{"object_id", static_cast<qint64>(shell->objectID)}});
        restore();
        EXPECT_FALSE(itemDelete.success);
        EXPECT_EQ(itemDelete.errorCode, QStringLiteral("INVALID_PROJECT"));
        ASSERT_EQ(manager.findItemByGuid(attenuationGuid_), shell);

        corrupt();
        const bool deleted = manager.deleteItem(shell);
        restore();
        EXPECT_FALSE(deleted);
        EXPECT_EQ(manager.findItemByGuid(attenuationGuid_), shell);
        EXPECT_EQ(parent->fieldGroups, parentBefore);
        EXPECT_EQ(item_->fieldGroups, itemBefore);
        EXPECT_FALSE(parent->dirtyBit);
        EXPECT_FALSE(item_->dirtyBit);
        EXPECT_EQ(stack()->count(), 0);
    }
}

TEST_F(AttenuationBindingUndoTest, UnbindWithoutBindingFailsAndPushesNothing)
{
    const auto result = unbind();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_COMMAND_REQUIRED"));
    EXPECT_EQ(stack()->count(), 0);
}

TEST_F(AttenuationBindingUndoTest, BindOnBusFailsWithNoComponentAndPushesNothing)
{
    auto* master = VNX::ProjectManager::instance().getMasterBusItem();
    ASSERT_NE(master, nullptr);
    auto* bus = VNX::ProjectManager::instance().createItem(
        master, QStringLiteral("TestBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(bus, nullptr);
    ASSERT_EQ(bus->comp<VNX::VSpatial>(), nullptr);
    const int base = stack()->count();

    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("bind_attenuation_to_object"),
        QJsonObject{
            {"object_id", static_cast<qint64>(bus->objectID)},
            {"attenuation", QStringLiteral("TestAtt")}
        });
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("NO_COMPONENT"));
    EXPECT_EQ(stack()->count(), base);

    const auto unbindResult = VNX::ActionManager::instance().execute(
        QStringLiteral("unbind_attenuation_from_object"),
        QJsonObject{{"object_id", static_cast<qint64>(bus->objectID)}});
    EXPECT_FALSE(unbindResult.success);
    EXPECT_EQ(unbindResult.errorCode, QStringLiteral("NO_COMPONENT"));
    EXPECT_EQ(stack()->count(), base);
}
