#include "Core/ThemePackageLoader.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

struct PackageContent
{
    QByteArray tokens = R"json({"colors":{"accent.primary":"#ff00f0ff"},"metrics":{"spacing.medium":12}})json";
    QByteArray curve = R"json({"colors":{"curve.line":"#ff112233"},"metrics":{"curve.lineWidth":2}})json";
    QByteArray meter = R"json({"colors":{"meter.green":"#ff22aa44","meter.spectrogram.grid":"#ff102030"},"metrics":{"meter.spectrogram.plotInsetLeft":42}})json";
    QByteArray waveform = R"json({"colors":{"waveform.line":"#ff334455"}})json";
    QByteArray assets = R"json({"assets":{}})json";
    std::optional<QByteArray> presenters;
    std::optional<QByteArray> recipes;
    QMap<QString, QByteArray> domainVisuals;
    std::optional<QString> colorScheme;
    int schemaVersion = 1;
    QString harkenRange = QStringLiteral(">=0.1.0 <1.0.0");
    QStringList capabilities{
        QStringLiteral("curve-v1"),
        QStringLiteral("meter-v1"),
        QStringLiteral("waveform-v1")};
};

bool writePackage(const QString& themesRoot,
                  const QString& directoryName,
                  const QString& id,
                  const std::optional<QString>& baseThemeId = std::nullopt,
                  const PackageContent& content = {})
{
    const QString packageRoot = QDir(themesRoot).filePath(directoryName);
    QDir().mkpath(QDir(packageRoot).filePath(QStringLiteral("visuals")));
    QByteArray capabilities;
    for (qsizetype index = 0; index < content.capabilities.size(); ++index)
    {
        if (index > 0)
            capabilities += ',';
        capabilities += '"' + content.capabilities.at(index).toUtf8() + '"';
    }
    const QByteArray base = baseThemeId
        ? QByteArray(",\"baseThemeId\":\"") + baseThemeId->toUtf8() + '"'
        : QByteArray();
    const QByteArray colorScheme = content.colorScheme
        ? QByteArray(",\"colorScheme\":\"") + content.colorScheme->toUtf8() + '"'
        : QByteArray();
    const QByteArray presenterEntry = content.presenters
        ? QByteArray(",\"presenters\":\"presenters.json\"")
        : QByteArray();
    const QByteArray recipeEntry = content.recipes
        ? QByteArray(",\"recipes\":\"recipes.json\"")
        : QByteArray();
    QByteArray domainVisualEntries;
    for (auto it = content.domainVisuals.constBegin();
         it != content.domainVisuals.constEnd(); ++it)
    {
        domainVisualEntries += QByteArray(",\"") + it.key().toUtf8()
            + "\":\"visuals/" + it.key().toUtf8() + ".json\"";
    }
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":" + QByteArray::number(content.schemaVersion)
        + ",\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"Test "+ id.toUtf8()
        + "\",\"version\":\"1.0.0\"" + colorScheme + base
        + ",\"compatibility\":{\"harkenVersion\":\""
        + content.harkenRange.toUtf8() + "\",\"capabilities\":["
        + capabilities
        + "]},\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\","
          "\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\""
        + domainVisualEntries + "},"
          "\"assetMap\":\"assets.json\""
        + presenterEntry + recipeEntry + "}}";
    bool domainVisualsWritten = true;
    for (auto it = content.domainVisuals.constBegin();
         it != content.domainVisuals.constEnd(); ++it)
    {
        domainVisualsWritten = domainVisualsWritten
            && writeFile(
                QDir(packageRoot).filePath(QStringLiteral("visuals/") + it.key()
                                            + QStringLiteral(".json")),
                it.value());
    }
    return writeFile(QDir(packageRoot).filePath(QStringLiteral("manifest.json")),
                     manifest)
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("tokens.json")),
                     content.tokens)
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/curve.json")), content.curve)
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/meter.json")), content.meter)
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/waveform.json")), content.waveform)
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("assets.json")),
                     content.assets)
        && domainVisualsWritten
        && (!content.presenters
            || writeFile(QDir(packageRoot).filePath(
                             QStringLiteral("presenters.json")),
                         *content.presenters))
        && (!content.recipes
            || writeFile(QDir(packageRoot).filePath(
                             QStringLiteral("recipes.json")),
                         *content.recipes));
}

