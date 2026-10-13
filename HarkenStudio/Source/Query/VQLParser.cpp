
#include "VQLParser.h"

namespace VNX {

static const VQLToken kEofToken = { VQLTokenType::Eof, {}, 0.0, false, 0, 0 };

VQLParser::VQLParser(std::vector<VQLToken> tokens)
    : tokens_(std::move(tokens))
{
}

const VQLToken& VQLParser::current() const
{
    if (pos_ >= static_cast<int>(tokens_.size())) return kEofToken;
    return tokens_[pos_];
}

const VQLToken& VQLParser::peekNext() const
{
    int next = pos_ + 1;
    if (next >= static_cast<int>(tokens_.size())) return kEofToken;
    return tokens_[next];
}

bool VQLParser::check(VQLTokenType type) const
{
    return current().type == type;
}

bool VQLParser::match(VQLTokenType type)
{
    if (check(type)) { ++pos_; return true; }
    return false;
}

const VQLToken& VQLParser::consume(VQLTokenType type, const QString& errMsg)
{
    if (check(type)) {
        const VQLToken& t = current();
        ++pos_;
        return t;
    }
    error(errMsg);
    return kEofToken;
}

void VQLParser::error(const QString& msg)
{
    if (!hadError_) {
        hadError_ = true;
        lastError_.message = msg;
        lastError_.line    = current().line;
        lastError_.column  = current().column;
    }
}


VQLCompiledQuery VQLParser::parse(VQLError* outError)
{
    auto ast = std::make_shared<VQLQueryAST>();

    if (!match(VQLTokenType::Dollar)) {
    }

    parseRootSelector(*ast);
    parsePathSteps(*ast);

    if (check(VQLTokenType::Where))
        parseWhereClause(*ast);

    if (check(VQLTokenType::Select))
        parseSelectClause(*ast);

    if (hadError_ && outError)
        *outError = lastError_;

    return hadError_ ? nullptr : ast;
}


void VQLParser::parseRootSelector(VQLQueryAST& ast)
{
    if (check(VQLTokenType::StringLiteral)) {
        ast.rootSelector = current().text;
        ++pos_;
    }
}


void VQLParser::parsePathSteps(VQLQueryAST& ast)
{
    while (match(VQLTokenType::Slash)) {
        if (hadError_) return;

        VQLPathStep step;

        if (check(VQLTokenType::TypePrefix)) {
            step.kind  = VQLPathStep::Kind::TypeFilter;
            step.value = current().text;
            ++pos_;
        }
        else if (check(VQLTokenType::Star)) {
            step.kind = VQLPathStep::Kind::Wildcard;
            ++pos_;
        }
        else if (check(VQLTokenType::StringLiteral)) {
            step.kind  = VQLPathStep::Kind::Name;
            step.value = current().text;
            ++pos_;
        }
        else if (check(VQLTokenType::Identifier)) {
            step.kind  = VQLPathStep::Kind::Name;
            step.value = current().text;
            ++pos_;
        }
        else {
            error("Expected path step after '/'");
            return;
        }

        ast.pathSteps.push_back(std::move(step));
    }

    if (check(VQLTokenType::TypePrefix) && ast.pathSteps.empty() && ast.rootSelector.isEmpty()) {
        VQLPathStep step;
        step.kind  = VQLPathStep::Kind::TypeFilter;
        step.value = current().text;
        ++pos_;
        ast.pathSteps.push_back(std::move(step));
    }
}


void VQLParser::parseWhereClause(VQLQueryAST& ast)
{
    consume(VQLTokenType::Where, "Expected 'where'");
    if (hadError_) return;
    ast.predicate = parseOrExpr();
}


void VQLParser::parseSelectClause(VQLQueryAST& ast)
{
    consume(VQLTokenType::Select, "Expected 'select'");
    if (hadError_) return;

    auto parseOneSelectItem = [&]() -> bool {
        if (tryParseAggregate(ast))
            return true;

        if (check(VQLTokenType::Identifier)) {
            ast.returnFields.push_back(current().text);
            ++pos_;
            return true;
        }
        return false;
    };

    if (!parseOneSelectItem()) {
        error("Expected field name or aggregate function after 'select'");
        return;
    }

    while (match(VQLTokenType::Comma)) {
        if (!parseOneSelectItem()) {
            error("Expected field name or aggregate function after ','");
            return;
        }
    }
}

bool VQLParser::tryParseAggregate(VQLQueryAST& ast)
{
    if (!check(VQLTokenType::Identifier)) return false;

    QString funcName = current().text.toLower();
    VQLAggregateType aggType;
    if      (funcName == "count") aggType = VQLAggregateType::Count;
    else if (funcName == "sum")   aggType = VQLAggregateType::Sum;
    else if (funcName == "avg")   aggType = VQLAggregateType::Avg;
    else if (funcName == "min")   aggType = VQLAggregateType::Min;
    else if (funcName == "max")   aggType = VQLAggregateType::Max;
    else return false;

    if (peekNext().type != VQLTokenType::LParen)
        return false;

    ++pos_;
    consume(VQLTokenType::LParen, "Expected '(' after aggregate function");
    if (hadError_) return true;

    VQLAggregate agg;
    agg.type = aggType;

    if (check(VQLTokenType::Star)) {
        agg.fieldName = QStringLiteral("*");
        ++pos_;
    } else if (check(VQLTokenType::Identifier)) {
        agg.fieldName = current().text;
        ++pos_;
    } else {
        error("Expected field name or '*' in aggregate function");
        return true;
    }

    consume(VQLTokenType::RParen, "Expected ')' after aggregate argument");
    ast.aggregates.push_back(std::move(agg));
    return true;
}


VQLPredicatePtr VQLParser::parseOrExpr()
{
    auto left = parseAndExpr();
    while (match(VQLTokenType::Or)) {
        auto right = parseAndExpr();
        left = VQLPredicate::makeOr(std::move(left), std::move(right));
    }
    return left;
}

VQLPredicatePtr VQLParser::parseAndExpr()
{
    auto left = parseNotExpr();
    while (match(VQLTokenType::And)) {
        auto right = parseNotExpr();
        left = VQLPredicate::makeAnd(std::move(left), std::move(right));
    }
    return left;
}

VQLPredicatePtr VQLParser::parseNotExpr()
{
    if (match(VQLTokenType::Not)) {
        auto child = parseNotExpr();
        return VQLPredicate::makeNot(std::move(child));
    }
    return parsePrimaryPred();
}

VQLPredicatePtr VQLParser::parsePrimaryPred()
{
    if (match(VQLTokenType::LParen)) {
        auto expr = parseOrExpr();
        consume(VQLTokenType::RParen, "Expected ')'");
        return expr;
    }
    return parseComparison();
}

VQLPredicatePtr VQLParser::parseComparison()
{
    if (!check(VQLTokenType::Identifier)) {
        error(QString("Expected field name in predicate, got '%1'").arg(current().text));
        return nullptr;
    }

    QString fieldName = current().text;
    ++pos_;

    VQLCompOp op = parseCompOp();
    if (hadError_) return nullptr;

    VQLValue val = parseValue();
    if (hadError_) return nullptr;

    return VQLPredicate::makeComparison(fieldName, op, val);
}

VQLCompOp VQLParser::parseCompOp()
{
    if (match(VQLTokenType::Eq))   return VQLCompOp::Eq;
    if (match(VQLTokenType::Ne))   return VQLCompOp::Ne;
    if (match(VQLTokenType::Lt))   return VQLCompOp::Lt;
    if (match(VQLTokenType::Gt))   return VQLCompOp::Gt;
    if (match(VQLTokenType::Le))   return VQLCompOp::Le;
    if (match(VQLTokenType::Ge))   return VQLCompOp::Ge;
    if (match(VQLTokenType::Like)) return VQLCompOp::Like;

    error("Expected comparison operator (=, !=, <, >, <=, >=, like)");
    return VQLCompOp::Eq;
}

VQLValue VQLParser::parseValue()
{
    if (check(VQLTokenType::NumberLiteral)) {
        double v = current().numberValue;
        ++pos_;
        return VQLValue(v);
    }
    if (check(VQLTokenType::StringLiteral)) {
        QString v = current().text;
        ++pos_;
        return VQLValue(v);
    }
    if (check(VQLTokenType::BoolLiteral)) {
        bool v = current().boolValue;
        ++pos_;
        return VQLValue(v);
    }
    if (check(VQLTokenType::Identifier)) {
        QString v = current().text;
        ++pos_;
        return VQLValue(v);
    }

    error("Expected value (number, string, boolean, or identifier)");
    return VQLValue{};
}

}
