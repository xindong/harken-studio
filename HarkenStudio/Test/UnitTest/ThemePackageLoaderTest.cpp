#include "Core/ThemeColorCatalog.h"
#include "Core/ThemePackageLoader.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool writeColorPackage(const QString& themesRoot,
                       const QString& id,
                       const QString& baseId,
                       const QJsonObject& colors)
{
    const QString packageRoot = QDir(themesRoot).filePath(id);
    const QJsonObject manifest{
        {QStringLiteral("schemaVersion"), 6},
        {QStringLiteral("id"), id},
        {QStringLiteral("displayName"), id},
        {QStringLiteral("version"), QStringLiteral("1.0.0")},
        {QStringLiteral("baseThemeId"), baseId},
        {QStringLiteral("compatibility"), QJsonObject{
            {QStringLiteral("harkenVersion"), QStringLiteral(">=0.1.0 <1.0.0")},
            {QStringLiteral("capabilities"), QJsonArray{
                QStringLiteral("curve-v1"), QStringLiteral("meter-v1"),
                QStringLiteral("waveform-v1")}}}},
        {QStringLiteral("entries"), QJsonObject{
            {QStringLiteral("tokens"), QStringLiteral("tokens.json")},
            {QStringLiteral("visuals"), QJsonObject{
                {QStringLiteral("curve"), QStringLiteral("visuals/curve.json")},
                {QStringLiteral("meter"), QStringLiteral("visuals/meter.json")},
                {QStringLiteral("waveform"), QStringLiteral("visuals/waveform.json")},
                {QStringLiteral("nodeGraph"), QStringLiteral("visuals/nodeGraph.json")}}},
            {QStringLiteral("assetMap"), QStringLiteral("assets.json")}}}
    };
    return writeFile(QDir(packageRoot).filePath(QStringLiteral("manifest.json")),
                     QJsonDocument(manifest).toJson(QJsonDocument::Compact))
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("tokens.json")),
                     QJsonDocument(QJsonObject{{QStringLiteral("colors"), colors}})
                         .toJson(QJsonDocument::Compact))
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/curve.json")),
                     R"json({"colors":{}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/meter.json")),
                     R"json({"colors":{}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/waveform.json")),
                     R"json({"colors":{}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/nodeGraph.json")),
                     R"json({"colors":{}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("assets.json")),
                     R"json({"assets":{}})json");
}

}

TEST(ThemePackageLoader, BuiltinPresetsLoadWithoutExternalPackages)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString missingRoot = QDir(root.path()).filePath(QStringLiteral("missing"));
    const auto discovered = VNX::discoverThemePackages(missingRoot);
    ASSERT_TRUE(discovered.diagnostics.isEmpty());
    ASSERT_FALSE(discovered.themes.isEmpty());
    QSet<QString> ids;
    for (const auto& preset : discovered.themes) {
        SCOPED_TRACE(preset.id.toStdString());
        EXPECT_TRUE(preset.builtin);
        EXPECT_FALSE(preset.id.isEmpty());
        EXPECT_FALSE(preset.displayName.isEmpty());
        EXPECT_FALSE(ids.contains(preset.id));
        ids.insert(preset.id);
        const auto loaded = VNX::loadThemePackageCandidate(missingRoot, preset.id);
        ASSERT_TRUE(loaded.ok());
        EXPECT_EQ(loaded.candidate->manifest.id, preset.id);
        EXPECT_EQ(loaded.candidate->manifest.displayName, preset.displayName);
        EXPECT_EQ(loaded.candidate->inheritanceOrder, QStringList{preset.id});
        for (const auto& role : VNX::themeColorRoles()) {
            SCOPED_TRACE(role.key);
            EXPECT_TRUE(role.read(*loaded.candidate->snapshot).isValid());
        }
    }
    EXPECT_TRUE(ids.contains(VNX::builtinThemeId()));
    EXPECT_FALSE(QDir(missingRoot).exists());
}