const VNX::ThemePackageDiagnostic& onlyDiagnostic(
    const VNX::ThemePackageLoadResult& result)
{
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.candidate.has_value());
    EXPECT_EQ(result.diagnostics.size(), 1);
    return result.diagnostics.front();
}

}

TEST(ThemePackageLoader, InheritsAndOverridesColorScheme)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());

    PackageContent light;
    light.colorScheme = QStringLiteral("light");
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("light"),
                             QStringLiteral("theme.light"), std::nullopt, light));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("inherited"),
                             QStringLiteral("theme.inherited"),
                             QStringLiteral("theme.light")));
    PackageContent dark;
    dark.colorScheme = QStringLiteral("dark");
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("dark"),
                             QStringLiteral("theme.dark"),
                             QStringLiteral("theme.light"), dark));

    const auto inherited = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.inherited"));
    const auto overridden = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.dark"));

    ASSERT_TRUE(inherited.ok());
    ASSERT_TRUE(overridden.ok());
    EXPECT_EQ(inherited.candidate->snapshot->colorScheme,
              VNX::ThemeColorScheme::Light);
    EXPECT_EQ(overridden.candidate->snapshot->colorScheme,
              VNX::ThemeColorScheme::Dark);
}

TEST(ThemePackageLoader, BuildsCandidateWithoutPublishingRuntimeState)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    PackageContent content;
    content.assets = R"json({
        "assets": {
            "theme.preview": {
                "type": "image",
                "sources": [{"path": "assets/preview.png"}]
            }
        }
    })json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("test"),
                             QStringLiteral("com.harken.test"), std::nullopt,
                             content));

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("com.harken.test"));

    ASSERT_TRUE(result.ok())
        << result.diagnostics.value(0).file.toStdString() << " "
        << result.diagnostics.value(0).path.toStdString() << " "
        << result.diagnostics.value(0).message.toStdString();
    ASSERT_TRUE(result.candidate->snapshot);
    EXPECT_EQ(result.candidate->inheritanceOrder,
              QStringList({QStringLiteral("harken.builtin.dark"),
                           QStringLiteral("com.harken.test")}));
    EXPECT_EQ(result.candidate->snapshot->ui.colors.accent,
              QColor(QStringLiteral("#ff00f0ff")));
    EXPECT_EQ(result.candidate->snapshot->ui.metrics.spacingMedium, 12);
    EXPECT_EQ(result.candidate->snapshot->domains.curve.colors.attenuationVolume,
              QColor(QStringLiteral("#ff112233")));
    EXPECT_DOUBLE_EQ(result.candidate->snapshot->domains.curve.metrics.curveLineWidth,
                     2.0);
    EXPECT_EQ(result.candidate->snapshot->domains.meter.spectrogramMeter.colors.grid,
              QColor(QStringLiteral("#ff102030")));
    EXPECT_EQ(result.candidate->snapshot->domains.meter.spectrogramMeter.metrics.plotInsetLeft,
              42);
    ASSERT_EQ(result.candidate->assets.assets.size(), 1);
    EXPECT_EQ(result.candidate->assets.assets
                  .value(QStringLiteral("theme.preview"))
                  .sources.front()
                  .path,
              QStringLiteral("com.harken.test/assets/preview.png"));
}

