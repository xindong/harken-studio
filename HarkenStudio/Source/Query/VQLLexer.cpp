
#include "VQLLexer.h"

namespace VNX {

VQLLexer::VQLLexer(const QString& source)
    : source_(source)
{
}

QChar VQLLexer::peek() const
{
    if (pos_ >= source_.size()) return QChar('\0');
    return source_[pos_];
}

QChar VQLLexer::advance()
{
    QChar ch = source_[pos_++];
    if (ch == '\n') { ++line_; col_ = 1; }
    else            { ++col_; }
    return ch;
}

bool VQLLexer::atEnd() const
{
    return pos_ >= source_.size();
}

void VQLLexer::skipWhitespace()
{
    while (!atEnd() && peek().isSpace())
        advance();
}

VQLToken VQLLexer::makeToken(VQLTokenType type, const QString& text) const
{
    VQLToken t;
    t.type   = type;
    t.text   = text;
    t.line   = line_;
    t.column = col_;
    return t;
}

VQLToken VQLLexer::readString(QChar quoteChar)
{
    advance();
    QString result;
    while (!atEnd() && peek() != quoteChar) {
        QChar ch = advance();
        if (ch == '\\' && !atEnd()) {
            QChar esc = advance();
            if      (esc == quoteChar) result += quoteChar;
            else if (esc == '\\') result += '\\';
            else if (esc == 'n')  result += '\n';
            else                  { result += '\\'; result += esc; }
        } else {
            result += ch;
        }
    }
    if (!atEnd()) advance();

    VQLToken t = makeToken(VQLTokenType::StringLiteral, result);
    return t;
}

VQLToken VQLLexer::readNumber()
{
    int startCol = col_;
    QString num;

    if (peek() == '-') num += advance();

    while (!atEnd() && (peek().isDigit() || peek() == '.'))
        num += advance();

    VQLToken t;
    t.type        = VQLTokenType::NumberLiteral;
    t.text        = num;
    t.numberValue = num.toDouble();
    t.line        = line_;
    t.column      = startCol;
    return t;
}

VQLToken VQLLexer::readIdentifierOrKeyword()
{
    int startCol = col_;
    QString id;

    while (!atEnd() && (peek().isLetterOrNumber() || peek() == '_'))
        id += advance();

    if (id.compare("type", Qt::CaseInsensitive) == 0 && !atEnd() && peek() == ':') {
        advance();
        QString typeName;
        while (!atEnd() && (peek().isLetterOrNumber() || peek() == '_'))
            typeName += advance();
        VQLToken t;
        t.type   = VQLTokenType::TypePrefix;
        t.text   = typeName;
        t.line   = line_;
        t.column = startCol;
        return t;
    }

    QString lower = id.toLower();
    VQLTokenType kwType = VQLTokenType::Identifier;

    if      (lower == "where")  kwType = VQLTokenType::Where;
    else if (lower == "select") kwType = VQLTokenType::Select;
    else if (lower == "and")    kwType = VQLTokenType::And;
    else if (lower == "or")     kwType = VQLTokenType::Or;
    else if (lower == "not")    kwType = VQLTokenType::Not;
    else if (lower == "like")   kwType = VQLTokenType::Like;
    else if (lower == "true")  {
        VQLToken t;
        t.type      = VQLTokenType::BoolLiteral;
        t.text      = id;
        t.boolValue = true;
        t.line      = line_;
        t.column    = startCol;
        return t;
    }
    else if (lower == "false") {
        VQLToken t;
        t.type      = VQLTokenType::BoolLiteral;
        t.text      = id;
        t.boolValue = false;
        t.line      = line_;
        t.column    = startCol;
        return t;
    }

    VQLToken t;
    t.type   = kwType;
    t.text   = id;
    t.line   = line_;
    t.column = startCol;
    return t;
}

std::vector<VQLToken> VQLLexer::tokenize(VQLError* outError)
{
    std::vector<VQLToken> tokens;

    while (true) {
        skipWhitespace();
        if (atEnd()) break;

        QChar ch = peek();

        if (ch == '$')  { advance(); tokens.push_back(makeToken(VQLTokenType::Dollar, "$")); }
        else if (ch == '/')  { advance(); tokens.push_back(makeToken(VQLTokenType::Slash, "/")); }
        else if (ch == '*')  { advance(); tokens.push_back(makeToken(VQLTokenType::Star, "*")); }
        else if (ch == ',')  { advance(); tokens.push_back(makeToken(VQLTokenType::Comma, ",")); }
        else if (ch == '(')  { advance(); tokens.push_back(makeToken(VQLTokenType::LParen, "(")); }
        else if (ch == ')')  { advance(); tokens.push_back(makeToken(VQLTokenType::RParen, ")")); }
        else if (ch == '"')  { tokens.push_back(readString('"')); }
        else if (ch == '\'') { tokens.push_back(readString('\'')); }
        else if (ch == '!') {
            advance();
            if (!atEnd() && peek() == '=') {
                advance();
                tokens.push_back(makeToken(VQLTokenType::Ne, "!="));
            } else {
                tokens.push_back(makeToken(VQLTokenType::Not, "!"));
            }
        }
        else if (ch == '=')  { advance(); tokens.push_back(makeToken(VQLTokenType::Eq, "=")); }
        else if (ch == '<') {
            advance();
            if (!atEnd() && peek() == '=') {
                advance();
                tokens.push_back(makeToken(VQLTokenType::Le, "<="));
            } else {
                tokens.push_back(makeToken(VQLTokenType::Lt, "<"));
            }
        }
        else if (ch == '>') {
            advance();
            if (!atEnd() && peek() == '=') {
                advance();
                tokens.push_back(makeToken(VQLTokenType::Ge, ">="));
            } else {
                tokens.push_back(makeToken(VQLTokenType::Gt, ">"));
            }
        }
        else if (ch == '-' && pos_ + 1 < source_.size() && source_[pos_ + 1].isDigit()) {
            tokens.push_back(readNumber());
        }
        else if (ch.isDigit()) {
            tokens.push_back(readNumber());
        }
        else if (ch.isLetter() || ch == '_') {
            tokens.push_back(readIdentifierOrKeyword());
        }
        else {
            if (outError) {
                outError->message = QString("Unexpected character '%1'").arg(ch);
                outError->line    = line_;
                outError->column  = col_;
            }
            advance();
        }
    }

    tokens.push_back(makeToken(VQLTokenType::Eof));
    return tokens;
}

}
