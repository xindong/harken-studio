#include "Launch/StudioLauncher.h"

#include <gtest/gtest.h>

TEST(StudioLauncherTest, BuildsProjectCreationArguments)
{
    harken::hub::StudioCreateSpec spec;
    spec.projectName = QStringLiteral("My Project");
    spec.projectLocation = QStringLiteral("D:/Projects");
    spec.templateId = QStringLiteral("mmorpg");

    EXPECT_EQ(harken::hub::StudioLauncher::argumentsForCreate(spec),
              (QStringList{
                  QStringLiteral("--create-project"),
                  QStringLiteral("--project-name"),
                  QStringLiteral("My Project"),
                  QStringLiteral("--project-location"),
                  QStringLiteral("D:/Projects"),
                  QStringLiteral("--project-template-id"),
                  QStringLiteral("mmorpg"),
              }));
}

TEST(StudioLauncherTest, OmitsTemplateArgumentsForEmptyProject)
{
    harken::hub::StudioCreateSpec spec;
    spec.projectName = QStringLiteral("Empty");
    spec.projectLocation = QStringLiteral("D:/Projects");

    const QStringList arguments =
        harken::hub::StudioLauncher::argumentsForCreate(spec);
    EXPECT_FALSE(arguments.contains(QStringLiteral("--project-template-id")));
    EXPECT_FALSE(arguments.contains(QStringLiteral("--project-template-file")));
}

TEST(StudioLauncherTest, PassesTheProjectPathAsASingleArgument)
{
    harken::hub::StudioLaunchSpec spec;
    spec.projectPath = QStringLiteral("D:/Old Projects/Legacy.hsp");
    EXPECT_EQ(harken::hub::StudioLauncher::argumentsFor(spec),
              (QStringList{QStringLiteral("--project"), spec.projectPath}));
}
