#include "ItemTypeCatalog.h"
#include "ActionManager.h"
#include "Actions/ActionSelectors.h"
#include "AttenuationStore.h"
#include "Components/VSelection.h"
#include "Components/VAttenuation.h"
#include "Components/VMixNode.h"
#include "NameValidator.h"
#include "ProjectIndex.h"
#include "ProjectSerializer.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Shared/ProjectItemMenuController.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/RefDomainAdapter.h"
#include "Widgets/Selector/SelectorWindow.h"

#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <memory>

using namespace VNX;

namespace {

using TestSupport::ensureApp;

class NewObjectNamingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());

        ProjectConfig config;
        config.name = QStringLiteral("NewObjectNamingProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        auto& pm = ProjectManager::instance();
        layer_ = pm.createItem(nullptr, QStringLiteral("Naming_Layer"),
                               ProjectItem::Type::Layer);
        ASSERT_NE(layer_, nullptr);

        errorConnection_ = QObject::connect(
            &pm, &ProjectManager::errorOccurred,
            [this](const QString& error) { errors_ << error; });

        ProjectItemMenuController::Callbacks cb;
        cb.getSelected = [this]() { return layer_; };
        controller_ = std::make_unique<ProjectItemMenuController>(std::move(cb));
        UndoRedoManager::instance().clear();
        QObject::connect(&promptGuard_, &QTimer::timeout, [] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                ADD_FAILURE() << "Object creation opened a modal dialog";
                dialog->reject();
            }
        });
        promptGuard_.start(0);
    }

    void TearDown() override
    {
        promptGuard_.stop();
        QObject::disconnect(errorConnection_);
        controller_.reset();
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QTimer promptGuard_;
    QTemporaryDir tempDir_;
    std::unique_ptr<ProjectItemMenuController> controller_;
    ProjectItem* layer_ = nullptr;
    QStringList errors_;
    QMetaObject::Connection errorConnection_;
};

TEST_F(NewObjectNamingTest, RepeatedCreatesUseFreeDefaultNamesWithoutPrompt)
{
    QStringList created;
    for (int i = 0; i < 3; ++i) {
        ProjectItem* item = controller_->createByType(ProjectItem::Type::Sound);
        ASSERT_NE(item, nullptr) << "creation " << i << " failed";
        created << item->name;
    }

    const QStringList expected{QStringLiteral("New_Sound"),
                               QStringLiteral("New_Sound_01"),
                               QStringLiteral("New_Sound_02")};
    EXPECT_EQ(created, expected);
    EXPECT_TRUE(errors_.isEmpty()) << errors_.join(QStringLiteral(" | ")).toStdString();
}

TEST_F(NewObjectNamingTest, DefaultNameAvoidsOtherTypesInTheSameNamespace)
{
    ProjectItem* taken = ProjectManager::instance().createItem(
        layer_, QStringLiteral("New_Sound"), ProjectItem::Type::Container);
    ASSERT_NE(taken, nullptr);

    ProjectItem* item = controller_->createByType(ProjectItem::Type::Sound);

    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->name, QStringLiteral("New_Sound_01"));
    EXPECT_TRUE(errors_.isEmpty()) << errors_.join(QStringLiteral(" | ")).toStdString();
}

TEST_F(NewObjectNamingTest, TrailingSeparatorProducesASingleSeparatorSuffix)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* taken = pm.createItem(nullptr, QStringLiteral("Play_"),
                                       ProjectItem::Type::Action);
    ASSERT_NE(taken, nullptr);

    EXPECT_EQ(pm.suggestUniqueName(ProjectItem::Type::Action, QStringLiteral("Play_")),
              QStringLiteral("Play_01"));
}