TEST(ThemePackageLoader, BuiltinPresetIdsCannotBeReplacedByExternalPackages)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const auto builtins = VNX::discoverThemePackages(root.path());
    ASSERT_FALSE(builtins.themes.isEmpty());
    for (const auto& preset : builtins.themes) {
        SCOPED_TRACE(preset.id.toStdString());
        QTemporaryDir packages;
        ASSERT_TRUE(packages.isValid());
        const auto original = VNX::loadThemePackageCandidate(root.path(), preset.id);
        ASSERT_TRUE(original.ok());
        ASSERT_TRUE(writeColorPackage(packages.path(), preset.id, VNX::builtinThemeId(), {
            {QStringLiteral("accent.primary"), QStringLiteral("#80123456")}}));

        const auto discovered = VNX::discoverThemePackages(packages.path());
        EXPECT_TRUE(std::any_of(discovered.diagnostics.cbegin(), discovered.diagnostics.cend(),
            [](const VNX::ThemePackageDiagnostic& diagnostic) {
                return diagnostic.code == VNX::ThemePackageErrorCode::DuplicateThemeId;
            }));
        EXPECT_EQ(std::count_if(discovered.themes.cbegin(), discovered.themes.cend(),
            [&preset](const VNX::ThemePackageDescriptor& theme) {
                return theme.id == preset.id && theme.builtin;
            }), 1);
        const auto loaded = VNX::loadThemePackageCandidate(packages.path(), preset.id);
        ASSERT_TRUE(loaded.ok());
        for (const auto& role : VNX::themeColorRoles()) {
            SCOPED_TRACE(role.key);
            EXPECT_EQ(role.read(*loaded.candidate->snapshot),
                      role.read(*original.candidate->snapshot));
        }
    }
}

TEST(ThemePackageLoader, ExternalPackagesCanInheritAlternateBuiltinPresets)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const auto builtins = VNX::discoverThemePackages(root.path());
    bool inheritedAlternate = false;
    const QColor accent(QStringLiteral("#80123456"));
    for (const auto& preset : builtins.themes) {
        if (!preset.builtin || preset.id == VNX::builtinThemeId())
            continue;
        SCOPED_TRACE(preset.id.toStdString());
        inheritedAlternate = true;
        const QString childId = QStringLiteral("theme.child");
        ASSERT_TRUE(writeColorPackage(root.path(), childId, preset.id, {
            {QStringLiteral("accent.primary"), accent.name(QColor::HexArgb)}}));
        const auto base = VNX::loadThemePackageCandidate(root.path(), preset.id);
        const auto child = VNX::loadThemePackageCandidate(root.path(), childId);
        ASSERT_TRUE(base.ok());
        ASSERT_TRUE(child.ok());
        EXPECT_EQ(child.candidate->inheritanceOrder, (QStringList{preset.id, childId}));
        for (const auto& role : VNX::themeColorRoles()) {
            SCOPED_TRACE(role.key);
            EXPECT_EQ(role.read(*child.candidate->snapshot),
                      QLatin1String(role.key) == QLatin1String("accent.primary")
                          ? accent : role.read(*base.candidate->snapshot));
        }
        EXPECT_EQ(child.candidate->snapshot->qtPalette.highlight, accent);
        EXPECT_EQ(child.candidate->snapshot->domains.curve.colors.handleActiveFill, accent);
    }
    EXPECT_TRUE(inheritedAlternate);
}

