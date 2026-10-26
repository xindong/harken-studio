#include "Components/VSelection.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QHash>
#include <QStringList>
#include <QTemporaryDir>

using namespace VNX;

namespace {

class NewObjectNamingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TestSupport::ensureApp();
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
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        QObject::disconnect(errorConnection_);
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
    ProjectItem* layer_ = nullptr;
    QStringList errors_;
    QMetaObject::Connection errorConnection_;
};

TEST_F(NewObjectNamingTest, CopiesRemapDescendantIdentitiesAndReferences)
{
    auto& pm = ProjectManager::instance();
    auto* root = pm.createItem(nullptr, QStringLiteral("Copy"), ProjectItem::Type::Container);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(pm.createItem(root, QStringLiteral("Copy_01"), ProjectItem::Type::Sound), nullptr);
    ASSERT_NE(pm.createItem(root, QStringLiteral("Copy_02"), ProjectItem::Type::Sound), nullptr);
    ASSERT_TRUE(pm.renameItem(root->children[1].get(), QStringLiteral("Copy_01")));
    for (int operation = 0; operation < 3; ++operation) {
        ASSERT_EQ(pm.setClipboard({root}, ProjectManager::ClipboardOp::Copy), 1);
        ProjectItem* clone = nullptr;
        if (operation == 0) clone = pm.duplicateItem(root);
        if (operation == 1 || operation == 2) {
            const auto pasted = operation == 1
                ? pm.pasteClipboardAtBucketRoot(QStringLiteral("Layers")) : pm.pasteClipboard(layer_);
            ASSERT_EQ(pasted.size(), 1u);
            clone = pasted.front();
        }
        ASSERT_NE(clone, nullptr);
        ASSERT_EQ(clone->children.size(), 2u);
        for (size_t i = 0; i < clone->children.size(); ++i) {
            const auto& child = clone->children[i];
            EXPECT_NE(child->guid, root->children[i]->guid);
            EXPECT_EQ(child->parent, clone);
            EXPECT_EQ(child->parentRef.guid, clone->guid);
            EXPECT_EQ(clone->comp<VSelection>()->selections.shared.occurrenceRows()[i].childGuid, child->guid);
        }
        const QUuid guid = clone->guid;
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        EXPECT_EQ(pm.findItemByGuid(guid), nullptr);
        ASSERT_TRUE(UndoRedoManager::instance().redo());
        ASSERT_NE(pm.findItemByGuid(guid), nullptr);
    }
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
        expected.insert(item->guid, item->name);
    }
    auto* duplicate = pm.createItem(layer_, QStringLiteral("Temporary"), ProjectItem::Type::Sound);
    ASSERT_NE(duplicate, nullptr);
    ASSERT_TRUE(pm.renameItem(duplicate, QStringLiteral("\u811a\u6b65")));
    expected.insert(duplicate->guid, duplicate->name);
    const QString projectPath = pm.currentProject()->projectPath;
    ASSERT_TRUE(pm.saveProject()) << errors_.join(QLatin1Char('\n')).toStdString();
    ASSERT_TRUE(pm.closeProject());
    ASSERT_TRUE(pm.loadProject(projectPath));
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        auto* item = pm.findItemByGuid(it.key());
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->name, it.value());
    }
}

}
