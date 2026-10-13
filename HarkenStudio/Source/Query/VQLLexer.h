
#pragma once

#include "VQLTypes.h"
#include <vector>

namespace VNX {

class VQLLexer
{
public:
    explicit VQLLexer(const QString& source);

    std::vector<VQLToken> tokenize(VQLError* outError = nullptr);

private:
    QChar           peek() const;
    QChar           advance();
    bool            atEnd() const;
    void            skipWhitespace();
    VQLToken        readString(QChar quoteChar = '"');
    VQLToken        readNumber();
    VQLToken        readIdentifierOrKeyword();
    VQLToken        makeToken(VQLTokenType type, const QString& text = {}) const;

    QString         source_;
    int             pos_    = 0;
    int             line_   = 1;
    int             col_    = 1;
};

}
