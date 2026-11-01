
#include "AIMarkdownRenderer.h"

#include <QGuiApplication>
#include <QStringList>

namespace VNX {
namespace AIMarkdownRenderer {
namespace {

// Nesting cap for emphasis recursion (bold > italic > code is depth 3).
constexpr int kMaxInlineDepth = 4;
// Escaping alone expands at most 6x ("\"" -> "&quot;"); subset tags add a
// bounded constant per consumed marker, so any output beyond this multiple
// signals a renderer defect and triggers the plain-escaped fallback.
constexpr int kMaxExpansionFactor = 12;
constexpr int kExpansionSlack = 256;

void appendEscaped(QString& out, QChar ch)
{
    switch (ch.unicode())
    {
    case u'&': out += QLatin1String("&amp;"); break;
    case u'<': out += QLatin1String("&lt;"); break;
    case u'>': out += QLatin1String("&gt;"); break;
    case u'"': out += QLatin1String("&quot;"); break;
    case u'\'': out += QLatin1String("&#39;"); break;
    default: out += ch; break;
    }
}

void appendEscaped(QString& out, const QString& text)
{
    for (const QChar ch : text)
        appendEscaped(out, ch);
}

bool isWordChar(QChar ch)
{
    return ch.isLetterOrNumber() || ch == QLatin1Char('_');
}

enum EmphasisKind
{
    BoldAsterisk = 0,
    BoldUnderscore,
    ItalicAsterisk,
    ItalicUnderscore,
    EmphasisKindCount
};

struct EmphasisSpec
{
    QChar marker;
    int length;
    const char* openTag;
    const char* closeTag;
};

const EmphasisSpec kEmphasis[EmphasisKindCount] = {
    {QLatin1Char('*'), 2, "<b>", "</b>"},
    {QLatin1Char('_'), 2, "<b>", "</b>"},
    {QLatin1Char('*'), 1, "<i>", "</i>"},
    {QLatin1Char('_'), 1, "<i>", "</i>"},
};

struct InlineScanState
{
    // Once a close-scan for a kind reaches the end of the text without a
    // valid closing marker, no later open of that kind can close either
    // (close validity is position-local). Remembering that keeps repeated
    // unmatched markers linear instead of quadratic.
    bool exhausted[EmphasisKindCount] = {false, false, false, false};
    bool backtickExhausted = false;
};

void renderInline(QString& out, const QString& text, int depth);

bool isValidClose(const QString& text, int idx, const EmphasisSpec& spec)
{
    if (text.at(idx - 1).isSpace())
        return false;
    if (spec.marker == QLatin1Char('_'))
    {
        const int after = idx + spec.length;
        if (after < text.size() && isWordChar(text.at(after)))
            return false;
    }
    return true;
}

// Attempts to render an emphasis span opening at position i. Returns the
// position after the consumed span, or -1 when the marker must stay literal.
int tryEmphasis(QString& out, const QString& text, int i, EmphasisKind kind,
                InlineScanState& state, int depth)
{
    const EmphasisSpec& spec = kEmphasis[kind];
    const int n = text.size();
    if (spec.marker == QLatin1Char('_') && i > 0 && isWordChar(text.at(i - 1)))
        return -1;
    const int contentStart = i + spec.length;
    if (contentStart >= n || text.at(contentStart).isSpace())
        return -1;
    if (state.exhausted[kind])
        return -1;

    const QString marker(spec.length, spec.marker);
    int searchFrom = contentStart + 1;
    while (true)
    {
        const int idx = text.indexOf(marker, searchFrom);
        if (idx < 0)
        {
            state.exhausted[kind] = true;
            return -1;
        }
        if (isValidClose(text, idx, spec))
        {
            out += QLatin1String(spec.openTag);
            renderInline(out, text.mid(contentStart, idx - contentStart), depth + 1);
            out += QLatin1String(spec.closeTag);
            return idx + spec.length;
        }
        searchFrom = idx + 1;
    }
}

void renderInline(QString& out, const QString& text, int depth)
{
    if (depth > kMaxInlineDepth)
    {
        appendEscaped(out, text);
        return;
    }

    InlineScanState state;
    const int n = text.size();
    int i = 0;
    while (i < n)
    {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('`'))
        {
            if (!state.backtickExhausted)
            {
                const int close = text.indexOf(QLatin1Char('`'), i + 1);
                if (close < 0)
                {
                    state.backtickExhausted = true;
                }
                else if (close > i + 1)
                {
                    out += QLatin1String("<code>");
                    appendEscaped(out, text.mid(i + 1, close - i - 1));
                    out += QLatin1String("</code>");
                    i = close + 1;
                    continue;
                }
            }
        }
        else if (ch == QLatin1Char('*') || ch == QLatin1Char('_'))
        {
            const bool doubled = i + 1 < n && text.at(i + 1) == ch;
            EmphasisKind kind;
            if (doubled)
                kind = ch == QLatin1Char('*') ? BoldAsterisk : BoldUnderscore;
            else
                kind = ch == QLatin1Char('*') ? ItalicAsterisk : ItalicUnderscore;
            const int next = tryEmphasis(out, text, i, kind, state, depth);
            if (next > 0)
            {
                i = next;
                continue;
            }
        }
        appendEscaped(out, ch);
        ++i;
    }
}

enum class ListMode
{
    None,
    Unordered,
    Ordered
};

bool parseUnorderedItem(const QString& trimmed, QString* content)
{
    if (trimmed.size() >= 2
        && (trimmed.startsWith(QLatin1String("- "))
            || trimmed.startsWith(QLatin1String("* "))))
    {
        *content = trimmed.mid(2);
        return true;
    }
    return false;
}

bool parseOrderedItem(const QString& trimmed, QString* content)
{
    int i = 0;
    while (i < trimmed.size() && i < 9 && trimmed.at(i).isDigit())
        ++i;
    if (i == 0 || i + 1 >= trimmed.size())
        return false;
    if (trimmed.at(i) != QLatin1Char('.') || trimmed.at(i + 1) != QLatin1Char(' '))
        return false;
    *content = trimmed.mid(i + 2);
    return true;
}

bool isTableRow(const QString& trimmed)
{
    return trimmed.size() >= 2 && trimmed.startsWith(QLatin1Char('|'));
}

QStringList splitTableRow(const QString& trimmed)
{
    QString inner = trimmed.mid(1);
    if (inner.endsWith(QLatin1Char('|')))
        inner.chop(1);
    QStringList cells = inner.split(QLatin1Char('|'));
    for (QString& cell : cells)
        cell = cell.trimmed();
    return cells;
}

bool isTableSeparator(const QString& trimmed, int columnCount)
{
    if (!isTableRow(trimmed))
        return false;
    const QStringList cells = splitTableRow(trimmed);
    if (cells.size() != columnCount)
        return false;
    for (const QString& cell : cells)
    {
        int begin = cell.startsWith(QLatin1Char(':')) ? 1 : 0;
        int end = cell.endsWith(QLatin1Char(':')) ? cell.size() - 1 : cell.size();
        if (end <= begin)
            return false;
        for (int i = begin; i < end; ++i)
        {
            if (cell.at(i) != QLatin1Char('-'))
                return false;
        }
    }
    return true;
}

int headingLevel(const QString& trimmed)
{
    int hashes = 0;
    while (hashes < trimmed.size() && trimmed.at(hashes) == QLatin1Char('#'))
        ++hashes;
    if (hashes < 1 || hashes > 3)
        return 0;
    if (hashes >= trimmed.size() || trimmed.at(hashes) != QLatin1Char(' '))
        return 0;
    return hashes;
}

QString renderBlocks(const QString& text, const QColor& tableBorderColor)
{
    QString normalized = text;
    normalized.remove(QLatin1Char('\r'));
    const QStringList lines = normalized.split(QLatin1Char('\n'));

    QString out;
    ListMode list = ListMode::None;
    bool prevInline = false;
    // A failed closer scan proves no exact ``` line remains anywhere ahead,
    // so later fence openers can skip scanning entirely. Successful scans
    // consume the scanned lines into the block, keeping the pass linear.
    bool fenceCloserExhausted = false;

    const auto closeList = [&out, &list]() {
        if (list == ListMode::Unordered)
            out += QLatin1String("</ul>");
        else if (list == ListMode::Ordered)
            out += QLatin1String("</ol>");
        list = ListMode::None;
    };

    for (int li = 0; li < lines.size(); ++li)
    {
        const QString& line = lines.at(li);
        const QString trimmed = line.trimmed();

        if (trimmed.startsWith(QLatin1String("```")) && !fenceCloserExhausted)
        {
            int closeIdx = -1;
            for (int j = li + 1; j < lines.size(); ++j)
            {
                if (lines.at(j).trimmed() == QLatin1String("```"))
                {
                    closeIdx = j;
                    break;
                }
            }
            if (closeIdx < 0)
                fenceCloserExhausted = true;
            if (closeIdx >= 0)
            {
                closeList();
                QString code;
                for (int j = li + 1; j < closeIdx; ++j)
                {
                    if (j > li + 1)
                        code += QLatin1Char('\n');
                    code += lines.at(j);
                }
                out += QLatin1String("<pre>");
                appendEscaped(out, code);
                out += QLatin1String("</pre>");
                li = closeIdx;
                prevInline = false;
                continue;
            }
            // Unclosed fence: the marker line stays literal text.
        }

        if (isTableRow(trimmed) && li + 1 < lines.size()
            && isTableSeparator(lines.at(li + 1).trimmed(),
                                splitTableRow(trimmed).size()))
        {
            closeList();
            const QStringList headerCells = splitTableRow(trimmed);
            // QSS cannot reach rich-text internals, so the grid styling must
            // ride inline.
            out += QLatin1String(
                "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" "
                "style=\"border-collapse:collapse;border-color:");
            out += tableBorderColor.name(QColor::HexArgb);
            out += QLatin1String(";border-style:solid\"><tr>");
            for (const QString& cell : headerCells)
            {
                out += QLatin1String("<th>");
                renderInline(out, cell, 0);
                out += QLatin1String("</th>");
            }
            out += QLatin1String("</tr>");
            int row = li + 2;
            for (; row < lines.size(); ++row)
            {
                const QString rowTrimmed = lines.at(row).trimmed();
                if (!isTableRow(rowTrimmed))
                    break;
                const QStringList cells = splitTableRow(rowTrimmed);
                out += QLatin1String("<tr>");
                // GFM row semantics: short rows pad with empty cells, excess
                // cells are dropped, keeping every row at the header width.
                for (int c = 0; c < headerCells.size(); ++c)
                {
                    out += QLatin1String("<td>");
                    if (c < cells.size())
                        renderInline(out, cells.at(c), 0);
                    out += QLatin1String("</td>");
                }
                out += QLatin1String("</tr>");
            }
            out += QLatin1String("</table>");
            li = row - 1;
            prevInline = false;
            continue;
        }

        QString content;
        if (parseUnorderedItem(trimmed, &content))
        {
            if (list != ListMode::Unordered)
            {
                closeList();
                out += QLatin1String("<ul>");
                list = ListMode::Unordered;
            }
            out += QLatin1String("<li>");
            renderInline(out, content, 0);
            out += QLatin1String("</li>");
            prevInline = false;
            continue;
        }
        if (parseOrderedItem(trimmed, &content))
        {
            if (list != ListMode::Ordered)
            {
                closeList();
                out += QLatin1String("<ol>");
                list = ListMode::Ordered;
            }
            out += QLatin1String("<li>");
            renderInline(out, content, 0);
            out += QLatin1String("</li>");
            prevInline = false;
            continue;
        }

        closeList();

        if (const int level = headingLevel(trimmed))
        {
            // h2..h4 scale relative to the label's base font, so the panel
            // font-size setting keeps applying.
            const int tag = level + 1;
            out += QStringLiteral("<h%1>").arg(tag);
            renderInline(out, trimmed.mid(level + 1).trimmed(), 0);
            out += QStringLiteral("</h%1>").arg(tag);
            prevInline = false;
            continue;
        }

        if (prevInline)
            out += QLatin1String("<br/>");
        renderInline(out, line, 0);
        prevInline = true;
    }
    closeList();
    return out;
}

}

QString renderToHtml(const QString& text)
{
    return renderToHtml(text, QGuiApplication::palette().color(QPalette::Mid));
}

QString renderToHtml(const QString& text, const QColor& tableBorderColor)
{
    QString html = renderBlocks(text, tableBorderColor);
    if (html.size() > text.size() * kMaxExpansionFactor + kExpansionSlack)
    {
        QString fallback;
        for (const QChar ch : text)
        {
            if (ch == QLatin1Char('\n'))
                fallback += QLatin1String("<br/>");
            else if (ch != QLatin1Char('\r'))
                appendEscaped(fallback, ch);
        }
        return fallback;
    }
    return html;
}

}
}
