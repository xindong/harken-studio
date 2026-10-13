
#include "VQLEvaluator.h"
#include "MemoryDataSource.h"
#include "VQLFieldCatalog.h"
#include "../Core/ItemTypeCatalog.h"
#include "../Core/ProjectTypes.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <limits>

namespace VNX {


QString VQLEvaluator::typeToString(int type)
{
    return ItemTypeCatalog::canonicalName(static_cast<ProjectItem::Type>(type));
}

int VQLEvaluator::stringToType(const QString& name)
{
    const auto type = ItemTypeCatalog::parseTypeName(name, Qt::CaseInsensitive, false);
    return type ? static_cast<int>(*type) : -1;
}


VQLResultSet VQLEvaluator::evaluate(const VQLQueryAST& ast, ProjectData* project, VQLError* outError)
{
    std::unique_ptr<MemoryDataSource> fallback;
    if (!dataSource_) {
        if (!project) {
            if (outError) outError->message = "No project loaded";
            return {};
        }
        fallback = std::make_unique<MemoryDataSource>();
        fallback->setProject(project);
        dataSource_ = fallback.get();
    }

    std::vector<ProjectItem*> candidates;
    dataSource_->collectRoots(ast.rootSelector, candidates);

    if (!ast.pathSteps.empty())
        applyPathSteps(ast.pathSteps, candidates);

    if (ast.predicate)
        applyPredicate(ast.predicate, candidates);

    VQLResultSet result(std::move(candidates), project);

    if (!ast.aggregates.empty()) {
        QJsonObject aggResult = computeAggregates(ast.aggregates, result.items(), dataSource_);
        result.setAggregateResults(aggResult);
    }

    if (fallback)
        dataSource_ = nullptr;

    return result;
}


void VQLEvaluator::applyPathSteps(const std::vector<VQLPathStep>& steps,
                                   std::vector<ProjectItem*>& candidates)
{
    for (const auto& step : steps) {
        std::vector<ProjectItem*> nextCandidates;

        switch (step.kind) {
        case VQLPathStep::Kind::TypeFilter: {
            for (auto* item : candidates) {
                if (dataSource_->matchesType(item, step.value))
                    nextCandidates.push_back(item);
            }
            break;
        }
        case VQLPathStep::Kind::Name: {
            for (auto* item : candidates) {
                if (item->name.compare(step.value, Qt::CaseInsensitive) == 0 ||
                    wildcardMatch(step.value, item->name))
                    nextCandidates.push_back(item);
            }
            break;
        }
        case VQLPathStep::Kind::Wildcard:
            nextCandidates = candidates;
            break;
        }

        candidates = std::move(nextCandidates);
    }
}


void VQLEvaluator::applyPredicate(const VQLPredicatePtr& pred,
                                   std::vector<ProjectItem*>& candidates)
{
    if (!pred) return;

    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
            [&](const ProjectItem* item) {
                return !evaluatePredicate(pred.get(), item);
            }),
        candidates.end());
}

bool VQLEvaluator::evaluatePredicate(const VQLPredicate* pred, const ProjectItem* item)
{
    if (!pred) return true;

    switch (pred->kind) {
    case VQLPredicate::Kind::Comparison:
        return evaluateComparison(pred->fieldName, pred->op, pred->value, item);

    case VQLPredicate::Kind::And:
        return evaluatePredicate(pred->left.get(), item)
            && evaluatePredicate(pred->right.get(), item);

    case VQLPredicate::Kind::Or:
        return evaluatePredicate(pred->left.get(), item)
            || evaluatePredicate(pred->right.get(), item);

    case VQLPredicate::Kind::Not:
        return !evaluatePredicate(pred->left.get(), item);
    }
    return false;
}

bool VQLEvaluator::evaluateComparison(const QString& field, VQLCompOp op,
                                       const VQLValue& value, const ProjectItem* item)
{
    VQLValue prop = dataSource_->getProperty(item, field);
    if (vqlValueIsNull(prop)) return false;

    if (op == VQLCompOp::Like && std::holds_alternative<QString>(prop)
                              && std::holds_alternative<QString>(value)) {
        return wildcardMatch(std::get<QString>(value), std::get<QString>(prop));
    }

    if (op == VQLCompOp::Eq && std::holds_alternative<QString>(prop)
                            && std::holds_alternative<QString>(value)) {
        const auto& pattern = std::get<QString>(value);
        if (pattern.contains('*'))
            return wildcardMatch(pattern, std::get<QString>(prop));
    }

    return compareValues(prop, op, value);
}


