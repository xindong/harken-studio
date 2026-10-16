#include "UnitTest/StudioObjectIdTestSupport.h"
#include "NameValidator.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "ProjectObjectIndex.h"
#include "Components/VMixNode.h"
#include "Components/VTrack.h"
#include "Components/VPiece.h"
#include "UndoCommands.h"

#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace VNX;

namespace {

using TestSupport::ensureApp;

QJsonObject readJsonFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject documentContent(const QJsonObject& document)
{
    return document.value(QStringLiteral("content")).toObject();
}

const std::vector<ActionPublishedNameHistoryRow>& historyOf(ProjectManager& manager)
{
    return manager.currentProject()->actionPublishedNameHistory;
}

std::vector<ActionPublishedNameHistoryRow> historyForGuid(
    ProjectManager& manager, const QUuid& guid)
{
    std::vector<ActionPublishedNameHistoryRow> rows;
    for (const auto& row : historyOf(manager)) {
        if (row.objectGuid == guid) rows.push_back(row);
    }
    return rows;
}

class ActionIdentityTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ActionIdentityProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        projectPath_ = manager.currentProject()->projectPath;
        projectRoot_ = QFileInfo(projectPath_).absolutePath();
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    ProjectItem* createAction(const QString& name, const QString& path = QString())
    {
        return ProjectManager::instance().createItem(
            nullptr, name, ProjectItem::Type::Action, path);
    }

    QString actionFile(const QString& relativePath) const
    {
        return QDir(projectRoot_).filePath(QStringLiteral("Actions/") + relativePath);
    }

    QString harkenFile() const { return projectPath_; }

    void reopen()
    {
        auto& manager = ProjectManager::instance();
        ASSERT_TRUE(manager.closeProject());
        ASSERT_TRUE(manager.loadProject(projectPath_));
    }

    QTemporaryDir tempDir_;
    QString projectPath_;
    QString projectRoot_;
};

}

TEST_F(ActionIdentityTest, RenameRetainsIds)
{
    auto& manager = ProjectManager::instance();
    ASSERT_NE(manager.createFolder(nullptr, QStringLiteral("Combat"), QStringLiteral("Actions")),
              nullptr);

    ProjectItem* action = createAction(QStringLiteral("Play_A"), QStringLiteral("Combat"));
    ASSERT_NE(action, nullptr);
    const QUuid guid = action->guid;
    const harkenstudio::ObjectId runtimeStableId = action->runtimeId();
    EXPECT_EQ(action->nameBindingRevision, 1u);
    EXPECT_EQ(action->name, QStringLiteral("Play_A"));
    EXPECT_EQ(manager.currentProject()->itemFolderPath(action), QStringLiteral("Combat"));

    ASSERT_TRUE(manager.saveProject());
    ASSERT_NO_FATAL_FAILURE(reopen());
    action = manager.findItemByGuid(guid);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->guid, guid);
    EXPECT_EQ(action->runtimeId(), runtimeStableId);
    EXPECT_EQ(action->name, QStringLiteral("Play_A"));
    EXPECT_EQ(action->nameBindingRevision, 1u);
    EXPECT_EQ(manager.currentProject()->itemFolderPath(action), QStringLiteral("Combat"));

    ASSERT_TRUE(manager.renameItem(action, QStringLiteral("Play_B")));
    EXPECT_EQ(action->guid, guid);
    EXPECT_EQ(action->runtimeId(), runtimeStableId);
    EXPECT_EQ(action->name, QStringLiteral("Play_B"));
    EXPECT_EQ(action->nameBindingRevision, 2u);

    ASSERT_TRUE(manager.saveProject());
    const QJsonObject savedAction =
        documentContent(readJsonFile(actionFile(QStringLiteral("Combat/")
            + NameValidator::objectFileName(QStringLiteral("Play_B"), guid))));
    EXPECT_EQ(savedAction.value(QStringLiteral("name")).toString(),
              QStringLiteral("Play_B"));
    EXPECT_EQ(savedAction.value(QStringLiteral("nameBindingRevision")).toInteger(), 2);
    EXPECT_FALSE(savedAction.contains(QStringLiteral("publicName")));

    ASSERT_NO_FATAL_FAILURE(reopen());
    action = manager.findItemByGuid(guid);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->guid, guid);
    EXPECT_EQ(action->runtimeId(), runtimeStableId);
    EXPECT_EQ(action->name, QStringLiteral("Play_B"));
    EXPECT_EQ(action->nameBindingRevision, 2u);
}

