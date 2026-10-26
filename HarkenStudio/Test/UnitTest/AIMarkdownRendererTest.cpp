#include "AI/AIMarkdownRenderer.h"

#include <gtest/gtest.h>

#include <QString>

using VNX::AIMarkdownRenderer::renderToHtml;

TEST(AIMarkdownRendererTest, ScriptTagIsEscapedNeverLive)
{
    const QString out = renderToHtml(QStringLiteral("<script>alert(1)</script>"));
    EXPECT_FALSE(out.contains(QStringLiteral("<script")));
}
