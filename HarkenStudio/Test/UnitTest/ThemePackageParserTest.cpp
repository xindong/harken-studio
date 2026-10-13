#include "Core/ThemePackageParser.h"

#include <gtest/gtest.h>

namespace {

const QByteArray kValidManifest = R"json({
    "schemaVersion": 1,
    "id": "com.harken.test-theme",
    "displayName": "Test Theme",
    "version": "1.2.0",
    "colorScheme": "light",
    "baseThemeId": "harken.builtin.dark",
    "compatibility": {
        "harkenVersion": ">=1.0.0 <2.0.0",
        "capabilities": ["curve-v1", "meter-v1", "waveform-v1"]
    },
    "entries": {
        "tokens": "tokens.json",
        "visuals": {
            "curve": "visuals/curve.json",
            "meter": "visuals/meter.json",
            "waveform": "visuals/waveform.json"
        },
        "assetMap": "assets.json"
    },
    "previewAsset": "theme.preview"
})json";

}

TEST(ThemePackageParser, ParsesManifestDto)
{
    const auto result = VNX::parseThemeManifest(kValidManifest);

    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value.has_value());
    EXPECT_EQ(result.value->schemaVersion, 1);
    EXPECT_EQ(result.value->id, QStringLiteral("com.harken.test-theme"));
    EXPECT_EQ(result.value->displayName, QStringLiteral("Test Theme"));
    EXPECT_EQ(result.value->version, QStringLiteral("1.2.0"));
    EXPECT_EQ(result.value->colorScheme,
              std::optional<QString>(QStringLiteral("light")));
    EXPECT_EQ(result.value->baseThemeId,
              std::optional<QString>(QStringLiteral("harken.builtin.dark")));
    EXPECT_EQ(result.value->compatibility.harkenVersionRange,
              std::optional<QString>(QStringLiteral(">=1.0.0 <2.0.0")));
    EXPECT_EQ(result.value->compatibility.capabilities.size(), 3);
    EXPECT_EQ(result.value->entries.tokens, QStringLiteral("tokens.json"));
    EXPECT_EQ(result.value->entries.visuals.curve,
              QStringLiteral("visuals/curve.json"));
    EXPECT_EQ(result.value->entries.visuals.meter,
              QStringLiteral("visuals/meter.json"));
    EXPECT_EQ(result.value->entries.visuals.waveform,
              QStringLiteral("visuals/waveform.json"));
    EXPECT_EQ(result.value->entries.assetMap, QStringLiteral("assets.json"));
    EXPECT_EQ(result.value->previewAssetId,
              std::optional<QString>(QStringLiteral("theme.preview")));
}

TEST(ThemePackageParser, RejectsInvalidColorScheme)
{
    QByteArray manifest = kValidManifest;
    manifest.replace(R"json("colorScheme": "light")json",
                     R"json("colorScheme": "automatic")json");

    const auto result = VNX::parseThemeManifest(manifest);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::InvalidValue);
    EXPECT_EQ(result.diagnostics.front().path, QStringLiteral("$.colorScheme"));
}

TEST(ThemePackageParser, ReportsMissingManifestFieldWithStableLocation)
{
    QByteArray manifest = kValidManifest;
    manifest.replace(R"json("displayName": "Test Theme",)json", "");

    const auto result = VNX::parseThemeManifest(manifest);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::MissingField);
    EXPECT_EQ(VNX::themePackageErrorCodeName(result.diagnostics.front().code),
              QStringLiteral("theme.schema.missing_field"));
    EXPECT_EQ(result.diagnostics.front().file, QStringLiteral("manifest.json"));
    EXPECT_EQ(result.diagnostics.front().path, QStringLiteral("$.displayName"));
}

TEST(ThemePackageParser, RejectsUnsupportedSchemaVersion)
{
    QByteArray manifest = kValidManifest;
    manifest.replace(R"json("schemaVersion": 1)json",
                     R"json("schemaVersion": 6, "futureField": true)json");

    const auto result = VNX::parseThemeManifest(manifest);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::UnsupportedSchemaVersion);
    EXPECT_EQ(result.diagnostics.front().path, QStringLiteral("$.schemaVersion"));
}