TEST_F(ActionIdentityTest, CreateSavePersistsAuthoredName)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* action = createAction(QStringLiteral("Play_Call"));
    ASSERT_NE(action, nullptr);
    const QUuid guid = action->guid;
    const harkenstudio::ObjectId runtimeStableId = action->runtimeId();
    EXPECT_EQ(action->name, QStringLiteral("Play_Call"));
    EXPECT_EQ(action->nameBindingRevision, 1u);
    const auto created = historyForGuid(manager, guid);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(created.front().event, QStringLiteral("created"));
    EXPECT_EQ(created.front().name, QStringLiteral("Play_Call"));
    EXPECT_EQ(created.front().runtimeId(), runtimeStableId);
    EXPECT_EQ(created.front().nameBindingRevision, 1u);
    EXPECT_TRUE(created.front().priorName.isEmpty());

    ASSERT_TRUE(manager.saveProject());
    const QJsonObject savedAction =
        documentContent(readJsonFile(actionFile(NameValidator::objectFileName(QStringLiteral("Play_Call"), guid))));
    EXPECT_EQ(savedAction.value(QStringLiteral("name")).toString(),
              QStringLiteral("Play_Call"));
    EXPECT_EQ(savedAction.value(QStringLiteral("nameBindingRevision")).toInteger(), 1);
    EXPECT_FALSE(savedAction.contains(QStringLiteral("publicName")));
    EXPECT_FALSE(savedAction.value(QStringLiteral("extensions")).toObject()
                     .contains(QStringLiteral("actionPublishedNameHistory")));

    const QJsonObject savedRoot = documentContent(readJsonFile(harkenFile()));
    EXPECT_FALSE(savedRoot.contains(QStringLiteral("publicName")));
    const QJsonArray savedHistory =
        savedRoot.value(QStringLiteral("actionPublishedNameHistory")).toArray();
    ASSERT_EQ(savedHistory.size(), 1);
    EXPECT_EQ(savedHistory.at(0).toObject().value(QStringLiteral("event")).toString(),
              QStringLiteral("created"));
    EXPECT_EQ(savedHistory.at(0).toObject().value(QStringLiteral("name")).toString(),
              QStringLiteral("Play_Call"));
    EXPECT_EQ(savedHistory.at(0).toObject().value(QStringLiteral("objectGuid")).toString(),
              VNX::objectIdString(runtimeStableId));
    EXPECT_FALSE(savedRoot.value(QStringLiteral("extensions")).toObject()
                     .contains(QStringLiteral("actionPublishedNameHistory")));

    ASSERT_NO_FATAL_FAILURE(reopen());
    action = manager.findItemByGuid(guid);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->name, QStringLiteral("Play_Call"));
    EXPECT_EQ(action->guid, guid);
    EXPECT_EQ(action->runtimeId(), runtimeStableId);
    EXPECT_EQ(action->nameBindingRevision, 1u);
    const auto reloaded = historyForGuid(manager, guid);
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_EQ(reloaded.front().event, QStringLiteral("created"));
    EXPECT_EQ(reloaded.front().name, QStringLiteral("Play_Call"));
}

