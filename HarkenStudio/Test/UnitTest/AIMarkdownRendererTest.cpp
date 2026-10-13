#include "AI/AIMarkdownRenderer.h"

#include <gtest/gtest.h>

#include <QColor>
#include <QString>

using VNX::AIMarkdownRenderer::renderToHtml;

namespace {

std::string toStd(const QString& text)
{
    return text.toStdString();
}

}

TEST(AIMarkdownRendererTest, PlainTextPassesThroughUnchanged)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("Mock response"))), "Mock response");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral(
                  "[Error] PROVIDER_NOT_FOUND: No LLM provider is configured."))),
              "[Error] PROVIDER_NOT_FOUND: No LLM provider is configured.");
    EXPECT_EQ(toStd(renderToHtml(QString())), "");
}

TEST(AIMarkdownRendererTest, CjkAndEmojiPassThroughIntact)
{
    const QString text = QString::fromUtf8(
        "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95 \xF0\x9F\x8E\xB5 mixed text");
    EXPECT_EQ(renderToHtml(text), text);
}

TEST(AIMarkdownRendererTest, BoldRenders)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("**x**"))), "<b>x</b>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("__x__"))), "<b>x</b>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("a **bold** b"))), "a <b>bold</b> b");
}

TEST(AIMarkdownRendererTest, ItalicRenders)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("*x*"))), "<i>x</i>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("_x_"))), "<i>x</i>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("a *it* b"))), "a <i>it</i> b");
}

TEST(AIMarkdownRendererTest, NestedItalicAndCodeInsideBold)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("**a *b* `c`**"))),
              "<b>a <i>b</i> <code>c</code></b>");
}

TEST(AIMarkdownRendererTest, InlineCodeRendersAndSuppressesMarkers)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("`x`"))), "<code>x</code>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("`**a**`"))), "<code>**a**</code>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("`<b>&</b>`"))),
              "<code>&lt;b&gt;&amp;&lt;/b&gt;</code>");
}

TEST(AIMarkdownRendererTest, FencedCodeBlockRenders)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("```\ncode\n```"))), "<pre>code</pre>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("```cpp\nint a = 1;\nint b = 2;\n```"))),
              "<pre>int a = 1;\nint b = 2;</pre>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("```\n**not bold** <script>\n```"))),
              "<pre>**not bold** &lt;script&gt;</pre>");
}

TEST(AIMarkdownRendererTest, HeadingsRender)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("# Title"))), "<h2>Title</h2>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("## Title"))), "<h3>Title</h3>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("### Title"))), "<h4>Title</h4>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("#### Title"))), "#### Title");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("#NoSpace"))), "#NoSpace");
}

TEST(AIMarkdownRendererTest, UnorderedListRenders)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("- a\n- b"))),
              "<ul><li>a</li><li>b</li></ul>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("* a\n* b"))),
              "<ul><li>a</li><li>b</li></ul>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("text\n- item"))),
              "text<ul><li>item</li></ul>");
}

TEST(AIMarkdownRendererTest, OrderedListRenders)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("1. a\n2. b"))),
              "<ol><li>a</li><li>b</li></ol>");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("1. **x**"))),
              "<ol><li><b>x</b></li></ol>");
}

namespace {

const char kTableOpen[] =
    "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" "
    "style=\"border-collapse:collapse;border-color:#606060;"
    "border-style:solid\">";

}

TEST(AIMarkdownRendererTest, TableRenders)
{
    EXPECT_EQ(toStd(renderToHtml(
                  QStringLiteral("| a | b |\n|---|---|\n| c | d |"),
                  QColor(QStringLiteral("#606060")))),
              std::string(kTableOpen)
                  + "<tr><th>a</th><th>b</th></tr>"
                    "<tr><td>c</td><td>d</td></tr></table>");
}

TEST(AIMarkdownRendererTest, TableBorderConsumesProvidedThemeColor)
{
    const QString out = renderToHtml(
        QStringLiteral("| a |\n|---|\n| b |"), QColor(QStringLiteral("#d97706")));

    EXPECT_TRUE(out.contains(QStringLiteral("border-color:#d97706;")));
    EXPECT_FALSE(out.contains(QStringLiteral("#606060")));
}

