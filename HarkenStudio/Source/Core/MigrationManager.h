
#pragma once

#include "ProjectFormatVersion.h"
#include "ProjectMigration.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility>

namespace VNX {

struct ProjectData;
struct ProjectItem;

class MigrationManager
{
public:
    MigrationManager();
    explicit MigrationManager(ProjectMigrationRegistry registry);

    static constexpr int CURRENT_PROJECT_SCHEMA_VERSION = kCurrentProjectSchemaVersion;

    static MigrationManager& instance();

    int detectVersion(const QJsonObject& root) const;

    bool registerMigration(ProjectMigrationStep step, QString* error = nullptr);

    ProjectMigrationRegistry& registry() { return registry_; }

    using TreeValidatorForTesting = std::function<bool(
        const QString& stagingDirectory, int expectedVersion, QString* error)>;
    void setTreeValidatorForTesting(TreeValidatorForTesting validator)
    {
        treeValidatorForTesting_ = std::move(validator);
    }

    bool validateSchema(const QJsonObject& root, ProjectData& project);

    ProjectMigrationPlan planProject(const QString& projectDirectory,
                                     const QString& projectFile,
                                     int sourceSchema,
                                     int targetSchema) const;
    bool migrateProject(const ProjectMigrationPlan& plan,
                        const QString& sourceDirectory,
                        const QString& stagingDirectory,
                        const QString& projectFile,
                        QStringList* warnings = nullptr,
                        QString* error = nullptr,
                        ProjectMigrationProgressCallback progress = {}) const;

    static void backfillItemGuidAndPath(ProjectItem* item, ProjectItem* parent);

private:
    ProjectMigrationRegistry registry_;
    TreeValidatorForTesting treeValidatorForTesting_;
};

}