TEST_F(ActionIdentityTest, RenameUndoRedoIncrementsRevision)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* taken = createAction(QStringLiteral("Play_Taken"));
    ProjectItem* action = createAction(QStringLiteral("Play_A"));
    ASSERT_NE(taken, nullptr);
    ASSERT_NE(action, nullptr);
    const QUuid guid = action->guid;
    const harkenstudio::ObjectId runtimeStableId = action->runtimeId();
    const auto createdRows = historyForGuid(manager, guid);
    ASSERT_EQ(createdRows.size(), 1u);
    EXPECT_EQ(action->nameBindingRevision, 1u);

    const auto failedName = action->name;
    const auto failedRevision = action->nameBindingRevision;
    const auto failedHistory = historyOf(manager).size();
    EXPECT_FALSE(manager.renameItem(action, QString()));
    EXPECT_FALSE(manager.renameItem(action, QStringLiteral("Play/Invalid")));
    EXPECT_EQ(action->name, failedName);
    EXPECT_EQ(action->guid, guid);
    EXPECT_EQ(action->runtimeId(), runtimeStableId);
    EXPECT_EQ(action->nameBindingRevision, failedRevision);
    EXPECT_EQ(historyOf(manager).size(), failedHistory);

    ASSERT_TRUE(manager.renameItem(action, QStringLiteral("Play_B")));
    EXPECT_EQ(action->name, QStringLiteral("Play_B"));
    EXPECT_EQ(action->nameBindingRevision, 2u);
    auto rows = historyForGuid(manager, guid);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[1].event, QStringLiteral("renamed_from"));
    EXPECT_EQ(rows[1].name, QStringLiteral("Play_B"));
    EXPECT_EQ(rows[1].priorName, QStringLiteral("Play_A"));
    EXPECT_EQ(rows[1].nameBindingRevision, 2u);

    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(action->name, QStringLiteral("Play_A"));
    EXPECT_EQ(action->guid, guid);
    EXPECT_EQ(action->runtimeId(), runtimeStableId);
    EXPECT_EQ(action->nameBindingRevision, 3u);
    rows = historyForGuid(manager, guid);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[2].event, QStringLiteral("renamed_from"));
    EXPECT_EQ(rows[2].name, QStringLiteral("Play_A"));
    EXPECT_EQ(rows[2].priorName, QStringLiteral("Play_B"));
    EXPECT_EQ(rows[2].nameBindingRevision, 3u);

    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(action->name, QStringLiteral("Play_B"));
    EXPECT_EQ(action->nameBindingRevision, 4u);
    rows = historyForGuid(manager, guid);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[3].event, QStringLiteral("renamed_from"));
    EXPECT_EQ(rows[3].name, QStringLiteral("Play_B"));
    EXPECT_EQ(rows[3].priorName, QStringLiteral("Play_A"));

    ASSERT_TRUE(manager.renameItem(action, QStringLiteral("Play_A")));
    EXPECT_EQ(action->name, QStringLiteral("Play_A"));
    EXPECT_EQ(action->nameBindingRevision, 5u);
    rows = historyForGuid(manager, guid);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[4].event, QStringLiteral("renamed_from"));
    EXPECT_EQ(rows[4].name, QStringLiteral("Play_A"));
    EXPECT_EQ(rows[4].priorName, QStringLiteral("Play_B"));
    EXPECT_EQ(rows[4].nameBindingRevision, 5u);
    EXPECT_EQ(rows[0].event, QStringLiteral("created"));
}

TEST_F(ActionIdentityTest, CloneResetsNameBindingRevision)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* source = createAction(QStringLiteral("Play_Source"));
    ASSERT_NE(source, nullptr);
    ASSERT_TRUE(manager.renameItem(source, QStringLiteral("Play_Renamed")));
    const QUuid sourceGuid = source->guid;
    const harkenstudio::ObjectId sourceId = source->runtimeId();
    const auto sourceHistoryBefore = historyForGuid(manager, sourceGuid);
    ASSERT_EQ(sourceHistoryBefore.size(), 2u);
    EXPECT_EQ(source->nameBindingRevision, 2u);

    ProjectItem* clone = manager.duplicateItem(source);
    ASSERT_NE(clone, nullptr);
    EXPECT_NE(clone->guid, sourceGuid);
    EXPECT_NE(clone->runtimeId(), sourceId);
    EXPECT_EQ(clone->nameBindingRevision, 1u);
    const auto cloneHistory = historyForGuid(manager, clone->guid);
    ASSERT_EQ(cloneHistory.size(), 1u);
    EXPECT_EQ(cloneHistory.front().event, QStringLiteral("created"));
    EXPECT_EQ(cloneHistory.front().name, clone->name);
    const auto sourceHistoryAfter = historyForGuid(manager, sourceGuid);
    ASSERT_EQ(sourceHistoryAfter.size(), sourceHistoryBefore.size());
    EXPECT_EQ(source->nameBindingRevision, 2u);
}

TEST_F(ActionIdentityTest, MissingRevisionLoadsAsOne)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* action = createAction(QStringLiteral("Play_Legacy"));
    ASSERT_NE(action, nullptr);
    const QUuid guid = action->guid;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());

    const QString path = actionFile(NameValidator::objectFileName(QStringLiteral("Play_Legacy"), guid));
    QJsonObject document = readJsonFile(path);
    QJsonObject content = documentContent(document);
    ASSERT_TRUE(content.contains(QStringLiteral("nameBindingRevision")));
    content.remove(QStringLiteral("nameBindingRevision"));
    document.insert(QStringLiteral("content"), content);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();

    ASSERT_TRUE(manager.loadProject(projectPath_));
    action = manager.findItemByGuid(guid);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->nameBindingRevision, 1u);
    EXPECT_EQ(action->name, QStringLiteral("Play_Legacy"));
}