TEST(AIMarkdownRendererTest, TableSeparatorAcceptsAlignmentColons)
{
    const QString out = renderToHtml(
        QStringLiteral("| a | b | c |\n|:---|:--:|---:|\n| 1 | 2 | 3 |"));
    EXPECT_TRUE(out.contains(QStringLiteral("<th>a</th>")));
    EXPECT_TRUE(out.contains(QStringLiteral("<td>3</td>")));
}

TEST(AIMarkdownRendererTest, TableCellsRenderInlineMarkdownAndEscape)
{
    const QString out = renderToHtml(
        QStringLiteral("| h |\n|---|\n| **x** `y` <b>z</b> |"));
    EXPECT_TRUE(out.contains(
        QStringLiteral("<td><b>x</b> <code>y</code> &lt;b&gt;z&lt;/b&gt;</td>")));
}

TEST(AIMarkdownRendererTest, TableRowsPadAndTruncateToHeaderWidth)
{
    const QString out = renderToHtml(
        QStringLiteral("| a | b |\n|---|---|\n| 1 |\n| 2 | 3 | 4 |"));
    EXPECT_TRUE(out.contains(QStringLiteral("<tr><td>1</td><td></td></tr>")));
    EXPECT_TRUE(out.contains(QStringLiteral("<tr><td>2</td><td>3</td></tr>")));
    EXPECT_FALSE(out.contains(QStringLiteral("<td>4</td>")));
}

TEST(AIMarkdownRendererTest, TableEndsAtFirstNonRowLine)
{
    const QString out = renderToHtml(
        QStringLiteral("| a |\n|---|\n| 1 |\ntail"));
    EXPECT_TRUE(out.endsWith(QStringLiteral("</table>tail")));
}

TEST(AIMarkdownRendererTest, PipeLinesWithoutSeparatorStayLiteral)
{
    const QString out = renderToHtml(QStringLiteral("| a | b |\n| c | d |"));
    EXPECT_FALSE(out.contains(QStringLiteral("<table")));
    EXPECT_EQ(toStd(out), "| a | b |<br/>| c | d |");

    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("a | b"))), "a | b");
    EXPECT_FALSE(renderToHtml(QStringLiteral("| a |\n| :: |"))
                     .contains(QStringLiteral("<table")));
}

TEST(AIMarkdownRendererTest, TableSeparatorColumnCountMustMatchHeader)
{
    const QString out = renderToHtml(QStringLiteral("| a | b |\n|---|\n| c | d |"));
    EXPECT_FALSE(out.contains(QStringLiteral("<table")));
}

TEST(AIMarkdownRendererTest, PathologicalPipeSpamStaysBounded)
{
    QString pipes;
    for (int i = 0; i < 5000; ++i)
        pipes += QStringLiteral("||\n|-|\n");
    const QString out = renderToHtml(pipes);
    EXPECT_LE(out.size(), pipes.size() * 12 + 256);
}

TEST(AIMarkdownRendererTest, ParagraphLineBreaks)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("a\nb"))), "a<br/>b");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("a\n\nb"))), "a<br/><br/>b");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("a\r\nb"))), "a<br/>b");
}

TEST(AIMarkdownRendererTest, ScriptTagIsEscapedNeverLive)
{
    const QString out = renderToHtml(QStringLiteral("<script>alert(1)</script>"));
    EXPECT_TRUE(out.contains(QStringLiteral("&lt;script&gt;")));
    EXPECT_FALSE(out.contains(QStringLiteral("<script")));
    EXPECT_EQ(toStd(out), "&lt;script&gt;alert(1)&lt;/script&gt;");
}

