#include "Launch/StudioLauncher.h"

#include <gtest/gtest.h>

TEST(StudioLauncherTest, BuildsProjectCreationArguments)
{
    VNH::StudioCreateSpec spec;
    spec.projectName = QStringLiteral("My Project");
    spec.projectLocation = QStringLiteral("D:/Projects");
    spec.templateId = QStringLiteral("mmorpg");
    spec.templatePath = QStringLiteral("D:/Studio/mmorpg.json");

    EXPECT_EQ(VNH::StudioLauncher::argumentsForCreate(spec),
              (QStringList{
                  QStringLiteral("--create-project"),
                  QStringLiteral("--project-name"),
                  QStringLiteral("My Project"),
                  QStringLiteral("--project-location"),
                  QStringLiteral("D:/Projects"),
                  QStringLiteral("--project-template-id"),
                  QStringLiteral("mmorpg"),
                  QStringLiteral("--project-template-file"),
                  QStringLiteral("D:/Studio/mmorpg.json"),
              }));
}

TEST(StudioLauncherTest, OmitsTemplateArgumentsForEmptyProject)
{
    VNH::StudioCreateSpec spec;
    spec.projectName = QStringLiteral("Empty");
    spec.projectLocation = QStringLiteral("D:/Projects");

    const QStringList arguments =
        VNH::StudioLauncher::argumentsForCreate(spec);
    EXPECT_FALSE(arguments.contains(QStringLiteral("--project-template-id")));
    EXPECT_FALSE(arguments.contains(QStringLiteral("--project-template-file")));
}

TEST(StudioLauncherTest, PassesTheProjectPathAsASingleArgument)
{
    VNH::StudioLaunchSpec spec;
    spec.projectPath = QStringLiteral("D:/Old Projects/Legacy.hsp");
    EXPECT_EQ(VNH::StudioLauncher::argumentsFor(spec),
              (QStringList{QStringLiteral("--project"), spec.projectPath}));
}