TEST_F(ActionIdentityTest, VariableRenamePreservesGuidAndReleasesFormerName)
{
    auto& manager = ProjectManager::instance();
    auto* variable = manager.createItem(nullptr, QStringLiteral("Speed"), ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    const auto guid = variable->guid;
    ASSERT_TRUE(manager.renameItem(variable, QStringLiteral("Velocity")));
    auto* replacement = manager.createItem(nullptr, QStringLiteral("Speed"), ProjectItem::Type::Variable);
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->name, QStringLiteral("Speed"));
    auto* clone = manager.duplicateItem(variable);
    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(clone->name, QStringLiteral("Velocity_01"));
    EXPECT_NE(clone->guid, guid);
    ASSERT_TRUE(manager.deleteItem(replacement));
    replacement = manager.createItem(nullptr, QStringLiteral("Speed"), ProjectItem::Type::Variable);
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->name, QStringLiteral("Speed"));
    ASSERT_TRUE(manager.saveProject());
    ASSERT_NO_FATAL_FAILURE(reopen());
    variable = manager.findItemByGuid(guid);
    ASSERT_NE(variable, nullptr);
    EXPECT_EQ(variable->name, QStringLiteral("Velocity"));
    EXPECT_EQ(variable->guid, guid);
}

TEST_F(ActionIdentityTest, NameReusePreservesIdentityAcrossRenameUndoRedo)
{
    auto& manager = ProjectManager::instance();
    for (auto type : {ProjectItem::Type::Action, ProjectItem::Type::Variable}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto* original = manager.createItem(nullptr, QStringLiteral("Name_A"), type);
        auto* other = manager.createItem(nullptr, QStringLiteral("Name_Other"), type);
        ASSERT_NE(original, nullptr);
        ASSERT_NE(other, nullptr);
        const QUuid originalGuid = original->guid;
        const QUuid otherGuid = other->guid;
        ASSERT_TRUE(manager.renameItem(original, QStringLiteral("Name_B")));

        auto* created = manager.createItem(nullptr, QStringLiteral("Name_A"), type);
        ASSERT_NE(created, nullptr);
        const QUuid createdGuid = created->guid;
        EXPECT_EQ(created->name, QStringLiteral("Name_A"));
        EXPECT_NE(createdGuid, originalGuid);
        ASSERT_TRUE(manager.renameItem(other, QStringLiteral("Name_A")));
        EXPECT_EQ(other->name, QStringLiteral("Name_A_01"));

        ASSERT_TRUE(manager.deleteItem(created));
        ASSERT_TRUE(manager.renameItem(other, QStringLiteral("Name_A")));
        EXPECT_EQ(other->name, QStringLiteral("Name_A"));
        EXPECT_EQ(other->guid, otherGuid);

        auto& undo = UndoRedoManager::instance();
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(other->name, QStringLiteral("Name_A_01"));
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(manager.findItemByGuid(createdGuid), created);
        EXPECT_EQ(created->name, QStringLiteral("Name_A"));
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(other->name, QStringLiteral("Name_Other"));
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(manager.findItemByGuid(createdGuid), nullptr);
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(original->name, QStringLiteral("Name_A"));
        EXPECT_EQ(original->guid, originalGuid);

        ASSERT_TRUE(undo.redo());
        EXPECT_EQ(original->name, QStringLiteral("Name_B"));
        ASSERT_TRUE(undo.redo());
        EXPECT_EQ(manager.findItemByGuid(createdGuid), created);
        EXPECT_EQ(created->name, QStringLiteral("Name_A"));
    }
}

