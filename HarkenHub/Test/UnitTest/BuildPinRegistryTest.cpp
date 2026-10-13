#include "Maintenance/BuildPinRegistry.h"

#include "Projects/ProjectStore.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <memory>

using VNH::BuildPinRegistry;

namespace {

class EnvGuard
{
public:
    EnvGuard(const char* name, const QString& value)
        : name_(name)
    {
        qputenv(name, value.toUtf8());
    }
    ~EnvGuard() { qunsetenv(name_); }

private:
    const char* name_;
};

class BuildPinRegistryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        machineGuard = std::make_unique<EnvGuard>(
            "HARKEN_HUB_MACHINE_ROOT",
            dir.filePath(QStringLiteral("machine")));
        pinUserGuard = std::make_unique<EnvGuard>(
            "HARKEN_HUB_PIN_USER", QStringLiteral("alice"));
    }

    QTemporaryDir dir;
    std::unique_ptr<EnvGuard> machineGuard;
    std::unique_ptr<EnvGuard> pinUserGuard;
};

}

TEST_F(BuildPinRegistryTest, PinRegistryUnionsAllUsers)
{
    QString error;
    ASSERT_TRUE(BuildPinRegistry::writeCurrentUserPins(
        {QStringLiteral("b1"), QStringLiteral("b2"),
         QStringLiteral("../escape"), QStringLiteral("b1")},
        &error))
        << error.toStdString();

    {
        const EnvGuard otherUser("HARKEN_HUB_PIN_USER",
                                 QStringLiteral("bob"));
        ASSERT_TRUE(BuildPinRegistry::writeCurrentUserPins(
            {QStringLiteral("b3")}, &error));
    }

    const auto pinned = BuildPinRegistry::machinePinnedBuildIds();
    EXPECT_EQ(pinned, (QSet<QString>{QStringLiteral("b1"),
                                     QStringLiteral("b2"),
                                     QStringLiteral("b3")}));
}

TEST_F(BuildPinRegistryTest, PinRegistryReflectsProjectAssociations)
{
    VNH::ProjectStore store;
    ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("projects.db"))));
    VNH::ProjectRecord project;
    project.canonicalPath = dir.filePath(QStringLiteral("p1.hsp"));
    project.displayName = QStringLiteral("p1");
    project.preferredStudioBuildId = QStringLiteral("preferred1");
    project.lastKnownGoodStudioBuildId = QStringLiteral("good1");
    ASSERT_TRUE(store.addProject(project));

    QString error;
    ASSERT_TRUE(BuildPinRegistry::refreshFromProjects(store, &error))
        << error.toStdString();

    const auto pinned = BuildPinRegistry::machinePinnedBuildIds();
    EXPECT_TRUE(pinned.contains(QStringLiteral("preferred1")));
    EXPECT_FALSE(pinned.contains(QStringLiteral("good1")));
}
