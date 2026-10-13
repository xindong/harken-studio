#include "Launch/StudioLauncher.h"

#include <gtest/gtest.h>

TEST(StudioLauncherTest, BuildsProjectCreationArguments)
{
    VNH::StudioCreateSpec spec;
    spec.launch.serverName = QStringLiteral("session");
    spec.launch.sessionToken = QStringLiteral("token");
    spec.launch.expectedBuildId = QStringLiteral("run1.aaaa1111");
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
                  QStringLiteral("--hub-session"),
                  QStringLiteral("session"),
                  QStringLiteral("--hub-token"),
                  QStringLiteral("token"),
                  QStringLiteral("--expected-build-id"),
                  QStringLiteral("run1.aaaa1111"),
                  QStringLiteral("--launch-intent"),
                  QStringLiteral("DirectOpenOnly"),
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

TEST(StudioLauncherTest, CarriesHubUpgradeApproval)
{
    VNH::StudioLaunchSpec spec;
    spec.projectPath = QStringLiteral("D:/Projects/Test.hsp");
    spec.serverName = QStringLiteral("session");
    spec.sessionToken = QStringLiteral("token");
    spec.expectedBuildId = QStringLiteral("run2.bbbb2222");
    spec.intent = VNH::LaunchIntent::UpgradeApprovedByHub;
    spec.projectOpenRequestId = QStringLiteral("request");
    spec.projectOpenApproval = QString(64, QLatin1Char('a'));

    const QStringList arguments = VNH::StudioLauncher::argumentsFor(spec);
    EXPECT_EQ(arguments.value(arguments.indexOf(
                  QStringLiteral("--launch-intent")) + 1),
              QStringLiteral("UpgradeApprovedByHub"));
    EXPECT_EQ(arguments.value(arguments.indexOf(
                  QStringLiteral("--project-open-request-id")) + 1),
              QStringLiteral("request"));
    EXPECT_EQ(arguments.value(arguments.indexOf(
                  QStringLiteral("--project-open-approval")) + 1),
              QString(64, QLatin1Char('a')));
}

TEST(StudioLauncherTest, OrdinaryOpenDiscardsAnyLeftoverApproval)
{
    VNH::StudioLaunchSpec spec;
    spec.intent = VNH::LaunchIntent::DirectOpenOnly;
    spec.projectOpenRequestId = QStringLiteral("previous-request");
    spec.projectOpenApproval = QString(64, QLatin1Char('a'));
    const auto arguments = VNH::StudioLauncher::argumentsFor(spec);
    EXPECT_FALSE(arguments.contains(QStringLiteral("--project-open-request-id")));
    EXPECT_FALSE(arguments.contains(QStringLiteral("--project-open-approval")));
}

TEST(StudioLauncherTest, LegacyOpenUsesOnlyTheProjectArgumentWithoutNewProtocol)
{
    VNH::StudioLaunchSpec spec;
    spec.projectPath = QStringLiteral("D:/Old Projects/Legacy.hsp");
    spec.projectOpenApproval = QStringLiteral("must-not-be-sent");
    for (const int protocol : {0, 3}) {
        spec.capabilitiesVersion = protocol;
        EXPECT_EQ(VNH::StudioLauncher::argumentsFor(spec),
                  (QStringList{QStringLiteral("--project"), spec.projectPath}));
    }
    spec.capabilitiesVersion = 1;
    const auto arguments = VNH::StudioLauncher::argumentsFor(spec);
    EXPECT_TRUE(arguments.contains(QStringLiteral("--hub-session")));
    EXPECT_FALSE(arguments.contains(QStringLiteral("--project-open-approval")));
}