TEST(ThemePackageLoader, MergesMultipleInheritanceLayersDeterministically)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    PackageContent base;
    base.tokens = R"json({"colors":{"text.primary":"#ff101010","accent.primary":"#ff111111"}})json";
    base.curve = R"json({"colors":{"curve.grid":"#ff222222","curve.line":"#ff333333"}})json";
    base.assets = R"json({"assets":{"shared":{"type":"image","sources":[{"path":"base.png"}]},"base-only":{"type":"font","sources":[{"path":"base.ttf"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("base"),
                             QStringLiteral("theme.base"), std::nullopt, base));
    PackageContent child;
    child.tokens = R"json({"colors":{"accent.primary":"#ffabcdef"}})json";
    child.curve = R"json({"colors":{"curve.line":"#ff778899"}})json";
    child.assets = R"json({"assets":{"shared":{"type":"image","sources":[{"path":"child.png"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("child"),
                             QStringLiteral("theme.child"),
                             QStringLiteral("theme.base"), child));

    const auto first = VNX::loadThemePackageCandidate(root.path(),
                                                       QStringLiteral("theme.child"));
    const auto second = VNX::loadThemePackageCandidate(root.path(),
                                                        QStringLiteral("theme.child"));

    ASSERT_TRUE(first.ok())
        << first.diagnostics.value(0).file.toStdString() << " "
        << first.diagnostics.value(0).path.toStdString() << " "
        << first.diagnostics.value(0).message.toStdString();
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.candidate->inheritanceOrder,
              QStringList({QStringLiteral("harken.builtin.dark"),
                           QStringLiteral("theme.base"),
                           QStringLiteral("theme.child")}));
    EXPECT_EQ(first.candidate->snapshot->ui.colors.textPrimary,
              QColor(QStringLiteral("#ff101010")));
    EXPECT_EQ(first.candidate->snapshot->ui.colors.accent,
              QColor(QStringLiteral("#ffabcdef")));
    EXPECT_EQ(first.candidate->snapshot->domains.curve.colors.grid,
              QColor(QStringLiteral("#ff222222")));
    EXPECT_EQ(first.candidate->snapshot->domains.curve.colors.attenuationVolume,
              QColor(QStringLiteral("#ff778899")));
    EXPECT_EQ(first.candidate->assets.assets
                  .value(QStringLiteral("shared"))
                  .sources.front()
                  .path,
              QStringLiteral("theme.child/child.png"));
    EXPECT_TRUE(first.candidate->assets.assets.contains(
        QStringLiteral("base-only")));
    EXPECT_EQ(first.candidate->snapshot->ui.colors.accent,
              second.candidate->snapshot->ui.colors.accent);
}

TEST(ThemePackageLoader, RejectsMissingBaseTheme)
{
    QTemporaryDir root;
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("child"),
                             QStringLiteral("theme.child"),
                             QStringLiteral("theme.missing")));

    const auto result = VNX::loadThemePackageCandidate(root.path(),
                                                        QStringLiteral("theme.child"));

    const auto& error = onlyDiagnostic(result);
    EXPECT_EQ(error.code, VNX::ThemePackageErrorCode::MissingBaseTheme);
    EXPECT_EQ(error.path, QStringLiteral("$.baseThemeId"));
}

TEST(ThemePackageLoader, RejectsInheritanceCycle)
{
    QTemporaryDir root;
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("a"),
                             QStringLiteral("theme.a"),
                             QStringLiteral("theme.b")));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("b"),
                             QStringLiteral("theme.b"),
                             QStringLiteral("theme.a")));

    const auto result = VNX::loadThemePackageCandidate(root.path(),
                                                        QStringLiteral("theme.a"));

    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::InheritanceCycle);
}

TEST(ThemePackageLoader, EnforcesInheritanceDepth)
{
    QTemporaryDir root;
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("a"),
                             QStringLiteral("theme.a")));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("b"),
                             QStringLiteral("theme.b"),
                             QStringLiteral("theme.a")));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("c"),
                             QStringLiteral("theme.c"),
                             QStringLiteral("theme.b")));
    VNX::ThemePackageLoadOptions options;
    options.limits.maximumInheritanceDepth = 2;

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.c"), options);

    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::InheritanceDepthExceeded);
}

TEST(ThemePackageLoader, IgnoresMalformedSiblingWhenLoadingValidTheme)
{
    QTemporaryDir root;
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("valid"),
                             QStringLiteral("theme.valid")));
    ASSERT_TRUE(writeFile(
        QDir(root.path()).filePath(QStringLiteral("broken/manifest.json")),
        QByteArrayLiteral("{not-json")));

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.valid"));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.candidate->manifest.id, QStringLiteral("theme.valid"));
}

TEST(ThemePackageLoader, RejectsDuplicateThemeIds)
{
    QTemporaryDir root;
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("a"),
                             QStringLiteral("theme.same")));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("b"),
                             QStringLiteral("theme.same")));

    const auto result = VNX::loadThemePackageCandidate(root.path(),
                                                        QStringLiteral("theme.same"));

    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::DuplicateThemeId);
}