TEST(ThemePackageLoader, ResolvesSharedColorsAfterInheritanceWithoutMutatingSnapshots)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString parentId = QStringLiteral("theme.parent");
    const QString childId = QStringLiteral("theme.child");
    const QColor parentText(QStringLiteral("#c4d5e6"));
    const QColor inheritedLabel(QStringLiteral("#8a9bac"));
    const QColor childText(QStringLiteral("#e6d5c4"));
    const QColor childAccent(QStringLiteral("#cc88ee"));
    ASSERT_TRUE(writeColorPackage(root.path(), parentId, VNX::builtinThemeId(), {
        {QStringLiteral("text.primary"), parentText.name()},
        {QStringLiteral("text.secondary"), inheritedLabel.name()},
        {QStringLiteral("accent.primary"), QStringLiteral("#1abc9c")}}));
    ASSERT_TRUE(writeColorPackage(root.path(), childId, parentId, {
        {QStringLiteral("text.primary"), childText.name()},
        {QStringLiteral("accent.primary"), childAccent.name()}}));

    const auto parent = VNX::loadThemePackageCandidate(root.path(), parentId);
    const auto child = VNX::loadThemePackageCandidate(root.path(), childId);
    ASSERT_TRUE(parent.ok());
    ASSERT_TRUE(child.ok());
    const auto parentSnapshot = parent.candidate->snapshot;
    const auto childSnapshot = child.candidate->snapshot;
    EXPECT_EQ(parentSnapshot->domains.nodeGraphNode.contentValue, parentText);
    EXPECT_EQ(childSnapshot->domains.nodeGraphNode.contentValue, childText);
    EXPECT_EQ(childSnapshot->qtPalette.text, childText);
    EXPECT_EQ(childSnapshot->domains.meter.spectrumMeter.colors.axisText, inheritedLabel);
    EXPECT_EQ(childSnapshot->domains.curve.colors.handleActiveFill, childAccent);

    const QColor reloadedText(QStringLiteral("#abcdef"));
    const QColor reloadedAccent(QStringLiteral("#fedcba"));
    ASSERT_TRUE(writeColorPackage(root.path(), childId, parentId, {
        {QStringLiteral("text.primary"), reloadedText.name()},
        {QStringLiteral("accent.primary"), reloadedAccent.name()}}));
    const auto reloaded = VNX::loadThemePackageCandidate(root.path(), childId);
    ASSERT_TRUE(reloaded.ok());
    EXPECT_EQ(reloaded.candidate->snapshot->domains.nodeGraphNode.contentValue,
              reloadedText);
    EXPECT_EQ(reloaded.candidate->snapshot->domains.curve.colors.handleActiveFill,
              reloadedAccent);
    EXPECT_EQ(childSnapshot->domains.nodeGraphNode.contentValue, childText);
    EXPECT_EQ(childSnapshot->domains.curve.colors.handleActiveFill, childAccent);
    EXPECT_EQ(parentSnapshot->domains.nodeGraphNode.contentValue, parentText);
}

