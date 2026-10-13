
#pragma once

#include <QColor>
#include <QString>

namespace VNX {
namespace AIMarkdownRenderer {

// Converts untrusted AI response text into a safe Qt rich-text (HTML subset)
// string. All untrusted content is escaped at emission; only renderer-generated
// tags for a strict markdown subset (bold, italic, inline code, fenced code
// blocks, headings, flat lists, pipe tables, line breaks) are raw, so input
// HTML can never
// become live markup. Unbalanced markers stay as escaped literals.
QString renderToHtml(const QString& text);
QString renderToHtml(const QString& text, const QColor& tableBorderColor);

}
}