TEST(ThemePackageLoader, RejectsEntryAndAssetPathEscapes)
{
    QTemporaryDir root;
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("entry"),
                             QStringLiteral("theme.entry")));
    const QString manifest = QDir(root.path()).filePath(
        QStringLiteral("entry/manifest.json"));
    QFile manifestFile(manifest);
    ASSERT_TRUE(manifestFile.open(QIODevice::ReadOnly));
    QByteArray manifestBytes = manifestFile.readAll();
    manifestFile.close();
    manifestBytes.replace("\"tokens\":\"tokens.json\"",
                          "\"tokens\":\"../outside.json\"");
    ASSERT_TRUE(writeFile(manifest, manifestBytes));

    auto result = VNX::loadThemePackageCandidate(root.path(),
                                                  QStringLiteral("theme.entry"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::PathEscape);

    PackageContent asset;
    asset.assets = R"json({"assets":{"bad":{"type":"image","sources":[{"path":"../outside.png"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("asset"),
                             QStringLiteral("theme.asset"), std::nullopt, asset));
    result = VNX::loadThemePackageCandidate(root.path(),
                                             QStringLiteral("theme.asset"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::PathEscape);
}

TEST(ThemePackageLoader, RejectsSymbolicLinkEscapeWhenSupported)
{
    QTemporaryDir root;
    QTemporaryDir outside;
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("linked"),
                             QStringLiteral("theme.linked")));
    const QString outsideTokens = QDir(outside.path()).filePath(
        QStringLiteral("tokens.json"));
    ASSERT_TRUE(writeFile(outsideTokens,
                          R"json({"colors":{"accent.primary":"#ffffffff"}})json"));
    const QString linkedDirectory = QDir(root.path()).filePath(
        QStringLiteral("linked/escape"));
    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        std::filesystem::path(outside.path().toStdString()),
        std::filesystem::path(linkedDirectory.toStdString()), linkError);
    if (linkError)
        GTEST_SKIP() << "Symbolic links are unavailable in this environment.";
    const QString manifest = QDir(root.path()).filePath(
        QStringLiteral("linked/manifest.json"));
    QFile manifestFile(manifest);
    ASSERT_TRUE(manifestFile.open(QIODevice::ReadOnly));
    QByteArray manifestBytes = manifestFile.readAll();
    manifestFile.close();
    manifestBytes.replace("\"tokens\":\"tokens.json\"",
                          "\"tokens\":\"escape/tokens.json\"");
    ASSERT_TRUE(writeFile(manifest, manifestBytes));

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.linked"));
    std::error_code removeError;
    std::filesystem::remove(
        std::filesystem::path(linkedDirectory.toStdString()), removeError);

    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::PathEscape);
}

TEST(ThemePackageLoader, RejectsDuplicateJsonKeysBeforeSchemaParsing)
{
    QTemporaryDir root;
    PackageContent content;
    content.tokens = R"json({"colors":{"accent.primary":"#ff000000","accent.primary":"#ffffffff"}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("duplicate"),
                             QStringLiteral("theme.duplicate"), std::nullopt,
                             content));

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.duplicate"));

    const auto& error = onlyDiagnostic(result);
    EXPECT_EQ(error.code, VNX::ThemePackageErrorCode::DuplicateJsonKey);
    EXPECT_EQ(error.path, QStringLiteral("$.colors.accent.primary"));
}

TEST(ThemePackageLoader, AcceptsUtf8BomJsonDocuments)
{
    QTemporaryDir root;
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("bom"),
                             QStringLiteral("theme.bom")));
    const QString tokensPath = QDir(root.path()).filePath(
        QStringLiteral("bom/tokens.json"));
    QFile tokens(tokensPath);
    ASSERT_TRUE(tokens.open(QIODevice::ReadOnly));
    const QByteArray contents = tokens.readAll();
    tokens.close();
    ASSERT_TRUE(writeFile(tokensPath, QByteArray("\xEF\xBB\xBF") + contents));

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.bom"));

    EXPECT_TRUE(result.ok());
}

TEST(ThemePackageLoader, EnforcesReadAndStructureBudgets)
{
    QTemporaryDir root;
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("limited"),
                             QStringLiteral("theme.limited")));
    VNX::ThemePackageLoadOptions options;
    options.limits.maximumFileBytes = 32;

    auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.limited"), options);

    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::ResourceLimit);

    options.limits.maximumFileBytes = 1024 * 1024;
    options.limits.maximumJsonDepth = 2;
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.limited"), options);
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::ResourceLimit);
}