TEST_F(ActionIdentityTest, NameReusePreservesIdentityAcrossDeleteUndoRedo)
{
    auto& manager = ProjectManager::instance();
    for (auto type : {ProjectItem::Type::Action, ProjectItem::Type::Variable}) {
        SCOPED_TRACE(static_cast<int>(type));
        auto* original = manager.createItem(nullptr, QStringLiteral("Name_Gone"), type);
        ASSERT_NE(original, nullptr);
        const QUuid originalGuid = original->guid;
        ASSERT_TRUE(manager.deleteItem(original));
        EXPECT_EQ(manager.findItemByGuid(originalGuid), nullptr);

        auto* replacement = manager.createItem(nullptr, QStringLiteral("Name_Gone"), type);
        ASSERT_NE(replacement, nullptr);
        const QUuid replacementGuid = replacement->guid;
        EXPECT_EQ(replacement->name, QStringLiteral("Name_Gone"));
        EXPECT_NE(replacementGuid, originalGuid);

        auto& undo = UndoRedoManager::instance();
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(manager.findItemByGuid(replacementGuid), nullptr);
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(manager.findItemByGuid(originalGuid), original);
        EXPECT_EQ(original->name, QStringLiteral("Name_Gone"));
        ASSERT_TRUE(undo.redo());
        EXPECT_EQ(manager.findItemByGuid(originalGuid), nullptr);
        ASSERT_TRUE(undo.redo());
        EXPECT_EQ(manager.findItemByGuid(replacementGuid), replacement);
        EXPECT_EQ(replacement->name, QStringLiteral("Name_Gone"));

        ASSERT_TRUE(undo.undo());
        auto* recreated = manager.createItem(nullptr, QStringLiteral("Name_Gone"), type);
        ASSERT_NE(recreated, nullptr);
        EXPECT_EQ(recreated->name, QStringLiteral("Name_Gone"));
        EXPECT_NE(recreated->guid, originalGuid);
        EXPECT_NE(recreated->guid, replacementGuid);
    }
}

TEST_F(ActionIdentityTest, NamesAvoidConflictsAcrossFolders)
{
    auto& manager = ProjectManager::instance();
    ASSERT_NE(manager.createFolder(nullptr, QStringLiteral("Combat"), QStringLiteral("Actions")),
              nullptr);
    ProjectItem* rootAction = createAction(QStringLiteral("Play_Same"));
    ASSERT_NE(rootAction, nullptr);
    auto* suffixed = createAction(QStringLiteral("Play_Same"), QStringLiteral("Combat"));
    ASSERT_NE(suffixed, nullptr);
    EXPECT_EQ(suffixed->name, QStringLiteral("Play_Same_01"));
    ProjectItem* nested = createAction(QStringLiteral("Play_Other"), QStringLiteral("Combat"));
    ASSERT_NE(nested, nullptr);
    EXPECT_TRUE(manager.renameItem(nested, QStringLiteral("Play_Same")));
    EXPECT_EQ(nested->name, QStringLiteral("Play_Same_02"));
    ProjectItem* lower = createAction(QStringLiteral("Play_a"));
    ProjectItem* upper = createAction(QStringLiteral("Play_A"));
    ASSERT_NE(lower, nullptr);
    ASSERT_NE(upper, nullptr);
    EXPECT_NE(lower->guid, upper->guid);
    EXPECT_EQ(createAction(QStringLiteral("Play/Bad")), nullptr);
    EXPECT_EQ(createAction(QString(NameValidator::MAX_NAME_LENGTH + 1, QLatin1Char('A'))),
              nullptr);
}

TEST_F(ActionIdentityTest, DeletedHistoryDoesNotReserveNameAfterReopen)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* action = createAction(QStringLiteral("Play_Keep"));
    ASSERT_NE(action, nullptr);
    const QUuid guid = action->guid;
    const harkenstudio::ObjectId runtimeStableId = action->runtimeId();
    ASSERT_TRUE(manager.deleteItem(action));
    ASSERT_TRUE(manager.saveProject());
    ASSERT_NO_FATAL_FAILURE(reopen());
    EXPECT_EQ(manager.findItemByGuid(guid), nullptr);
    const auto rows = historyForGuid(manager, guid);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows.back().event, QStringLiteral("deleted"));
    EXPECT_EQ(rows.back().name, QStringLiteral("Play_Keep"));
    EXPECT_EQ(rows.back().runtimeId(), runtimeStableId);
    auto* replacement = createAction(QStringLiteral("Play_Keep"));
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->name, QStringLiteral("Play_Keep"));
    EXPECT_NE(replacement->guid, guid);
    const QUuid replacementGuid = replacement->guid;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_NO_FATAL_FAILURE(reopen());
    replacement = manager.findItemByGuid(replacementGuid);
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->name, QStringLiteral("Play_Keep"));
    EXPECT_EQ(manager.findItemByGuid(guid), nullptr);
}

