#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace VNX {

class ProjectContractReadContext;

struct PreflightReport
{
    int currentSchema = 0;
    int targetSchema = 0;
    QStringList warnings;
    QStringList blockingErrors;

    QJsonObject toJson() const;
};

class ProjectPreflight
{
public:
    static QString schemaMismatchError(int projectSchema, int studioSchema);
    static PreflightReport run(const QString& projectPath, int targetSchema,
                                ProjectContractReadContext* context = nullptr);
};

}
