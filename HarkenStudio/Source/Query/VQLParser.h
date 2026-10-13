
#pragma once

#include "VQLTypes.h"
#include <vector>

namespace VNX {

class VQLParser
{
public:
    explicit VQLParser(std::vector<VQLToken> tokens);

    VQLCompiledQuery parse(VQLError* outError = nullptr);

private:
    const VQLToken& current() const;
    const VQLToken& peekNext() const;
    bool            check(VQLTokenType type) const;
    bool            match(VQLTokenType type);
    const VQLToken& consume(VQLTokenType type, const QString& errMsg);
    void            error(const QString& msg);

    void            parseRootSelector(VQLQueryAST& ast);
    void            parsePathSteps(VQLQueryAST& ast);
    void            parseWhereClause(VQLQueryAST& ast);
    void            parseSelectClause(VQLQueryAST& ast);
    bool            tryParseAggregate(VQLQueryAST& ast);

    VQLPredicatePtr parseOrExpr();
    VQLPredicatePtr parseAndExpr();
    VQLPredicatePtr parseNotExpr();
    VQLPredicatePtr parsePrimaryPred();
    VQLPredicatePtr parseComparison();
    VQLCompOp       parseCompOp();
    VQLValue        parseValue();

    std::vector<VQLToken> tokens_;
    int                   pos_      = 0;
    bool                  hadError_ = false;
    VQLError              lastError_;
};

}