TEST_F(NewObjectNamingTest, CommandResultsContainTheActualCreatedAndRenamedNames)
{
    auto& pm = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    registerEditorActions(actions, &pm, nullptr, nullptr, nullptr);
    registerProjectActions(actions, &pm, nullptr, {});
    for (const QString& command : {QStringLiteral("create_action"), QStringLiteral("create_item")}) {
        const QJsonObject args{{"name", "Command_Name"}, {"type", "Sound"}};
        const auto first = actions.execute(command, args);
        const auto second = actions.execute(command, args);
        ASSERT_TRUE(first.success) << first.errorMessage.toStdString();
        ASSERT_TRUE(second.success) << second.errorMessage.toStdString();
        EXPECT_EQ(first.data.value("name").toString(), QStringLiteral("Command_Name"));
        EXPECT_EQ(second.data.value("name").toString(), QStringLiteral("Command_Name_01"));
        auto* item = pm.findItemByObjectID(second.data.value("authoring_handle").toInteger());
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->name, second.data.value("name").toString());
        const auto renamed = actions.execute(QStringLiteral("rename_item"),
            {{"object_id", second.data.value("authoring_handle")}, {"new_name", "Command_Name"}});
        ASSERT_TRUE(renamed.success) << renamed.errorMessage.toStdString();
        EXPECT_EQ(renamed.data.value("new_name").toString(), item->name);
        EXPECT_EQ(item->name, command == QStringLiteral("create_action")
            ? QStringLiteral("Command_Name_01") : QStringLiteral("Command_Name"));
    }
}

TEST_F(NewObjectNamingTest, AllCreationSpecsUseDefaultsWithoutPrompt)
{
    auto& pm = ProjectManager::instance();
    for (const auto& spec : ProjectItemMenuController::specs()) {
        if (spec.comingSoon) continue;
        ProjectItemMenuController::Callbacks cb;
        cb.creationParent = [&](ProjectItem::Type type) {
            return type == ProjectItem::Type::Bus ? pm.getMasterBusItem() : nullptr;
        };
        cb.currentBucketName = [] { return QStringLiteral("Actions"); };
        ProjectItemMenuController controller(std::move(cb));
        auto* first = controller.createByType(spec.type);
        auto* second = controller.createByType(spec.type);
        ASSERT_NE(first, nullptr) << spec.defaultNameKey;
        ASSERT_NE(second, nullptr) << spec.defaultNameKey;
        EXPECT_EQ(first->name, QString::fromLatin1(spec.defaultNameKey));
        EXPECT_NE(first->name, second->name);
        EXPECT_TRUE(NameValidator::validate(second->name));
    }
    EXPECT_TRUE(errors_.isEmpty()) << errors_.join(QStringLiteral(" | ")).toStdString();
}

TEST_F(NewObjectNamingTest, ConflictMessageNamesTheObjectInTheWay)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* taken = pm.createItem(nullptr, QStringLiteral("Busy_Name"),
                                       ProjectItem::Type::Variable);
    ASSERT_NE(taken, nullptr);

    const ProjectItem* conflict =
        pm.findNameConflict(QStringLiteral("Busy_Name"), ProjectItem::Type::Variable);
    ASSERT_EQ(conflict, taken);

    const QString message = ProjectManager::nameConflictMessage(
        QStringLiteral("Busy_Name"), ProjectItem::Type::Variable, *conflict);

    EXPECT_TRUE(message.contains(QStringLiteral("Busy_Name")));
    EXPECT_TRUE(message.contains(ItemTypeCatalog::displayName(ProjectItem::Type::Variable)));
    EXPECT_TRUE(message.contains(taken->objectPath));
    EXPECT_TRUE(message.contains(
        ItemTypeCatalog::nameNamespaceDisplayName(ItemTypeCatalog::NameNamespace::Variable)));
    EXPECT_FALSE(message.contains(QStringLiteral("namespace"), Qt::CaseInsensitive));
}

