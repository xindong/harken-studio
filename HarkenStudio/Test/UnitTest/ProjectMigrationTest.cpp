#include "Core/ProjectMigration.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <utility>

namespace {

void writeObject(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write(QJsonDocument(object).toJson()), 0);
}

TEST(ProjectMigration, WrongTargetVersionStopsBeforeFlush)
{
    QTemporaryDir source;
    QTemporaryDir staging;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(staging.isValid());
    const QString relative = QStringLiteral("Demo.hsp");
    writeObject(source.filePath(relative),
                QJsonObject{{QStringLiteral("schemaVersion"), 1}});

    VNX::ProjectMigrationRegistry registry;
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("project-1-to-2");
    step.fromSchema = 1;
    step.toSchema = 2;
    step.writeSet = {relative};
    step.transform = [relative](VNX::ProjectMigrationWorkspace& workspace,
                                QStringList*, QString* error) {
        QJsonObject root;
        if (!workspace.readObject(relative, &root, error))
            return false;
        root.insert(QStringLiteral("schemaVersion"), 99);
        return workspace.writeObject(relative, root, error);
    };
    ASSERT_TRUE(registry.registerStep(std::move(step)));

    const VNX::ProjectMigrationPlan plan = registry.plan(1, 2, source.path());
    VNX::ProjectMigrationWorkspace workspace(source.path(), staging.path(),
                                             plan.writeSet, relative);
    QString error;
    EXPECT_FALSE(registry.execute(
        plan, workspace, nullptr, &error, {},
        [&relative](VNX::ProjectMigrationWorkspace& current, int expected,
                    QString* validationError) {
            QJsonObject root;
            if (!current.readObject(relative, &root, validationError))
                return false;
            if (root.value(QStringLiteral("schemaVersion")).toInt() == expected)
                return true;
            if (validationError)
                *validationError = QStringLiteral("wrong target version");
            return false;
        }));
    EXPECT_FALSE(QFileInfo::exists(staging.filePath(relative)));
}

TEST(ProjectMigration, WriteOutsideDeclaredSetIsRejected)
{
    QTemporaryDir source;
    QTemporaryDir staging;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(staging.isValid());
    VNX::ProjectMigrationWorkspace workspace(
        source.path(), staging.path(), {QStringLiteral("Demo.hsp")});
    QString error;
    EXPECT_FALSE(workspace.writeObject(QStringLiteral("AudioSources/source.wav"),
                                       QJsonObject{}, &error));
}

TEST(ProjectMigration, MissingDependencyStopsThePlan)
{
    VNX::ProjectMigrationRegistry registry;
    VNX::ProjectMigrationStep step;
    step.id = QStringLiteral("root");
    step.fromSchema = 1;
    step.toSchema = 2;
    step.dependencies = {QStringLiteral("missing")};
    step.transform = [](VNX::ProjectMigrationWorkspace&, QStringList*, QString*) {
        return true;
    };
    ASSERT_TRUE(registry.registerStep(std::move(step)));

    const VNX::ProjectMigrationPlan plan = registry.plan(1, 2);
    EXPECT_FALSE(plan.ok);
}

TEST(ProjectMigration, CyclicDependenciesStopThePlan)
{
    VNX::ProjectMigrationRegistry registry;
    for (const QString& id : {QStringLiteral("first"), QStringLiteral("second")}) {
        VNX::ProjectMigrationStep step;
        step.id = id;
        step.fromSchema = 1;
        step.toSchema = 2;
        step.dependencies = {id == QStringLiteral("first")
                                 ? QStringLiteral("second")
                                 : QStringLiteral("first")};
        step.transform = [](VNX::ProjectMigrationWorkspace&, QStringList*, QString*) {
            return true;
        };
        ASSERT_TRUE(registry.registerStep(std::move(step)));
    }

    const VNX::ProjectMigrationPlan plan = registry.plan(1, 2);
    EXPECT_FALSE(plan.ok);
}

}
