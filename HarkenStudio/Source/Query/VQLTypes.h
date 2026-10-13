
#pragma once

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include "../Core/HarkenMath.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <functional>

namespace VNX {

struct ProjectItem;
struct ProjectData;
struct VQLError;


enum class VQLTokenType
{
    Dollar,
    Slash,
    StringLiteral,
    Identifier,
    TypePrefix,
    Where,
    Select,
    And,
    Or,
    Not,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    Like,
    NumberLiteral,
    BoolLiteral,
    Star,
    Comma,
    LParen,
    RParen,
    Eof
};

struct VQLToken
{
    VQLTokenType type = VQLTokenType::Eof;
    QString      text;
    double       numberValue = 0.0;
    bool         boolValue   = false;
    int          line        = 1;
    int          column      = 0;
};


using VQLValue = std::variant<std::monostate, double, bool, QString>;

inline bool vqlValueIsNull(const VQLValue& v) { return std::holds_alternative<std::monostate>(v); }


enum class VQLCompOp { Eq, Ne, Lt, Gt, Le, Ge, Like };

struct VQLPredicate;
using VQLPredicatePtr = std::shared_ptr<VQLPredicate>;

struct VQLPredicate
{
    enum class Kind { Comparison, And, Or, Not };

    Kind kind = Kind::Comparison;

    QString   fieldName;
    VQLCompOp op = VQLCompOp::Eq;
    VQLValue  value;

    VQLPredicatePtr left;
    VQLPredicatePtr right;

    static VQLPredicatePtr makeComparison(const QString& field, VQLCompOp op, VQLValue val)
    {
        auto p = std::make_shared<VQLPredicate>();
        p->kind      = Kind::Comparison;
        p->fieldName = field;
        p->op        = op;
        p->value     = val;
        return p;
    }

    static VQLPredicatePtr makeAnd(VQLPredicatePtr l, VQLPredicatePtr r)
    {
        auto p = std::make_shared<VQLPredicate>();
        p->kind  = Kind::And;
        p->left  = std::move(l);
        p->right = std::move(r);
        return p;
    }

    static VQLPredicatePtr makeOr(VQLPredicatePtr l, VQLPredicatePtr r)
    {
        auto p = std::make_shared<VQLPredicate>();
        p->kind  = Kind::Or;
        p->left  = std::move(l);
        p->right = std::move(r);
        return p;
    }

    static VQLPredicatePtr makeNot(VQLPredicatePtr child)
    {
        auto p = std::make_shared<VQLPredicate>();
        p->kind = Kind::Not;
        p->left = std::move(child);
        return p;
    }
};

struct VQLPathStep
{
    enum class Kind { Name, TypeFilter, Wildcard };

    Kind    kind = Kind::Name;
    QString value;
};


enum class VQLAggregateType { Count, Sum, Avg, Min, Max };

struct VQLAggregate
{
    VQLAggregateType type = VQLAggregateType::Count;
    QString          fieldName;
};

struct VQLQueryAST
{
    QString                    rootSelector;
    std::vector<VQLPathStep>   pathSteps;
    VQLPredicatePtr            predicate;
    std::vector<QString>       returnFields;
    std::vector<VQLAggregate>  aggregates;
};

using VQLCompiledQuery = std::shared_ptr<VQLQueryAST>;


class VQLResultSet
{
public:
    VQLResultSet() = default;
    explicit VQLResultSet(std::vector<ProjectItem*> items, const ProjectData* project = nullptr)
        : items_(std::move(items)), project_(project) {}

    bool   empty()  const { return items_.empty(); }
    size_t count()  const { return items_.size(); }

    const std::vector<ProjectItem*>& items() const { return items_; }
    std::vector<ProjectItem*>&       items()       { return items_; }

    using iterator       = std::vector<ProjectItem*>::iterator;
    using const_iterator = std::vector<ProjectItem*>::const_iterator;

    iterator       begin()       { return items_.begin(); }
    iterator       end()         { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end()   const { return items_.end(); }

    QJsonArray toJson(const std::vector<QString>& returnFields,
                      VQLError* outError = nullptr) const;

    bool hasAggregates() const { return !aggregateResults_.isEmpty(); }
    const QJsonObject& aggregateResults() const { return aggregateResults_; }
    void setAggregateResults(const QJsonObject& obj) { aggregateResults_ = obj; }

private:
    std::vector<ProjectItem*> items_;
    const ProjectData* project_ = nullptr;
    QJsonObject aggregateResults_;
};


struct VQLError
{
    QString message;
    int     line   = 0;
    int     column = 0;

    bool    isError() const { return !message.isEmpty(); }
    QString toString() const
    {
        if (line > 0)
            return QString("VQL error at %1:%2: %3").arg(line).arg(column).arg(message);
        return QString("VQL error: %1").arg(message);
    }
};

inline uint32_t vqlFnv1a(const char* str) { return HarkenEditor::hashString(str); }

inline uint32_t vqlFnv1a(const QString& s)
{
    QByteArray utf8 = s.toUtf8();
    return HarkenEditor::hashString(utf8.constData());
}

}
