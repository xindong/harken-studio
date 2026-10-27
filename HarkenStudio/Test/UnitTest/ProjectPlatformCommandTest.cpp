#include "Core/ActionManager.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/UndoCommands.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QEvent>
#include <QTemporaryDir>

namespace {
using namespace VNX;
namespace Platforms = ProjectPlatformSemantics;

class ProjectPlatformCommand : public testing::Test
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
        if (manager().isProjectOpen()) manager().closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ASSERT_TRUE(directory.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("PlatformCommands");
        config.location = directory.path();
        config.platforms = {{QUuid::createUuid(), QStringLiteral("Windows"), BasePlatform::Windows}};
        ASSERT_TRUE(manager().createProject(config));
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager(), nullptr);
        undo().clear();
    }

    void TearDown() override
    {
        undo().clear();
        ActionManager::instance().clearAllForTesting();
        if (manager().isProjectOpen()) manager().closeProject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    ProjectManager& manager() { return ProjectManager::instance(); }
    ProjectData& project() { return *manager().currentProject(); }
    UndoRedoManager& undo() { return UndoRedoManager::instance(); }
    QJsonObject authoredState()
    {
        ProjectSerializer serializer;
        EXPECT_TRUE(serializer.prepareReferenceWrite(project()));
        QJsonObject items;
        for (const auto* item : Platforms::referenceOwners(project()))
            items.insert(item->guid.toString(), serializer.projectItemToJson(*item));
        QJsonArray groups;
        for (const auto& group : project().packGroups) groups.append(group.toJson());
        QJsonArray targeting;
        for (const auto& row : manager().getActionPackTargeting()) targeting.append(row.toJson());
        QJsonArray deployments;
        for (const auto& row : project().deploymentProfiles)
            deployments.append(QJsonObject{{"id", row.profileId}, {"name", row.name},
                {"destination", row.destination}, {"platformIds", QJsonArray::fromStringList(row.platformIds)},
                {"languages", QJsonArray::fromStringList(row.languages)}});
        return {{"platforms", Platforms::platformsToJson(project().platforms)}, {"items", items},
            {"groups", groups}, {"targeting", targeting}, {"deployments", deployments},
            {"current", manager().getPackExportCurrentPlatform()},
            {"selected", QJsonArray::fromStringList(manager().getPackExportSelectedPlatforms())}};
    }
    void reopen()
    {
        const auto path = project().projectPath;
        ASSERT_TRUE(manager().saveProject());
        manager().closeProject();
        auto prepared = ProjectManager::prepareProject(path);
        ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
        ASSERT_TRUE(manager().commitPreparedProject(std::move(prepared)));
    }

    QTemporaryDir directory;
};

TEST_F(ProjectPlatformCommand, OldCommandCannotReplayIntoReopenedProjectWithSameIds)
{
    const auto lease = manager().currentProjectLease();
    const auto* command = ActionManager::instance().findAction("create_project_platform");
    ASSERT_NE(command, nullptr);
    const auto result = command->handler({{"name", "Android"}, {"base_platform", "Android"}});
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.canReplay());
    reopen();
    EXPECT_NE(manager().currentProject(), lease.get());
    const auto before = authoredState();
    EXPECT_FALSE(result.canReplay());
    result.undoFn();
    EXPECT_EQ(authoredState(), before);
    result.redoFn();
    EXPECT_EQ(authoredState(), before);
}

}