TEST(ThemePackageParser, RejectsUnknownFields)
{
    QByteArray manifest = kValidManifest;
    manifest.replace(R"json("displayName": "Test Theme",)json",
                     R"json("displayName": "Test Theme", "qss": "* {}",)json");

    const auto result = VNX::parseThemeManifest(manifest);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::UnknownField);
    EXPECT_EQ(result.diagnostics.front().path, QStringLiteral("$.qss"));
}

TEST(ThemePackageParser, ExposesVersionAndEvolutionPolicies)
{
    EXPECT_EQ(VNX::ThemePackageContract::minimumSchemaVersion, 1);
    EXPECT_EQ(VNX::ThemePackageContract::maximumSchemaVersion, 5);
    EXPECT_EQ(VNX::ThemePackageContract::unknownFieldPolicy,
              VNX::ThemeUnknownFieldPolicy::Reject);
    EXPECT_EQ(VNX::ThemePackageContract::deprecatedFieldPolicy,
              VNX::ThemeDeprecatedFieldPolicy::Reject);
    EXPECT_EQ(VNX::ThemePackageContract::arrayInheritancePolicy,
              VNX::ThemeArrayInheritancePolicy::Replace);
}

TEST(ThemePackageParser, ParsesTokenAndVisualDtos)
{
    const auto tokens = VNX::parseThemeTokens(R"json({
        "colors": {
            "accent.primary": "#ff00f0ff",
            "text.primary": "#ffe0e0e0"
        },
        "metrics": {
            "radius.small": 2,
            "spacing.medium": 8
        },
        "fonts": {
            "body": {"family": "Inter", "pointSize": 11, "weight": 400},
            "mono": {"family": "JetBrains Mono"}
        }
    })json");
    const auto visual = VNX::parseThemeVisual(R"json({
        "colors": {"curve.line": "#ff00f0ff"},
        "metrics": {"curve.lineWidth": 1.5}
    })json", QStringLiteral("visuals/curve.json"));

    ASSERT_TRUE(tokens.ok());
    EXPECT_EQ(tokens.value->colors.value(QStringLiteral("accent.primary")),
              QStringLiteral("#ff00f0ff"));
    EXPECT_DOUBLE_EQ(tokens.value->metrics.value(QStringLiteral("spacing.medium")),
                     8.0);
    ASSERT_TRUE(tokens.value->fonts.contains(QStringLiteral("body")));
    EXPECT_EQ(tokens.value->fonts.value(QStringLiteral("body")).family,
              std::optional<QString>(QStringLiteral("Inter")));
    EXPECT_EQ(tokens.value->fonts.value(QStringLiteral("body")).pointSize,
              std::optional<int>(11));
    EXPECT_EQ(tokens.value->fonts.value(QStringLiteral("body")).weight,
              std::optional<int>(400));
    ASSERT_TRUE(visual.ok());
    EXPECT_EQ(visual.value->colors.value(QStringLiteral("curve.line")),
              QStringLiteral("#ff00f0ff"));
    EXPECT_DOUBLE_EQ(
        visual.value->metrics.value(QStringLiteral("curve.lineWidth")), 1.5);
}

TEST(ThemePackageParser, ReportsSemanticValueTypeErrors)
{
    const auto result = VNX::parseThemeTokens(
        R"json({"colors": {"accent.primary": 42}})json");

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::FieldTypeMismatch);
    EXPECT_EQ(result.diagnostics.front().path,
              QStringLiteral("$.colors.accent.primary"));
}

TEST(ThemePackageParser, ParsesPresenterContextsWithoutClassNames)
{
    const auto result = VNX::parseThemePresenters(R"json({
        "contexts": {
            "inspector.float": "slider-with-value",
            "effect.parameter": "rotary-knob"
        }
    })json");

    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value.has_value());
    EXPECT_EQ(result.value->contexts.value(QStringLiteral("inspector.float")),
              QStringLiteral("slider-with-value"));
    EXPECT_EQ(result.value->contexts.value(QStringLiteral("effect.parameter")),
              QStringLiteral("rotary-knob"));
}