TEST_F(ActionIdentityTest, UndoDeleteRestoresSameIdentity)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* action = createAction(QStringLiteral("Play_Restored"));
    ASSERT_NE(action, nullptr);
    const QUuid guid = action->guid;
    const harkenstudio::ObjectId runtimeStableId = action->runtimeId();
    const auto revision = action->nameBindingRevision;
    ASSERT_TRUE(manager.deleteItem(action));
    EXPECT_EQ(manager.findItemByGuid(guid), nullptr);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    action = manager.findItemByGuid(guid);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->guid, guid);
    EXPECT_EQ(action->runtimeId(), runtimeStableId);
    EXPECT_EQ(action->nameBindingRevision, revision);
    EXPECT_EQ(action->name, QStringLiteral("Play_Restored"));
    auto* suffixed = createAction(QStringLiteral("Play_Restored"));
    ASSERT_NE(suffixed, nullptr);
    EXPECT_EQ(suffixed->name, QStringLiteral("Play_Restored_01"));
}

TEST_F(ActionIdentityTest, ActionDuplicateStableIdsRefuse)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* first = createAction(QStringLiteral("Play_One"));
    ProjectItem* second = createAction(QStringLiteral("Play_Two"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const harkenstudio::ObjectId firstId = first->runtimeId();
    const harkenstudio::ObjectId secondId = second->runtimeId();
    ASSERT_NE(firstId, secondId);
    second->guid = VNX::authoringUuid(harkentest::StudioObjectId(firstId));
    QString error;
    EXPECT_FALSE(validateProjectObjectIdentities(*manager.currentProject(), &error));
    EXPECT_EQ(first->runtimeId(), firstId);
    EXPECT_EQ(second->runtimeId(), firstId);
    second->guid = VNX::authoringUuid(harkentest::StudioObjectId(secondId));
}

TEST_F(ActionIdentityTest, MissingPublishedActionIdRefuses)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* action = createAction(QStringLiteral("Play_Published"));
    ASSERT_NE(action, nullptr);
    const harkenstudio::ObjectId publishedId = action->runtimeId();
    ASSERT_NE(publishedId, harkenstudio::ObjectId{});
    action->guid = VNX::authoringUuid(harkentest::StudioObjectId(0));
    QString error;
    EXPECT_FALSE(validateProjectObjectIdentities(*manager.currentProject(), &error));
    EXPECT_EQ(action->runtimeId(), harkenstudio::ObjectId{});
    action->guid = VNX::authoringUuid(harkentest::StudioObjectId(publishedId));
}

