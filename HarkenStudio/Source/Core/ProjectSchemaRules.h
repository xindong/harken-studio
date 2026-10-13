#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace VNX {

enum class ProjectSchemaReferenceKind
{
    Guid,
    Object,
    Parent
};

struct ProjectSchemaReferenceRule
{
    ProjectSchemaReferenceKind kind = ProjectSchemaReferenceKind::Object;
    QStringList path;
    QStringList targetTypes;
    bool allowNull = false;
    QString selectorField;
    QHash<QString, QStringList> targetTypesBySelector;
};

class ProjectSchemaRuleExecutor
{
public:
    virtual ~ProjectSchemaRuleExecutor() = default;

    virtual void uniqueAcrossProject(const QStringList& fields) = 0;
    virtual void documentIdMirrorsRoot(const QStringList& documentTypes) = 0;
    virtual void projectIdUniform() = 0;
    virtual void referencesResolve(const QList<ProjectSchemaReferenceRule>& references) = 0;
    virtual void acyclic(const QStringList& referencePath) = 0;
    virtual void singleton(const QString& documentType,
                           const QStringList& predicatePath,
                           const QString& expectedValue,
                           const QString& message) = 0;
    virtual void schemaVersionUniform() = 0;
};

}

namespace VNX::GeneratedSchemaRegistry {

bool validateProjectRules(int version,
                          VNX::ProjectSchemaRuleExecutor& executor,
                          QStringList& errors);

}