TEST(ThemePackageLoader, RejectsRemovedColorKeys)
{
    struct Override {
        const char* file;
        const char* key;
    };
    const std::array<Override, 30> overrides{{
        {"tokens.json", "slider.fill"},
        {"visuals/curve.json", "curve.axisText"},
        {"visuals/meter.json", "meter.spectrum.axisText"},
        {"visuals/waveform.json", "waveform.dbScale.label"},
        {"visuals/waveform.json", "waveform.previewPlayhead"},
        {"visuals/waveform.json", "waveform.previewGradientTop"},
        {"visuals/waveform.json", "waveform.previewGradientMid"},
        {"visuals/waveform.json", "waveform.previewLoopFill"},
        {"visuals/waveform.json", "waveform.previewLoopBorder"},
        {"visuals/waveform.json", "waveform.previewPeakClip"},
        {"visuals/waveform.json", "waveform.previewPeakSafe"},
        {"visuals/nodeGraph.json", "nodeGraph.sound"},
        {"visuals/nodeGraph.json", "nodeGraph.audioLayer"},
        {"visuals/nodeGraph.json", "nodeGraph.bus"},
        {"visuals/nodeGraph.json", "nodeGraph.receiveBus"},
        {"visuals/nodeGraph.json", "nodeGraph.attenuation"},
        {"visuals/nodeGraph.json", "nodeGraph.action"},
        {"visuals/nodeGraph.json", "nodeGraph.fallback"},
        {"visuals/nodeGraph.json", "nodeGraph.header.sound"},
        {"visuals/nodeGraph.json", "nodeGraph.header.audioLayer"},
        {"visuals/nodeGraph.json", "nodeGraph.header.bus"},
        {"visuals/nodeGraph.json", "nodeGraph.header.receiveBus"},
        {"visuals/nodeGraph.json", "nodeGraph.header.attenuation"},
        {"visuals/nodeGraph.json", "nodeGraph.header.action"},
        {"visuals/nodeGraph.json", "nodeGraph.header.fallback"},
        {"visuals/nodeGraph.json", "nodeGraph.role.source"},
        {"visuals/nodeGraph.json", "nodeGraph.role.parent"},
        {"visuals/nodeGraph.json", "nodeGraph.role.bus"},
        {"visuals/nodeGraph.json", "nodeGraph.role.receiveBus"},
        {"visuals/nodeGraph.json", "nodeGraph.node.titleText"}
    }};
    for (const auto& override : overrides) {
        SCOPED_TRACE(override.key);
        QTemporaryDir root;
        ASSERT_TRUE(root.isValid());
        const QString id = QStringLiteral("theme.override");
        ASSERT_TRUE(writeColorPackage(root.path(), id, VNX::builtinThemeId(), {}));
        const QString key = QString::fromLatin1(override.key);
        const QJsonObject document{{QStringLiteral("colors"), QJsonObject{
            {key, QStringLiteral("#123456")}}}};
        ASSERT_TRUE(writeFile(QDir(QDir(root.path()).filePath(id)).filePath(
                                  QString::fromLatin1(override.file)),
                              QJsonDocument(document).toJson(QJsonDocument::Compact)));

        const auto result = VNX::loadThemePackageCandidate(root.path(), id);
        EXPECT_FALSE(result.ok());
        EXPECT_FALSE(result.candidate.has_value());
        EXPECT_TRUE(std::any_of(result.diagnostics.cbegin(), result.diagnostics.cend(),
            [&key](const VNX::ThemePackageDiagnostic& diagnostic) {
                return diagnostic.code == VNX::ThemePackageErrorCode::UnknownSemanticKey
                    && diagnostic.path.endsWith(key);
            }));
    }
}

TEST(ThemePackageLoader, RejectsSymbolicLinkEscapeWhenSupported)
{
    QTemporaryDir root;
    QTemporaryDir outside;
    const QString packageRoot = QDir(root.path()).filePath(QStringLiteral("linked"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("manifest.json")),
        R"json({
            "schemaVersion":6,"id":"theme.linked","displayName":"Test theme.linked","version":"1.0.0",
            "compatibility":{"harkenVersion":">=0.1.0 <1.0.0","capabilities":["curve-v1","meter-v1","waveform-v1"]},
            "entries":{"tokens":"tokens.json","visuals":{"curve":"visuals/curve.json","meter":"visuals/meter.json","waveform":"visuals/waveform.json"},"assetMap":"assets.json"}
        })json"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("tokens.json")),
        R"json({"colors":{"accent.primary":"#ff00f0ff"},"metrics":{"spacing.medium":12}})json"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/curve.json")),
        R"json({"colors":{"curve.line":"#ff112233"},"metrics":{"curve.lineWidth":2}})json"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/meter.json")),
        R"json({"colors":{"meter.green":"#ff22aa44","meter.spectrogram.emptyHistory":"#ff102030"},"metrics":{"meter.spectrogram.plotInsetLeft":42}})json"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/waveform.json")),
        R"json({"colors":{"waveform.line":"#ff334455"}})json"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("assets.json")),
        R"json({"assets":{}})json"));
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

    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.candidate.has_value());
}
