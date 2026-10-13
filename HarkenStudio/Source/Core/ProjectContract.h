#pragma once

#include "ProjectIssue.h"
#include "ProjectFormatVersion.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace VNX {

struct ProjectContractProbe
{
    bool ok = false;
    ProjectIssue issue = ProjectIssue::None;
    QString detail;
    int schemaVersion = 0;
    QString projectId;
    QStringList blockingErrors;
    QStringList warnings;
};

class ProjectContractRegistry
{
public:
    static ProjectContractRegistry& instance();

    ProjectContractProbe probe(const QString& projectFilePath) const;
    QStringList validateTree(const QString& projectDirectory,
                             const QJsonObject& projectRoot,
                             bool requireCurrentSchema,
                             const QHash<QString, QJsonObject>& overrides = {}) const;

private:
    ProjectContractRegistry() = default;
};

}