TEST_F(NewObjectNamingTest, ExplicitRenameAllowsSiblingDuplicatesAndUndoRestoresTheOriginalName)
{
    auto& pm = ProjectManager::instance();
    ProjectItem* first = pm.createItem(layer_, QStringLiteral("Rename_Target"),
                                       ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ProjectItem* second = pm.createItem(layer_, QStringLiteral("Rename_Other"),
                                        ProjectItem::Type::Sound);
    ASSERT_NE(second, nullptr);
    errors_.clear();

    const QUuid guid = second->guid;
    ASSERT_TRUE(pm.renameItem(second, QStringLiteral("Rename_Target")));
    EXPECT_TRUE(errors_.isEmpty());
    EXPECT_EQ(second->name, QStringLiteral("Rename_Target"));
    EXPECT_EQ(second->guid, guid);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(second->name, QStringLiteral("Rename_Other"));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(second->name, QStringLiteral("Rename_Target"));
    const int count = UndoRedoManager::instance().undoStack()->count();
    ASSERT_TRUE(pm.renameItem(second, QStringLiteral("Rename_Target")));
    EXPECT_EQ(second->name, QStringLiteral("Rename_Target"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), count);
}

TEST_F(NewObjectNamingTest, CopiesPreserveDuplicateDescendantNamesAndRemapReferences)
{
    auto& pm = ProjectManager::instance();
    auto* root = pm.createItem(nullptr, QStringLiteral("Copy"), ProjectItem::Type::Container);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(pm.createItem(root, QStringLiteral("Copy_01"), ProjectItem::Type::Sound), nullptr);
    ASSERT_NE(pm.createItem(root, QStringLiteral("Copy_02"), ProjectItem::Type::Sound), nullptr);
    ASSERT_TRUE(pm.renameItem(root->children[1].get(), QStringLiteral("Copy_01")));
    for (int operation = 0; operation < 4; ++operation) {
        ASSERT_EQ(pm.setClipboard({root}, ProjectManager::ClipboardOp::Copy), 1);
        ProjectItem* clone = nullptr;
        if (operation == 0) clone = pm.duplicateItem(root);
        if (operation == 1 || operation == 2) {
            const auto pasted = operation == 1
                ? pm.pasteClipboardAtBucketRoot(QStringLiteral("Layers")) : pm.pasteClipboard(layer_);
            ASSERT_EQ(pasted.size(), 1u);
            clone = pasted.front();
        }
        if (operation == 3) clone = pm.duplicateItem(layer_->children.back().get());
        ASSERT_NE(clone, nullptr);
        ASSERT_EQ(clone->children.size(), 2u);
        EXPECT_EQ(clone->name, operation == 2 ? QStringLiteral("Copy")
            : operation == 1 ? QStringLiteral("Copy_02") : QStringLiteral("Copy_01"));
        for (size_t i = 0; i < clone->children.size(); ++i) {
            const auto& child = clone->children[i];
            EXPECT_EQ(child->name, QStringLiteral("Copy_01"));
            EXPECT_NE(child->guid, root->children[i]->guid);
            EXPECT_EQ(child->parent, clone);
            EXPECT_EQ(child->parentRef.guid, clone->guid);
            EXPECT_EQ(clone->comp<VSelection>()->selections.shared.occurrenceRows()[i].childGuid, child->guid);
        }
        const QUuid guid = clone->guid;
        const QString name = clone->name;
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        EXPECT_EQ(pm.findItemByGuid(guid), nullptr);
        ASSERT_TRUE(UndoRedoManager::instance().redo());
        ASSERT_EQ(pm.findItemByGuid(guid), clone);
        EXPECT_EQ(clone->name, name);
    }
}

TEST_F(NewObjectNamingTest, MaximumLengthNamesRemainValidAcrossCreateCopyAndRename)
{
    auto& pm = ProjectManager::instance();
    const QString name(NameValidator::MAX_NAME_LENGTH, QLatin1Char('A'));
    auto* first = pm.createItem(layer_, name, ProjectItem::Type::Sound);
    auto* second = pm.createItem(layer_, name, ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* copy = pm.duplicateItem(first);
    ASSERT_NE(copy, nullptr);
    ASSERT_TRUE(pm.renameItem(layer_, name));
    QSet<QString> names;
    for (auto* item : {first, second, copy}) {
        EXPECT_TRUE(NameValidator::validate(item->name));
        EXPECT_EQ(item->name.size(), NameValidator::MAX_NAME_LENGTH);
        EXPECT_FALSE(names.contains(item->name));
        names.insert(item->name);
    }
    EXPECT_EQ(layer_->name, first->name);
}

TEST_F(NewObjectNamingTest, NewParentCreationAvoidsExistingContainerNames)
{
    auto& pm = ProjectManager::instance();
    auto* first = controller_->createByType(ProjectItem::Type::Sound);
    auto* second = controller_->createByType(ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* firstParent = pm.createParentContainer({first}, ProjectItem::Type::Container,
                                                QStringLiteral("New_Container"));
    auto* secondParent = pm.createParentContainer({second}, ProjectItem::Type::Container,
                                                 QStringLiteral("New_Container"));
    ASSERT_NE(firstParent, nullptr);
    ASSERT_NE(secondParent, nullptr);
    EXPECT_EQ(secondParent->name, QStringLiteral("New_Container_01"));
    EXPECT_EQ(first->parent, firstParent);
    EXPECT_EQ(second->parent, secondParent);
}

TEST_F(NewObjectNamingTest, NumericSuffixesDoNotStopAt9999OrOverflow)
{
    auto& pm = ProjectManager::instance();
    for (const QString& name : {QStringLiteral("Sound_9999"),
                               QStringLiteral("Sound_2147483647"),
                               QStringLiteral("Sound_18446744073709551615"),
                               QStringLiteral("Sound_184467440737095516150")}) {
        auto* first = pm.createItem(layer_, name, ProjectItem::Type::Sound);
        auto* second = pm.createItem(layer_, name, ProjectItem::Type::Sound);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_NE(first->name, second->name);
        EXPECT_TRUE(pm.isNameUnique(second->name, second->type, second));
        EXPECT_TRUE(NameValidator::validate(second->name));
        if (name == QStringLiteral("Sound_9999"))
            EXPECT_EQ(second->name, QStringLiteral("Sound_10000"));
    }
}

TEST_F(NewObjectNamingTest, AttenuationsIgnoreCaseWhileAudioNodesKeepIt)
{
    auto& pm = ProjectManager::instance();
    for (auto type : {ProjectItem::Type::Sound, ProjectItem::Type::Attenuation}) {
        auto* upper = pm.createItem(nullptr, QStringLiteral("Case"), type);
        auto* lower = pm.createItem(nullptr, QStringLiteral("case"), type);
        ASSERT_NE(upper, nullptr);
        ASSERT_NE(lower, nullptr);
        EXPECT_EQ(lower->name, type == ProjectItem::Type::Sound
            ? QStringLiteral("case") : QStringLiteral("case_01"));
        ASSERT_TRUE(pm.renameItem(lower, QStringLiteral("CASE")));
        EXPECT_EQ(lower->name, QStringLiteral("CASE"));
    }
}

TEST_F(NewObjectNamingTest, UndoRestoresRecordedNameEvenWhenAnotherSiblingNowUsesIt)
{
    auto& pm = ProjectManager::instance();
    auto* sound = pm.createItem(layer_, QStringLiteral("Before"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(pm.renameItem(sound, QStringLiteral("After")));
    ASSERT_NE(pm.createAudioItem(layer_, QStringLiteral("Before"), {}, 0), nullptr);
    const int index = UndoRedoManager::instance().undoStack()->index();
    EXPECT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(sound->name, QStringLiteral("Before"));
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->index(), index - 1);
    EXPECT_TRUE(errors_.isEmpty());
}

TEST_F(NewObjectNamingTest, CreationUsesLogicalSiblingsAndMovingPreservesNames)
{
    auto& pm = ProjectManager::instance();
    auto* other = pm.createItem(nullptr, QStringLiteral("Other"), ProjectItem::Type::Layer);
    auto* first = pm.createItem(layer_, QStringLiteral("Shared"), ProjectItem::Type::Sound);
    auto* second = pm.createItem(other, QStringLiteral("Shared"), ProjectItem::Type::Sound);
    auto* third = pm.createItem(other, QStringLiteral("Shared"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(second->name, first->name);
    EXPECT_EQ(third->name, QStringLiteral("Shared_01"));
    ASSERT_TRUE(pm.moveItem(second, layer_));
    EXPECT_EQ(second->name, first->name);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(second->parent, other);
    EXPECT_EQ(second->name, first->name);
}

TEST_F(NewObjectNamingTest, SdkNamesAreStrictAndBucketUniqueAcrossFoldersAndOperations)
{
    auto& pm = ProjectManager::instance();
    for (auto type : {ProjectItem::Type::Action, ProjectItem::Type::Variable}) {
        const QString bucket = ItemTypeCatalog::folderNameOf(ItemTypeCatalog::bucketOf(type));
        ASSERT_NE(pm.createFolder(nullptr, QStringLiteral("Group"), bucket), nullptr);
        auto* first = pm.createItem(nullptr, QStringLiteral("Sdk_Name"), type);
        auto* second = pm.createItem(nullptr, QStringLiteral("Sdk_Name"), type, QStringLiteral("Group"));
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(pm.currentProject()->findItemBySelector(bucket.toLower() + QStringLiteral("/group/Sdk_Name_01")), second);
        EXPECT_EQ(pm.currentProject()->findItemBySelector(bucket + QStringLiteral("/Group/sdk_name_01")), nullptr);
        EXPECT_EQ(second->name, QStringLiteral("Sdk_Name_01"));
        ASSERT_TRUE(pm.renameItem(second, first->name));
        EXPECT_EQ(second->name, QStringLiteral("Sdk_Name_01"));
        auto* copy = pm.duplicateItem(first);
        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->name, QStringLiteral("Sdk_Name_02"));
        for (const auto& invalid : {QStringLiteral("Has Space"), QStringLiteral("Has-Dash"),
                                   QStringLiteral("Has.Dot"), QStringLiteral("1First"),
                                   QStringLiteral("_First"), QStringLiteral("\u4e2d\u6587")}) {
            EXPECT_FALSE(NameValidator::validateItemName(invalid, type));
            EXPECT_EQ(pm.createItem(nullptr, invalid, type), nullptr);
            EXPECT_FALSE(pm.renameItem(first, invalid));
        }
        EXPECT_EQ(first->name, QStringLiteral("Sdk_Name"));
        second->name = first->name;
        ProjectSerializer serializer;
        EXPECT_FALSE(serializer.prepareReferenceWrite(*pm.currentProject()));
        second->name = QStringLiteral("Sdk_Name_01");
    }
}

TEST_F(NewObjectNamingTest, UnicodeNamesNormalizeAndInvalidDisplayNamesAreRejected)
{
    auto& pm = ProjectManager::instance();
    auto* first = pm.createItem(layer_, QStringLiteral("Cafe\u0301"), ProjectItem::Type::Sound);
    auto* second = pm.createItem(layer_, QStringLiteral("Caf\u00e9"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->name, QStringLiteral("Caf\u00e9"));
    EXPECT_EQ(second->name, QStringLiteral("Caf\u00e9_01"));
    ASSERT_TRUE(pm.renameItem(second, QStringLiteral("Cafe\u0301")));
    EXPECT_EQ(first->name, second->name);
    const QString decomposed = QStringLiteral("e\u0301").repeated(128);
    auto* normalized = pm.createItem(layer_, decomposed, ProjectItem::Type::Sound);
    ASSERT_NE(normalized, nullptr);
    EXPECT_EQ(normalized->name, QString(128, QChar(0x00e9)));
    for (const auto& name : {QStringLiteral(" "), QStringLiteral("a/b"),
                            QStringLiteral("a\\b"), QStringLiteral("a\nb"),
                            QStringLiteral("a\u2028b")})
        EXPECT_EQ(pm.createItem(layer_, name, ProjectItem::Type::Sound), nullptr);
    const QString longName = QString::fromUcs4(U"\U0001f3b5").repeated(64);
    const QString file = NameValidator::objectFileName(longName, first->guid);
    EXPECT_TRUE(file.isValidUtf16());
    EXPECT_LE(file.toUtf8().size(), 255);
    EXPECT_TRUE(file.endsWith(first->guid.toString(QUuid::WithoutBraces) + QStringLiteral(".vp")));
}

TEST_F(NewObjectNamingTest, DuplicateUnicodeAndSanitizedNamesPersistWithIndependentGuids)
{
    auto& pm = ProjectManager::instance();
    QHash<QUuid, QString> expected;
    for (const auto& name : {QStringLiteral("\u811a\u6b65"), QStringLiteral("A:B"),
                            QStringLiteral("A?B"), QStringLiteral("Case"),
                            QStringLiteral("case"), QStringLiteral("CON"),
                            QString(128, QChar(0x97f3))}) {
        auto* item = pm.createItem(layer_, name, ProjectItem::Type::Sound);
        ASSERT_NE(item, nullptr);
        expected.insert(item->guid, name);
    }
    auto* duplicate = pm.createItem(layer_, QStringLiteral("Temporary"), ProjectItem::Type::Sound);
    ASSERT_NE(duplicate, nullptr);
    ASSERT_TRUE(pm.renameItem(duplicate, QStringLiteral("\u811a\u6b65")));
    expected.insert(duplicate->guid, duplicate->name);
    const QUuid duplicateGuid = duplicate->guid;
    const QString projectPath = pm.currentProject()->projectPath;
    const QDir root(QFileInfo(projectPath).absolutePath());
    ASSERT_TRUE(pm.saveProject()) << errors_.join(QLatin1Char('\n')).toStdString();
    QSet<QString> files;
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const QString fileName = NameValidator::objectFileName(it.value(), it.key());
        EXPECT_FALSE(files.contains(fileName.toCaseFolded()));
        files.insert(fileName.toCaseFolded());
        QFile file(root.filePath(QStringLiteral("Layers/") + fileName));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const auto content = QJsonDocument::fromJson(file.readAll()).object().value("content").toObject();
        EXPECT_EQ(content.value("name").toString(), it.value());
        EXPECT_EQ(QUuid::fromString(content.value("guid").toString()), it.key());
    }
    ProjectIndex index;
    ASSERT_EQ(index.scanProjectRoot(root.path()), ProjectIndex::ScanResult::Ok);
    EXPECT_EQ(index.findByName(ProjectItem::Type::Sound, QStringLiteral("\u811a\u6b65")), nullptr);
    EXPECT_EQ(index.findAllByName(ProjectItem::Type::Sound, QStringLiteral("\u811a\u6b65")).size(), 2u);
    ASSERT_NE(index.findByName(ProjectItem::Type::Sound, QStringLiteral("Case")), nullptr);
    EXPECT_EQ(index.findAllByName(ProjectItem::Type::Sound, QStringLiteral("CASE")).size(), 0u);
    ASSERT_TRUE(index.saveToCache(root.path()));
    ProjectIndex cached;
    ASSERT_TRUE(cached.loadFromCache(root.path()));
    EXPECT_EQ(cached.findAllByName(ProjectItem::Type::Sound, QStringLiteral("\u811a\u6b65")).size(), 2u);
    ASSERT_TRUE(pm.closeProject());
    ASSERT_TRUE(pm.loadProject(projectPath));
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        auto* item = pm.findItemByGuid(it.key());
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->name, it.value());
    }
    duplicate = pm.findItemByGuid(duplicateGuid);
    const QString oldFile = root.filePath(QStringLiteral("Layers/")
        + NameValidator::objectFileName(duplicate->name, duplicateGuid));
    ASSERT_TRUE(pm.renameItem(duplicate, QStringLiteral("\u65b0\u540d\u79f0")));
    ASSERT_TRUE(pm.saveProject()) << errors_.join(QLatin1Char('\n')).toStdString();
    EXPECT_FALSE(QFileInfo::exists(oldFile));
    EXPECT_TRUE(QFileInfo::exists(root.filePath(QStringLiteral("Layers/")
        + NameValidator::objectFileName(duplicate->name, duplicateGuid))));
}

TEST_F(NewObjectNamingTest, AmbiguousNamesAndPathsRequireGuidWithoutLosingUniqueDescendants)
{
    auto& pm = ProjectManager::instance();
    auto* left = pm.createItem(layer_, QStringLiteral("Branch"), ProjectItem::Type::Container);
    auto* right = pm.createItem(layer_, QStringLiteral("Other"), ProjectItem::Type::Container);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(pm.renameItem(right, left->name));
    auto* first = pm.createItem(left, QStringLiteral("Leaf"), ProjectItem::Type::Sound);
    auto* second = pm.createItem(right, QStringLiteral("OtherLeaf"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* project = pm.currentProject();
    EXPECT_EQ(project->findItemBySelector(project->itemPath(first)), first);
    ASSERT_TRUE(pm.renameItem(second, first->name));
    for (const auto& selector : {first->name, project->itemPath(first)}) {
        const auto resolved = resolveProjectSelector(project, selector);
        EXPECT_FALSE(resolved.ok);
        EXPECT_EQ(resolved.candidates.size(), 2);
    }
    EXPECT_EQ(project->findItemBySelector(first->guid.toString()), first);
    auto& actions = ActionManager::instance();
    registerProjectActions(actions, &pm, nullptr, {});
    const auto ambiguous = actions.execute(QStringLiteral("rename_item"),
        {{"path", project->itemPath(first)}, {"new_name", "Changed"}});
    EXPECT_FALSE(ambiguous.success);
    EXPECT_EQ(first->name, QStringLiteral("Leaf"));
    EXPECT_EQ(second->name, QStringLiteral("Leaf"));
    const auto renamed = actions.execute(QStringLiteral("rename_item"),
        {{"guid", second->guid.toString()}, {"new_name", "Changed"}});
    ASSERT_TRUE(renamed.success) << renamed.errorMessage.toStdString();
    EXPECT_EQ(second->name, QStringLiteral("Changed"));
    EXPECT_EQ(first->name, QStringLiteral("Leaf"));
}

TEST_F(NewObjectNamingTest, ReferencesSelectAndNavigateSamePathObjectsByIdentity)
{
    auto& pm = ProjectManager::instance();
    auto* first = pm.createItem(layer_, QStringLiteral("Shared"), ProjectItem::Type::Sound);
    auto* second = pm.createItem(layer_, QStringLiteral("Other"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(pm.renameItem(second, first->name));
    ASSERT_NE(first->objectID, second->objectID);
    ASSERT_EQ(pm.currentProject()->itemPath(first), pm.currentProject()->itemPath(second));
    const auto& adapter = refDomainAdapter(RefDomain::ProjectObject);
    QWidget host;
    ObjectRefNavigation navigation(&host);
    ObjectRefWidget reference(&host);
    reference.setFilterType(ProjectItem::Type::Sound);
    reference.setCurrentObjectID(second->objectID);
    auto* name = reference.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    uint32_t selectedId = 0;
    uint32_t locatedId = 0;
    uint32_t navigatedId = 0;
    QObject::connect(&reference, &ObjectRefWidget::objectChanged,
        &reference, [&](uint32_t id) { selectedId = id; });
    QObject::connect(&navigation, &ObjectRefNavigation::locateRequested,
        &reference, [&](RefDomain domain, uint32_t id) {
            EXPECT_EQ(domain, RefDomain::ProjectObject);
            locatedId = id;
        });
    QObject::connect(&navigation, &ObjectRefNavigation::navigateRequested,
        &reference, [&](RefDomain domain, uint32_t id) {
            EXPECT_EQ(domain, RefDomain::ProjectObject);
            navigatedId = id;
        });
    for (auto* item : {first, second}) {
        EXPECT_EQ(adapter.displayName(item->objectID), item->name);
        EXPECT_EQ(adapter.tooltip(item->objectID), pm.currentProject()->itemPath(item));
        reference.openSelector();
        SelectorWindow* selector = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* candidate = qobject_cast<SelectorWindow*>(widget);
            if (candidate && candidate->isVisible()) selector = candidate;
        }
        ASSERT_NE(selector, nullptr);
        auto* tree = selector->findChild<QTreeWidget*>();
        ASSERT_NE(tree, nullptr);
        ASSERT_NE(tree->currentItem(), nullptr);
        EXPECT_EQ(tree->currentItem()->data(0, kRoleObjectID).toUInt(), reference.currentObjectID());
        QTreeWidgetItem* target = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, kRoleObjectID).toUInt() == item->objectID) target = *it;
        ASSERT_NE(target, nullptr);
        EXPECT_EQ(target->text(0), item->name);
        EXPECT_EQ(target->toolTip(0), pm.currentProject()->itemPath(item));
        tree->setCurrentItem(target);
        selector->accept();
        EXPECT_EQ(selectedId, item->objectID);
        EXPECT_EQ(reference.currentObjectID(), item->objectID);
        EXPECT_EQ(name->text(), item->name);
        EXPECT_EQ(reference.toolTip(), pm.currentProject()->itemPath(item));
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&reference, &press);
        EXPECT_EQ(locatedId, item->objectID);
        QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(5, 5), QPointF(5, 5),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&reference, &doubleClick);
        QCoreApplication::sendPostedEvents(&reference, QEvent::MetaCall);
        EXPECT_EQ(navigatedId, item->objectID);
    }
}

TEST_F(NewObjectNamingTest, AttenuationRestoreKeepsDuplicateNamesAndMissingGuidNeverFallsBack)
{
    auto& pm = ProjectManager::instance();
    auto* first = pm.createItem(nullptr, QStringLiteral(" Distance "), ProjectItem::Type::Attenuation);
    auto* second = pm.createItem(nullptr, QStringLiteral("Other"), ProjectItem::Type::Attenuation);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(pm.renameItem(second, first->name));
    const auto saved = second->comp<VAttenuation>()->snapshot();
    EXPECT_EQ(AttenuationStore::find(pm.currentProject(), QUuid::createUuid(), first->name), nullptr);
    EXPECT_FALSE(AttenuationStore::erase(pm.currentProject(), QUuid::createUuid(), first->name));
    ASSERT_TRUE(AttenuationStore::erase(pm.currentProject(), saved.guid, saved.name));
    auto* restored = AttenuationStore::upsertJson(pm.currentProject(), saved.name, saved.toJson());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->hostItem()->name, first->name);
    EXPECT_EQ(restored->hostItem()->guid, saved.guid);
    EXPECT_EQ(AttenuationStore::find(pm.currentProject(), first->name), nullptr);
}

TEST_F(NewObjectNamingTest, GeneratedActionsFromUnicodeTargetsUseStrictUniqueNames)
{
    auto& pm = ProjectManager::instance();
    auto* sound = pm.createItem(layer_, QStringLiteral("\u811a\u6b65"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const auto first = pm.createActionsTargeting({sound}, VActionStep::Type::Play);
    const auto second = pm.createActionsTargeting({sound}, VActionStep::Type::Play);
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_TRUE(NameValidator::validateItemName(first.front()->name, ProjectItem::Type::Action));
    EXPECT_EQ(second.front()->name, first.front()->name + QStringLiteral("_01"));
}

TEST_F(NewObjectNamingTest, BusAndContainerCommandsKeepResolvedIdentityWithDuplicateNames)
{
    auto& pm = ProjectManager::instance();
    auto* first = pm.createItem(pm.getMasterBusItem(), QStringLiteral("Mix"), ProjectItem::Type::Bus);
    auto* second = pm.createItem(pm.getMasterBusItem(), QStringLiteral("Other"), ProjectItem::Type::Bus);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(pm.renameItem(second, first->name));
    auto& actions = ActionManager::instance();
    registerMixerActions(actions, nullptr, &pm, nullptr);
    const auto ambiguous = actions.execute(QStringLiteral("set_mixer_volume"),
        {{"bus", "Mix"}, {"volume_db", -4}});
    EXPECT_FALSE(ambiguous.success);
    EXPECT_FLOAT_EQ(first->comp<VNX::VMixNode>()->volume, 0.0f);
    EXPECT_FLOAT_EQ(second->comp<VNX::VMixNode>()->volume, 0.0f);
    const auto resolved = actions.execute(QStringLiteral("set_mixer_volume"),
        {{"bus", second->guid.toString()}, {"volume_db", -4}});
    EXPECT_TRUE(resolved.success) << resolved.errorMessage.toStdString();
    EXPECT_EQ(resolved.data.value(QStringLiteral("bus")).toString(), second->name);
    EXPECT_EQ(resolved.data.value(QStringLiteral("bus_guid")).toString(), second->guid.toString(QUuid::WithoutBraces));
    auto* container = pm.createItem(layer_, QStringLiteral("Group"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    auto* a = pm.createItem(container, QStringLiteral("Same"), ProjectItem::Type::Sound);
    auto* b = pm.createItem(container, QStringLiteral("Other"), ProjectItem::Type::Sound);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(pm.renameItem(b, a->name));
    registerContainerActions(actions, &pm);
    const auto collision = actions.execute(QStringLiteral("reorder_container_children"),
        {{"container_path", container->guid.toString()}, {"child_order", QJsonArray{a->name}}});
    EXPECT_FALSE(collision.success);
    EXPECT_EQ(container->children[0].get(), a);
    EXPECT_EQ(container->children[1].get(), b);
    const auto reordered = actions.execute(QStringLiteral("reorder_container_children"),
        {{"container_path", container->guid.toString()},
         {"child_order", QJsonArray{b->guid.toString(), a->guid.toString()}}});
    ASSERT_TRUE(reordered.success) << reordered.errorMessage.toStdString();
    EXPECT_EQ(container->children[0].get(), b);
    EXPECT_EQ(container->children[1].get(), a);
}

}