TEST(ThemePackageLoader, RejectsCompatibilityAndSemanticFailures)
{
    QTemporaryDir root;
    PackageContent incompatible;
    incompatible.capabilities.append(QStringLiteral("future-presenter-v9"));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("incompatible"),
                             QStringLiteral("theme.incompatible"), std::nullopt,
                             incompatible));
    auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.incompatible"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::UnsupportedCompatibility);

    PackageContent semantic;
    semantic.tokens = R"json({"colors":{"internal.widgetName":"#ff000000"}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("semantic"),
                             QStringLiteral("theme.semantic"), std::nullopt,
                             semantic));
    result = VNX::loadThemePackageCandidate(root.path(),
                                             QStringLiteral("theme.semantic"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::UnknownSemanticKey);

    PackageContent invalidColor;
    invalidColor.tokens = R"json({"colors":{"accent.primary":"not-a-color"}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("invalid-color"),
                             QStringLiteral("theme.invalid-color"), std::nullopt,
                             invalidColor));
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.invalid-color"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::InvalidColor);

    PackageContent invalidMetric;
    invalidMetric.curve = R"json({"metrics":{"curve.lineWidth":-1}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("invalid-metric"),
                             QStringLiteral("theme.invalid-metric"), std::nullopt,
                             invalidMetric));
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.invalid-metric"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::MetricOutOfRange);
}

TEST(ThemePackageLoader, LoadsAndInheritsDomainSemanticVisuals)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    PackageContent base;
    base.schemaVersion = 5;
    base.domainVisuals = {
        {QStringLiteral("nodeGraph"),
         R"json({"colors":{"nodeGraph.sound":"#ff010101","nodeGraph.node.bodyNormal":"#ff020202"}})json"},
        {QStringLiteral("blueprint"),
         R"json({"colors":{"blueprint.canvasBackground":"#ff030303"}})json"},
        {QStringLiteral("timeline"),
         R"json({"colors":{"timeline.clip":"#ff040404"}})json"},
        {QStringLiteral("music"),
         R"json({"colors":{"music.trackColor0":"#ff050505"}})json"},
        {QStringLiteral("log"),
         R"json({"colors":{"log.warning":"#ff060606"}})json"},
        {QStringLiteral("monitor"),
         R"json({"colors":{"monitor.statePlaying":"#ff080808"}})json"},
        {QStringLiteral("ai"),
         R"json({"colors":{"ai.brandAccent":"#ff090909"}})json"},
        {QStringLiteral("scene3d"),
         R"json({"colors":{"scene3d.listener":"#ff0a0a0a"}})json"},
        {QStringLiteral("spatial"),
         R"json({"colors":{"spatial.layerVolume":"#ff0b0b0b"}})json"},
        {QStringLiteral("feedback"),
         R"json({"colors":{"feedback.overlayHighlight":"#ff0c0c0c"}})json"}};
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("base"),
                             QStringLiteral("theme.domain-base"),
                             std::nullopt, base));
    PackageContent child;
    child.schemaVersion = 5;
    child.domainVisuals = {
        {QStringLiteral("nodeGraph"),
         R"json({"colors":{"nodeGraph.sound":"#ff111111"}})json"},
        {QStringLiteral("timeline"),
         R"json({"colors":{"timeline.clipSelected":"#ff121212"}})json"}};
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("child"),
                             QStringLiteral("theme.domain-child"),
                             QStringLiteral("theme.domain-base"), child));

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.domain-child"));

    ASSERT_TRUE(result.ok())
        << result.diagnostics.value(0).file.toStdString() << " "
        << result.diagnostics.value(0).path.toStdString() << " "
        << result.diagnostics.value(0).message.toStdString();
    const auto& domains = result.candidate->snapshot->domains;
    EXPECT_EQ(domains.nodeGraph.sound, QColor(QStringLiteral("#ff111111")));
    EXPECT_EQ(domains.nodeGraphNode.bodyNormal,
              QColor(QStringLiteral("#ff020202")));
    EXPECT_EQ(domains.blueprint.canvasBackground,
              QColor(QStringLiteral("#ff030303")));
    EXPECT_EQ(domains.timeline.clip, QColor(QStringLiteral("#ff040404")));
    EXPECT_EQ(domains.timeline.clipSelected,
              QColor(QStringLiteral("#ff121212")));
    EXPECT_EQ(domains.music.trackColors[0], QColor(QStringLiteral("#ff050505")));
    EXPECT_EQ(domains.log.warning, QColor(QStringLiteral("#ff060606")));
    EXPECT_EQ(domains.monitor.statePlaying,
              QColor(QStringLiteral("#ff080808")));
    EXPECT_EQ(domains.ai.brandAccent, QColor(QStringLiteral("#ff090909")));
    EXPECT_EQ(domains.scene3d.listener, QColor(QStringLiteral("#ff0a0a0a")));
    EXPECT_EQ(domains.spatial.layerVolume,
              QColor(QStringLiteral("#ff0b0b0b")));
    EXPECT_EQ(domains.feedback.overlayHighlight,
              QColor(QStringLiteral("#ff0c0c0c")));
}

TEST(ThemePackageLoader, RejectsUnknownDomainSemanticKeyAndMetrics)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    PackageContent content;
    content.schemaVersion = 5;
    content.domainVisuals = {
        {QStringLiteral("timeline"),
         R"json({"colors":{"timeline.internalWidget":"#ffffffff"}})json"}};
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("unknown"),
                             QStringLiteral("theme.domain-unknown"),
                             std::nullopt, content));

    auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.domain-unknown"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::UnknownSemanticKey);

    content.domainVisuals = {
        {QStringLiteral("timeline"),
         R"json({"colors":{"blueprint.canvasBackground":"#ffffffff"}})json"}};
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("foreign"),
                             QStringLiteral("theme.domain-foreign"),
                             std::nullopt, content));
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.domain-foreign"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::UnknownSemanticKey);

    content.domainVisuals = {
        {QStringLiteral("timeline"),
         R"json({"metrics":{"timeline.internalSize":12}})json"}};
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("metric"),
                             QStringLiteral("theme.domain-metric"),
                             std::nullopt, content));
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.domain-metric"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::UnknownSemanticKey);
}

TEST(ThemePackageLoader, RejectsLegacyProfilerSemanticColorKey)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    PackageContent content;
    content.schemaVersion = 5;
    content.domainVisuals = {
        {QStringLiteral("monitor"),
         R"json({"colors":{"profiler.statePlaying":"#ff080808"}})json"}};
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("legacy"),
                             QStringLiteral("theme.monitor-legacy-key"),
                             std::nullopt, content));

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.monitor-legacy-key"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::UnknownSemanticKey);
}
TEST(ThemePackageLoader, LoadsInheritedPresenterSelections)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    PackageContent base;
    base.schemaVersion = 2;
    base.presenters = R"json({"contexts":{
        "inspector.float":"rotary-knob",
        "effect.parameter":"slider-with-value"
    }})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("base"),
                             QStringLiteral("theme.presenter-base"),
                             std::nullopt, base));
    PackageContent child;
    child.schemaVersion = 2;
    child.presenters = R"json({"contexts":{
        "inspector.float":"horizontal-slider"
    }})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("child"),
                             QStringLiteral("theme.presenter-child"),
                             QStringLiteral("theme.presenter-base"), child));

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.presenter-child"));

    ASSERT_TRUE(result.ok())
        << result.diagnostics.value(0).message.toStdString();
    EXPECT_EQ(result.candidate->snapshot->ui.parameterPresenters.inspectorFloat,
              VNX::ParameterPresenterId::HorizontalSlider);
    EXPECT_EQ(result.candidate->snapshot->ui.parameterPresenters.effectParameter,
              VNX::ParameterPresenterId::SliderWithValue);
}