bool VQLEvaluator::compareValues(const VQLValue& lhs, VQLCompOp op, const VQLValue& rhs)
{
    if (std::holds_alternative<double>(lhs) && std::holds_alternative<double>(rhs)) {
        double l = std::get<double>(lhs);
        double r = std::get<double>(rhs);
        switch (op) {
            case VQLCompOp::Eq:   return l == r;
            case VQLCompOp::Ne:   return l != r;
            case VQLCompOp::Lt:   return l < r;
            case VQLCompOp::Gt:   return l > r;
            case VQLCompOp::Le:   return l <= r;
            case VQLCompOp::Ge:   return l >= r;
            case VQLCompOp::Like: return l == r;
        }
    }

    if (std::holds_alternative<QString>(lhs) && std::holds_alternative<QString>(rhs)) {
        const auto& l = std::get<QString>(lhs);
        const auto& r = std::get<QString>(rhs);
        int cmp = l.compare(r, Qt::CaseInsensitive);
        switch (op) {
            case VQLCompOp::Eq:   return cmp == 0;
            case VQLCompOp::Ne:   return cmp != 0;
            case VQLCompOp::Lt:   return cmp < 0;
            case VQLCompOp::Gt:   return cmp > 0;
            case VQLCompOp::Le:   return cmp <= 0;
            case VQLCompOp::Ge:   return cmp >= 0;
            case VQLCompOp::Like: return wildcardMatch(r, l);
        }
    }

    if (std::holds_alternative<bool>(lhs) && std::holds_alternative<bool>(rhs)) {
        bool l = std::get<bool>(lhs);
        bool r = std::get<bool>(rhs);
        switch (op) {
            case VQLCompOp::Eq:   return l == r;
            case VQLCompOp::Ne:   return l != r;
            default:              return false;
        }
    }

    if (std::holds_alternative<QString>(lhs) && std::holds_alternative<QString>(rhs)) {
        return std::get<QString>(lhs).compare(std::get<QString>(rhs), Qt::CaseInsensitive) == 0
               && op == VQLCompOp::Eq;
    }

    return false;
}


bool VQLEvaluator::wildcardMatch(const QString& pattern, const QString& text)
{
    QString regexStr;
    regexStr.reserve(pattern.size() * 2);
    for (const QChar& ch : pattern) {
        if      (ch == '*') regexStr += ".*";
        else if (ch == '?') regexStr += ".";
        else                regexStr += QRegularExpression::escape(QString(ch));
    }

    QRegularExpression re("^" + regexStr + "$",
                          QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}


QJsonObject VQLEvaluator::itemToJson(const ProjectItem* item,
                                     const std::vector<QString>& fields,
                                     VQLError* outError,
                                     const ProjectData* project)
{
    if (!item) return {};

    auto addField = [&](QJsonObject& obj, const QString& f) {
        QString outputName;
        VQLValue value;
        if (!VQLFieldCatalog::project(item, f, &outputName, &value, project)) {
            if (outError && !outError->isError())
                outError->message = QStringLiteral("Unknown VQL field '%1'").arg(f);
            return;
        }
        if (std::holds_alternative<double>(value))
            obj[outputName] = std::get<double>(value);
        else if (std::holds_alternative<bool>(value))
            obj[outputName] = std::get<bool>(value);
        else if (std::holds_alternative<QString>(value))
            obj[outputName] = std::get<QString>(value);
        else
            obj[outputName] = QJsonValue::Null;
    };

    QJsonObject obj;
    if (fields.empty()) {
        for (const QString& field : {QStringLiteral("name"),
                                     QStringLiteral("type"),
                                     QStringLiteral("objectID"),
                                     QStringLiteral("path"),
                                     QStringLiteral("childCount")})
            addField(obj, field);
    } else {
        for (const auto& f : fields)
            addField(obj, f);
    }
    return obj;
}


QJsonArray VQLResultSet::toJson(const std::vector<QString>& returnFields,
                                VQLError* outError) const
{
    QJsonArray arr;
    for (const auto* item : items_) {
        arr.append(VQLEvaluator::itemToJson(item, returnFields, outError, project_));
        if (outError && outError->isError()) return {};
    }
    return arr;
}


QJsonObject VQLEvaluator::computeAggregates(const std::vector<VQLAggregate>& aggregates,
                                             const std::vector<ProjectItem*>& items,
                                             IVQLDataSource* ds)
{
    QJsonObject result;

    for (const auto& agg : aggregates) {
        QString key;
        switch (agg.type) {
        case VQLAggregateType::Count: key = QStringLiteral("count(%1)").arg(agg.fieldName); break;
        case VQLAggregateType::Sum:   key = QStringLiteral("sum(%1)").arg(agg.fieldName); break;
        case VQLAggregateType::Avg:   key = QStringLiteral("avg(%1)").arg(agg.fieldName); break;
        case VQLAggregateType::Min:   key = QStringLiteral("min(%1)").arg(agg.fieldName); break;
        case VQLAggregateType::Max:   key = QStringLiteral("max(%1)").arg(agg.fieldName); break;
        }

        if (agg.type == VQLAggregateType::Count) {
            result[key] = static_cast<int>(items.size());
            continue;
        }

        double sum = 0.0;
        double minVal = std::numeric_limits<double>::max();
        double maxVal = std::numeric_limits<double>::lowest();
        int numericCount = 0;

        for (const auto* item : items) {
            VQLValue val = ds->getProperty(item, agg.fieldName);
            if (std::holds_alternative<double>(val)) {
                double v = std::get<double>(val);
                sum += v;
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
                ++numericCount;
            }
        }

        if (numericCount == 0) {
            result[key] = QJsonValue::Null;
            continue;
        }

        switch (agg.type) {
        case VQLAggregateType::Sum: result[key] = sum; break;
        case VQLAggregateType::Avg: result[key] = sum / numericCount; break;
        case VQLAggregateType::Min: result[key] = minVal; break;
        case VQLAggregateType::Max: result[key] = maxVal; break;
        default: break;
        }
    }

    return result;
}

}