TEST(ThemePackageParser, PresenterEntryRequiresSchemaVersionTwo)
{
    QByteArray manifest = kValidManifest;
    manifest.replace(R"json("assetMap": "assets.json")json",
                     R"json("assetMap": "assets.json", "presenters": "presenters.json")json");

    auto result = VNX::parseThemeManifest(manifest);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::InvalidValue);
    EXPECT_EQ(result.diagnostics.front().path,
              QStringLiteral("$.entries.presenters"));

    manifest.replace(R"json("schemaVersion": 1)json",
                     R"json("schemaVersion": 2)json");
    result = VNX::parseThemeManifest(manifest);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->entries.presenters,
              std::optional<QString>(QStringLiteral("presenters.json")));
}

TEST(ThemePackageParser, DomainVisualEntriesRequireSchemaVersionFour)
{
    QByteArray manifest = kValidManifest;
    manifest.replace(
        R"json("waveform": "visuals/waveform.json")json",
        R"json("waveform": "visuals/waveform.json", "nodeGraph": "visuals/nodeGraph.json", "timeline": "visuals/timeline.json")json");

    auto result = VNX::parseThemeManifest(manifest);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::InvalidValue);
    EXPECT_EQ(result.diagnostics.front().path,
              QStringLiteral("$.entries.visuals"));

    manifest.replace(R"json("schemaVersion": 1)json",
                     R"json("schemaVersion": 4)json");
    result = VNX::parseThemeManifest(manifest);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->entries.visuals.nodeGraph,
              std::optional<QString>(QStringLiteral("visuals/nodeGraph.json")));
    EXPECT_EQ(result.value->entries.visuals.timeline,
              std::optional<QString>(QStringLiteral("visuals/timeline.json")));
}

TEST(ThemePackageParser, MonitorVisualEntriesRequireSchemaVersionFive)
{
    QByteArray manifest = kValidManifest;
    manifest.replace(
        R"json("waveform": "visuals/waveform.json")json",
        R"json("waveform": "visuals/waveform.json", "monitor": "visuals/monitor.json")json");
    manifest.replace(R"json("schemaVersion": 1)json",
                     R"json("schemaVersion": 4)json");

    auto result = VNX::parseThemeManifest(manifest);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::InvalidValue);
    EXPECT_EQ(result.diagnostics.front().path,
              QStringLiteral("$.entries.visuals.monitor"));

    manifest.replace(R"json("schemaVersion": 4)json",
                     R"json("schemaVersion": 5)json");
    result = VNX::parseThemeManifest(manifest);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->entries.visuals.monitor,
              std::optional<QString>(QStringLiteral("visuals/monitor.json")));
}

TEST(ThemePackageParser, RejectsLegacyProfilerVisualKey)
{
    QByteArray manifest = kValidManifest;
    manifest.replace(R"json("schemaVersion": 1)json",
                     R"json("schemaVersion": 5)json");
    manifest.replace(
        R"json("waveform": "visuals/waveform.json")json",
        R"json("waveform": "visuals/waveform.json", "profiler": "visuals/profiler.json")json");

    const auto result = VNX::parseThemeManifest(manifest);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::UnknownField);
    EXPECT_EQ(result.diagnostics.front().path,
              QStringLiteral("$.entries.visuals.profiler"));
}
TEST(ThemePackageParser, RejectsArbitraryPresenterDocumentFields)
{
    const auto result = VNX::parseThemePresenters(R"json({
        "contexts": {"effect.parameter": "rotary-knob"},
        "className": "CustomKnob"
    })json");

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::UnknownField);
    EXPECT_EQ(result.diagnostics.front().path,
              QStringLiteral("$.className"));
}