TEST(ThemePackageLoader, RejectsUnknownAndUnsupportedPresenterSelections)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    PackageContent content;
    content.schemaVersion = 2;
    content.presenters = R"json({"contexts":{
        "effect.parameter":"VNX::CustomPresenter"
    }})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("unknown"),
                             QStringLiteral("theme.presenter-unknown"),
                             std::nullopt, content));

    auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.presenter-unknown"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::InvalidValue);

    content.presenters = R"json({"contexts":{
        "effect.parameter":"vertical-fader"
    }})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("unsupported"),
                             QStringLiteral("theme.presenter-unsupported"),
                             std::nullopt, content));
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.presenter-unsupported"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::InvalidValue);
}

TEST(ThemePackageLoader, LoadsInheritedComponentRecipesByHostAndBreakpoint)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    PackageContent base;
    base.schemaVersion = 3;
    base.assets = R"json({"assets":{"track.identity.bg":{"type":"image","sources":[{"path":"identity.png"}]}}})json";
    base.recipes = R"json({"recipes":[
        {"host":"track.header","breakpoint":"wide","variant":"track.expanded","slots":[
            {"id":"identity","asset":"track.identity.bg"},
            {"id":"volume","presenter":"vertical-fader"},
            {"id":"meter"}
        ]},
        {"host":"track.header","variant":"track.expanded","slots":[
            {"id":"identity"},{"id":"controls"},{"id":"meter"}
        ]}
    ]})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("base"),
                             QStringLiteral("theme.recipe-base"),
                             std::nullopt, base));
    PackageContent child;
    child.schemaVersion = 3;
    child.recipes = R"json({"recipes":[
        {"host":"track.header","breakpoint":"wide","variant":"track.compact","slots":[
            {"id":"meter"},
            {"id":"volume","preferred":72,"presenter":"rotary-knob"},
            {"id":"identity","alignment":"center"}
        ]}
    ]})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("child"),
                             QStringLiteral("theme.recipe-child"),
                             QStringLiteral("theme.recipe-base"), child));

    const auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.recipe-child"));

    ASSERT_TRUE(result.ok())
        << result.diagnostics.value(0).message.toStdString();
    const auto* wide = result.candidate->snapshot->ui.recipes.recipeFor(
        VNX::ComponentRecipeHostId::TrackHeader,
        VNX::ComponentRecipeBreakpoint::Wide);
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->variant, VNX::ComponentRecipeVariant::TrackCompact);
    ASSERT_EQ(wide->items.size(), 3);
    EXPECT_EQ(wide->items.at(0).id, VNX::ComponentRecipeSlotId::Meter);
    EXPECT_EQ(wide->items.at(1).preferred, 72);
    EXPECT_EQ(wide->items.at(1).presenter,
              VNX::ParameterPresenterId::RotaryKnob);
    const auto* track = result.candidate->snapshot->ui.recipes.recipeFor(
        VNX::ComponentRecipeHostId::TrackHeader);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->variant, VNX::ComponentRecipeVariant::TrackExpanded);
    EXPECT_EQ(track->items.size(), 3);
}

