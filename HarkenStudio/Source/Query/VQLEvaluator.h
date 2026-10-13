
#pragma once

#include "VQLTypes.h"
#include "IVQLDataSource.h"

namespace VNX {

struct ProjectData;
struct ProjectItem;

class VQLEvaluator
{
public:
    void setDataSource(IVQLDataSource* ds) { dataSource_ = ds; }

    VQLResultSet evaluate(const VQLQueryAST& ast, ProjectData* project, VQLError* outError = nullptr);

    static QJsonObject itemToJson(const ProjectItem* item,
                                  const std::vector<QString>& fields,
                                  VQLError* outError = nullptr,
                                  const ProjectData* project = nullptr);

private:
    IVQLDataSource* dataSource_ = nullptr;

    void applyPathSteps(const std::vector<VQLPathStep>& steps,
                        std::vector<ProjectItem*>& candidates);

    void applyPredicate(const VQLPredicatePtr& pred,
                        std::vector<ProjectItem*>& candidates);

    bool evaluatePredicate(const VQLPredicate* pred, const ProjectItem* item);
    bool evaluateComparison(const QString& field, VQLCompOp op,
                            const VQLValue& value, const ProjectItem* item);

    bool compareValues(const VQLValue& lhs, VQLCompOp op, const VQLValue& rhs);

    static bool wildcardMatch(const QString& pattern, const QString& text);

    static QString typeToString(int type);
    static int     stringToType(const QString& name);

    static QJsonObject computeAggregates(const std::vector<VQLAggregate>& aggregates,
                                         const std::vector<ProjectItem*>& items,
                                         IVQLDataSource* ds);
};

}