TEST(ThemePackageParser, ParsesConstrainedComponentRecipes)
{
    const auto result = VNX::parseThemeRecipes(R"json({
        "recipes": [{
            "host": "track.header",
            "breakpoint": "compact",
            "variant": "track.compact",
            "slots": [
                {"id": "identity", "preferred": 64, "alignment": "center"},
                {"id": "volume", "presenter": "rotary-knob"},
                {"id": "meter", "visible": false, "asset": "track.meter.bg"}
            ]
        }]
    })json");

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value->recipes.size(), 1);
    const auto& recipe = result.value->recipes.front();
    EXPECT_EQ(recipe.host, QStringLiteral("track.header"));
    EXPECT_EQ(recipe.breakpoint, QStringLiteral("compact"));
    EXPECT_EQ(recipe.variant, QStringLiteral("track.compact"));
    ASSERT_EQ(recipe.items.size(), 3);
    EXPECT_EQ(recipe.items.at(0).preferred, std::optional<int>(64));
    EXPECT_EQ(recipe.items.at(0).alignment, QStringLiteral("center"));
    EXPECT_EQ(recipe.items.at(1).presenter,
              std::optional<QString>(QStringLiteral("rotary-knob")));
    EXPECT_FALSE(recipe.items.at(2).visible);
    EXPECT_EQ(recipe.items.at(2).asset,
              std::optional<QString>(QStringLiteral("track.meter.bg")));
}

TEST(ThemePackageParser, RecipeEntryRequiresSchemaVersionThree)
{
    QByteArray manifest = kValidManifest;
    manifest.replace(R"json("assetMap": "assets.json")json",
                     R"json("assetMap": "assets.json", "recipes": "recipes.json")json");
    manifest.replace(R"json("schemaVersion": 1)json",
                     R"json("schemaVersion": 2)json");

    auto result = VNX::parseThemeManifest(manifest);

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::InvalidValue);
    EXPECT_EQ(result.diagnostics.front().path,
              QStringLiteral("$.entries.recipes"));

    manifest.replace(R"json("schemaVersion": 2)json",
                     R"json("schemaVersion": 3)json");
    result = VNX::parseThemeManifest(manifest);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->entries.recipes,
              std::optional<QString>(QStringLiteral("recipes.json")));
}

TEST(ThemePackageParser, RejectsArbitraryRecipeFields)
{
    const auto result = VNX::parseThemeRecipes(R"json({
        "recipes": [{
            "host": "track.header",
            "variant": "track.compact",
            "className": "CustomTrackHeader",
            "slots": [{"id": "identity"}]
        }]
    })json");

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::UnknownField);
    EXPECT_EQ(result.diagnostics.front().path,
              QStringLiteral("$.recipes[0].className"));
}

TEST(ThemePackageParser, RejectsRecipeSizeOutsideIntegerRange)
{
    const auto result = VNX::parseThemeRecipes(R"json({
        "recipes": [{
            "host": "track.header",
            "variant": "track.compact",
            "slots": [{"id": "identity", "preferred": 1e30}]
        }]
    })json");

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::FieldTypeMismatch);
    EXPECT_EQ(result.diagnostics.front().path,
              QStringLiteral("$.recipes[0].slots[0].preferred"));
}

TEST(ThemePackageParser, ParsesLogicalAssetMap)
{
    const auto result = VNX::parseThemeAssetMap(R"json({
        "assets": {
            "theme.preview": {
                "type": "image",
                "sources": [
                    {"path": "preview.png"},
                    {"path": "assets/preview@2x.png", "scale": 2}
                ]
            },
            "ui.body": {
                "type": "font",
                "sources": [{"path": "assets/fonts/body.ttf"}]
            }
        }
    })json");

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value->assets.size(), 2);
    const auto preview = result.value->assets.value(QStringLiteral("theme.preview"));
    EXPECT_EQ(preview.kind, VNX::ThemeAssetKind::Image);
    ASSERT_EQ(preview.sources.size(), 2);
    EXPECT_EQ(preview.sources.at(0).path, QStringLiteral("preview.png"));
    EXPECT_DOUBLE_EQ(preview.sources.at(0).scale, 1.0);
    EXPECT_DOUBLE_EQ(preview.sources.at(1).scale, 2.0);
    EXPECT_EQ(result.value->assets.value(QStringLiteral("ui.body")).kind,
              VNX::ThemeAssetKind::Font);
}