TEST(ThemePackageLoader, RejectsInvalidComponentRecipeContracts)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    PackageContent content;
    content.schemaVersion = 3;
    content.recipes = R"json({"recipes":[{
        "host":"internal.widget","variant":"track.compact",
        "slots":[{"id":"identity"}]
    }]})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("host"),
                             QStringLiteral("theme.recipe-host"),
                             std::nullopt, content));
    auto result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.recipe-host"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::UnknownRecipeHost);

    content.recipes = R"json({"recipes":[{
        "host":"track.header","variant":"track.compact",
        "slots":[{"id":"inserts"}]
    }]})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("slot"),
                             QStringLiteral("theme.recipe-slot"),
                             std::nullopt, content));
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.recipe-slot"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::UnknownRecipeSlot);

    content.recipes = R"json({"recipes":[{
        "host":"track.header","variant":"track.compact",
        "slots":[{"id":"identity"},{"id":"identity"}]
    }]})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("duplicate"),
                             QStringLiteral("theme.recipe-duplicate"),
                             std::nullopt, content));
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.recipe-duplicate"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::DuplicateRecipeSlot);

    content.recipes = R"json({"recipes":[{
        "host":"track.header","variant":"track.compact",
        "slots":[{"id":"volume","minimum":100,"maximum":40}]
    }]})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("size"),
                             QStringLiteral("theme.recipe-size"),
                             std::nullopt, content));
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.recipe-size"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::RecipeSizeConflict);

    content.recipes = R"json({"recipes":[{
        "host":"track.header","variant":"track.compact",
        "slots":[{"id":"identity","presenter":"vertical-fader"}]
    }]})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("presenter"),
                             QStringLiteral("theme.recipe-presenter"),
                             std::nullopt, content));
    result = VNX::loadThemePackageCandidate(
        root.path(), QStringLiteral("theme.recipe-presenter"));
    EXPECT_EQ(onlyDiagnostic(result).code,
              VNX::ThemePackageErrorCode::UnsupportedRecipePresenter);
}

TEST(ThemePackageLoader, ReturnsBuiltinFallbackAsImmutableCandidate)
{
    const auto result = VNX::loadThemePackageCandidate(
        QString(), QStringLiteral("harken.builtin.dark"));

    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.candidate->snapshot);
    EXPECT_EQ(result.candidate->manifest.id,
              QStringLiteral("harken.builtin.dark"));
    EXPECT_EQ(result.candidate->inheritanceOrder,
              QStringList({QStringLiteral("harken.builtin.dark")}));
}