TEST_F(ActionIdentityTest, NestedObjectIndexSurvivesEditsAndRejectsAmbiguousOwners)
{
    auto& manager = ProjectManager::instance();
    auto* sound = manager.createItem(nullptr, QStringLiteral("Tone"), ProjectItem::Type::Sound);
    auto* track = manager.createItem(nullptr, QStringLiteral("Track"), ProjectItem::Type::Track);
    auto* piece = manager.createItem(nullptr, QStringLiteral("Piece"), ProjectItem::Type::Piece);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(piece, nullptr);
    EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.parameters = {0.0f};
    auto* mix = sound->comp<VMixNode>();
    ASSERT_TRUE(mix->effects.replace({effect}));
    auto binding = BindingSemantics::defaultBinding(BindingSemantics::volumeAddress(), QUuid::createUuid());
    binding.guid = QUuid::createUuid();
    ASSERT_TRUE(mix->bindings.upsert(binding));
    Clip clip{QUuid::createUuid(), sound->guid, 0, 1000000000};
    ASSERT_TRUE(track->comp<VTrack>()->clips.mintOccurrenceOrdinal(&clip));
    track->comp<VTrack>()->clips.items.push_back(clip);
    Marker marker{QUuid::createUuid(), MarkerKind::Custom, QStringLiteral("Accent"), 0};
    piece->comp<VPiece>()->markers.items.push_back(marker);
    const auto index = ProjectObjectIndex::fromProject(manager.currentProject());
    for (const auto id : {sound->runtimeId(), effect.runtimeId(), binding.runtimeId(),
                           clip.runtimeId(), marker.runtimeId()})
        ASSERT_NE(index.find(id), nullptr);
    EXPECT_EQ(index.find(effect.runtimeId())->kind, ProjectObjectLocation::Kind::Effect);
    EXPECT_EQ(index.find(binding.runtimeId())->kind, ProjectObjectLocation::Kind::Binding);
    EXPECT_EQ(index.find(clip.runtimeId())->kind, ProjectObjectLocation::Kind::Clip);
    EXPECT_EQ(index.find(marker.runtimeId())->kind, ProjectObjectLocation::Kind::Marker);
    EXPECT_EQ(index.find(effect.runtimeId())->ownerGuid, sound->guid);
    EXPECT_EQ(index.find(clip.runtimeId())->ownerGuid, track->guid);
    EXPECT_EQ(index.find(marker.runtimeId())->ownerGuid, piece->guid);
    ASSERT_TRUE(manager.renameItem(sound, QStringLiteral("Renamed")));
    mix->volume = -12.0f;
    const auto renamed = ProjectObjectIndex::fromProject(manager.currentProject());
    EXPECT_EQ(renamed.find(effect.runtimeId())->name, QStringLiteral("Renamed / gain"));
    EXPECT_EQ(index.find(effect.runtimeId())->name, QStringLiteral("Tone / gain"));
    const auto excluded = ProjectObjectIndex::fromProject(manager.currentProject(), sound);
    EXPECT_EQ(excluded.find(sound->runtimeId()), nullptr);
    EXPECT_EQ(excluded.find(effect.runtimeId()), nullptr);
    EXPECT_EQ(excluded.find(binding.runtimeId()), nullptr);
    ASSERT_NE(excluded.find(clip.runtimeId()), nullptr);
    auto* duplicate = manager.createItem(nullptr, QStringLiteral("Duplicate"), ProjectItem::Type::Sound);
    ASSERT_NE(duplicate, nullptr);
    const QUuid original = duplicate->guid;
    duplicate->guid = sound->guid;
    const auto ambiguous = ProjectObjectIndex::fromProject(manager.currentProject());
    EXPECT_EQ(ambiguous.find(sound->runtimeId()), nullptr);
    EXPECT_EQ(ambiguous.find(effect.runtimeId()), nullptr);
    EXPECT_EQ(ambiguous.find(binding.runtimeId()), nullptr);
    QString error;
    EXPECT_FALSE(validateProjectObjectIdentities(*manager.currentProject(), &error));
    EXPECT_EQ(duplicate->guid, sound->guid);
    duplicate->guid = original;
}

TEST_F(ActionIdentityTest, DuplicateMintsNestedUuidsAndSaveReloadKeepsThem)
{
    auto& manager = ProjectManager::instance();
    auto* sound = manager.createItem(nullptr, QStringLiteral("Tone"), ProjectItem::Type::Sound);
    auto* variable = manager.createItem(nullptr, QStringLiteral("Intensity"), ProjectItem::Type::Variable);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(variable, nullptr);
    EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.parameters = {-3.0f};
    auto binding = BindingSemantics::defaultBinding(BindingSemantics::volumeAddress(), variable->guid);
    binding.guid = QUuid::createUuid();
    ASSERT_TRUE(sound->comp<VMixNode>()->effects.replace({effect}));
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(binding));
    auto* copy = manager.duplicateItem(sound);
    ASSERT_NE(copy, nullptr);
    auto* copiedMix = copy->comp<VMixNode>();
    ASSERT_EQ(copiedMix->effects.size(), 1u);
    ASSERT_EQ(copiedMix->bindings.size(), 1u);
    const auto copiedEffect = copiedMix->effects.entries().front().runtimeId();
    const auto copiedBinding = copiedMix->bindings.entries().front().runtimeId();
    const auto copiedGuid = copy->guid;
    EXPECT_NE(copy->guid, sound->guid);
    EXPECT_NE(copiedEffect, effect.runtimeId());
    EXPECT_NE(copiedBinding, binding.runtimeId());
    EXPECT_EQ(copiedMix->bindings.entries().front().variable, variable->guid);
    ASSERT_TRUE(manager.saveProject());
    ASSERT_NO_FATAL_FAILURE(reopen());
    copy = manager.findItemByGuid(copiedGuid);
    ASSERT_NE(copy, nullptr);
    copiedMix = copy->comp<VMixNode>();
    ASSERT_NE(copiedMix, nullptr);
    EXPECT_EQ(copiedMix->effects.entries().front().runtimeId(), copiedEffect);
    EXPECT_EQ(copiedMix->bindings.entries().front().runtimeId(), copiedBinding);
}