TEST(AIMarkdownRendererTest, ImgAndAnchorInjectionEscaped)
{
    const QString img = renderToHtml(QStringLiteral("<img src=x onerror=alert(1)>"));
    EXPECT_TRUE(img.contains(QStringLiteral("&lt;img")));
    EXPECT_FALSE(img.contains(QStringLiteral("<img")));

    const QString anchor = renderToHtml(
        QStringLiteral("<a href=\"javascript:alert(1)\">click</a>"));
    EXPECT_TRUE(anchor.contains(QStringLiteral("&lt;a href=&quot;javascript:")));
    EXPECT_FALSE(anchor.contains(QStringLiteral("<a ")));
}

TEST(AIMarkdownRendererTest, RawAmpersandsAndQuotesEscaped)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("a & b < c > d \" e ' f"))),
              "a &amp; b &lt; c &gt; d &quot; e &#39; f");
}

TEST(AIMarkdownRendererTest, MarkdownWrappedInjectionRendersBoldWithEscapedInner)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("**<b>x</b>**"))),
              "<b>&lt;b&gt;x&lt;/b&gt;</b>");
}

TEST(AIMarkdownRendererTest, UnbalancedMarkersStayLiteral)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("**x"))), "**x");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("*x"))), "*x");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("`x"))), "`x");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("x**"))), "x**");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("****"))), "****");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("``"))), "``");

    const QString fence = renderToHtml(QStringLiteral("```\ncode without close"));
    EXPECT_EQ(toStd(fence), "```<br/>code without close");
    EXPECT_FALSE(fence.contains(QStringLiteral("<pre>")));
}

TEST(AIMarkdownRendererTest, IntrawordUnderscoresStayLiteral)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("AI_CONFIRMATION_EXPIRED"))),
              "AI_CONFIRMATION_EXPIRED");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("snake_case_name"))),
              "snake_case_name");
}

TEST(AIMarkdownRendererTest, SpaceAdjacentMarkersStayLiteral)
{
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("3 * 4 * 5"))), "3 * 4 * 5");
    EXPECT_EQ(toStd(renderToHtml(QStringLiteral("** not bold **"))), "** not bold **");
}

TEST(AIMarkdownRendererTest, PathologicalMarkerInputStaysBoundedAndFast)
{
    QString alternating;
    for (int i = 0; i < 10000; ++i)
        alternating += QStringLiteral("*a");
    const QString out1 = renderToHtml(alternating);
    EXPECT_LE(out1.size(), alternating.size() * 12 + 256);

    QString unmatched;
    for (int i = 0; i < 10000; ++i)
        unmatched += QStringLiteral("_a ");
    const QString out2 = renderToHtml(unmatched);
    EXPECT_LE(out2.size(), unmatched.size() * 12 + 256);
    EXPECT_FALSE(out2.contains(QStringLiteral("<i>")));

    QString stars(40000, QLatin1Char('*'));
    const QString out3 = renderToHtml(stars);
    EXPECT_LE(out3.size(), stars.size() * 12 + 256);
}

TEST(AIMarkdownRendererTest, PathologicalUnclosedFenceLinesStayBoundedAndFast)
{
    QString openers;
    for (int i = 0; i < 5000; ++i)
        openers += QStringLiteral("```x\n");
    const QString out = renderToHtml(openers);
    EXPECT_LE(out.size(), openers.size() * 12 + 256);
    EXPECT_FALSE(out.contains(QStringLiteral("<pre>")));
    EXPECT_TRUE(out.startsWith(QStringLiteral("```x<br/>```x")));

    // Openers before the only closed pair still render one code block.
    const QString mixed = renderToHtml(QStringLiteral("```x\n```\ntail"));
    EXPECT_EQ(toStd(mixed), "<pre></pre>tail");
}

TEST(AIMarkdownRendererTest, NoHangingOpenTagsForDegenerateInput)
{
    const QString out = renderToHtml(QStringLiteral("**a\n*b\n`c\n```"));
    EXPECT_FALSE(out.contains(QStringLiteral("<b>")));
    EXPECT_FALSE(out.contains(QStringLiteral("<i>")));
    EXPECT_FALSE(out.contains(QStringLiteral("<code>")));
    EXPECT_FALSE(out.contains(QStringLiteral("<pre>")));
}
