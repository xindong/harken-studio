#include "Core/ColorPalette.h"
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
        {QStringLiteral("schemaVersion"), 7},
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

QJsonObject colorDefinition(const QString& roleKey, const QJsonArray& bindings)
{
    return {
        {QStringLiteral("roles"), QJsonArray{QJsonObject{
            {QStringLiteral("key"), roleKey},
            {QStringLiteral("group"), QStringLiteral("Surfaces")},
            {QStringLiteral("label"), QStringLiteral("Canvas")},
            {QStringLiteral("description"), QStringLiteral("The theme's canvas color.")},
            {QStringLiteral("bindings"), bindings}}}},
        {QStringLiteral("palettes"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("night")},
                        {QStringLiteral("displayName"), QStringLiteral("Night")},
                        {QStringLiteral("colorScheme"), QStringLiteral("dark")},
                        {QStringLiteral("colors"), QJsonObject{{roleKey, QStringLiteral("#182432")}}}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("day")},
                        {QStringLiteral("displayName"), QStringLiteral("Day")},
                        {QStringLiteral("colorScheme"), QStringLiteral("light")},
                        {QStringLiteral("colors"), QJsonObject{{roleKey, QStringLiteral("#e6e9f0")}}}}}},
        {QStringLiteral("defaultPalette"), QStringLiteral("night")}};
}

bool writeColorDefinition(const QString& root, const QString& id, const QJsonObject& definition)
{
    const QString packageRoot = QDir(root).filePath(id);
    QFile manifestFile(QDir(packageRoot).filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::ReadOnly))
        return false;
    QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    manifestFile.close();
    QJsonObject entries = manifest.value(QStringLiteral("entries")).toObject();
    entries.insert(QStringLiteral("colors"), QStringLiteral("colors.json"));
    manifest.insert(QStringLiteral("entries"), entries);
    return writeFile(manifestFile.fileName(), QJsonDocument(manifest).toJson())
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("colors.json")),
                     QJsonDocument(definition).toJson());
}

}

TEST(ThemePackageLoader, BuiltinThemeLoadsWithoutExternalPackages)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString missingRoot = QDir(root.path()).filePath(QStringLiteral("missing"));
    const auto discovered = VNX::discoverThemePackages(missingRoot);
    ASSERT_TRUE(discovered.diagnostics.isEmpty());
    ASSERT_EQ(discovered.themes.size(), 1);
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
        ASSERT_EQ(loaded.candidate->palettes.size(), 2);
        for (const auto& palette : loaded.candidate->palettes)
            for (const auto& role : loaded.candidate->colorRoles) {
                SCOPED_TRACE(role.key.toStdString());
                EXPECT_TRUE(palette.colors.value(role.key).isValid());
                for (const auto& binding : role.bindings)
                    EXPECT_TRUE(VNX::readThemeColor(*palette.snapshot, binding.target).isValid());
            }
    }
    EXPECT_TRUE(ids.contains(VNX::builtinThemeId()));
    EXPECT_FALSE(QDir(missingRoot).exists());
}

TEST(ThemePackageLoader, BuiltinThemeIdentityCannotBeReplacedByExternalPackages)
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
        EXPECT_EQ(*loaded.candidate->snapshot, *original.candidate->snapshot);
    }
}

TEST(ThemePackageLoader, ColorPalettesCannotReplaceThemeInheritance)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString paletteId = QStringLiteral("light");
    const QString childId = QStringLiteral("theme.child");
    EXPECT_FALSE(VNX::loadThemePackageCandidate(root.path(), paletteId).ok());
    ASSERT_TRUE(writeColorPackage(root.path(), childId, paletteId, {
        {QStringLiteral("accent.primary"), QStringLiteral("#80123456")}}));
    EXPECT_FALSE(VNX::loadThemePackageCandidate(root.path(), childId).ok());
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

TEST(ThemePackageLoader, RejectsUnknownColorOverrides)
{
    struct Override {
        const char* file;
        const char* key;
    };
    const std::array<Override, 26> overrides{{
        {"tokens.json", "slider.fill"},
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
            "schemaVersion":7,"id":"theme.linked","displayName":"Test theme.linked","version":"1.0.0",
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

TEST(ThemePackageLoader, ColorDefinitionsAreScopedToThemesAndReplaceInheritedRoles)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString parentId = QStringLiteral("theme.parent");
    const QString childId = QStringLiteral("theme.child");
    ASSERT_TRUE(writeColorPackage(root.path(), parentId, VNX::builtinThemeId(), {}));
    ASSERT_TRUE(writeColorDefinition(root.path(), parentId, colorDefinition(
        QStringLiteral("surface"), QJsonArray{
            QJsonObject{{QStringLiteral("target"), QStringLiteral("panel.background")}},
            QJsonObject{{QStringLiteral("target"), QStringLiteral("curve.canvasBackground")}}})));
    ASSERT_TRUE(writeColorPackage(root.path(), childId, parentId, {}));
    ASSERT_TRUE(writeColorDefinition(root.path(), childId, colorDefinition(
        QStringLiteral("outline"), QJsonArray{
            QJsonObject{{QStringLiteral("target"), QStringLiteral("curve.grid")},
                        {QStringLiteral("alpha"), 96}}})));

    const auto parent = VNX::loadThemePackageCandidate(root.path(), parentId);
    const auto child = VNX::loadThemePackageCandidate(root.path(), childId);
    ASSERT_TRUE(parent.ok());
    ASSERT_TRUE(child.ok());
    ASSERT_EQ(parent.candidate->colorRoles.size(), 1);
    ASSERT_EQ(child.candidate->colorRoles.size(), 1);
    EXPECT_NE(parent.candidate->colorRoles.front().key, child.candidate->colorRoles.front().key);
    ASSERT_EQ(child.candidate->palettes.size(), 2);
    EXPECT_EQ(parent.candidate->defaultPaletteId, child.candidate->defaultPaletteId);
    EXPECT_FALSE(child.candidate->palettes.front().colors.contains(QStringLiteral("surface")));
    EXPECT_EQ(child.candidate->snapshot->domains.curve.colors.grid.alpha(), 96);
    EXPECT_EQ(child.candidate->palettes.back().snapshot->colorScheme, VNX::ThemeColorScheme::Light);
    EXPECT_EQ(parent.candidate->snapshot->domains.curve.colors.canvasBackground, QColor("#182432"));
}

TEST(ThemePackageLoader, InheritedPalettesReceiveRoleAndFixedColorOverridesIndependently)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString parentId = QStringLiteral("theme.parent");
    const QString childId = QStringLiteral("theme.child");
    ASSERT_TRUE(writeColorPackage(root.path(), parentId, VNX::builtinThemeId(), {}));
    ASSERT_TRUE(writeColorDefinition(root.path(), parentId, colorDefinition(
        QStringLiteral("surface"), QJsonArray{
            QJsonObject{{QStringLiteral("target"), QStringLiteral("curve.grid")},
                        {QStringLiteral("alpha"), 96}},
            QJsonObject{{QStringLiteral("target"), QStringLiteral("panel.background")}},
            QJsonObject{{QStringLiteral("target"), QStringLiteral("curve.canvasBackground")}}})));
    ASSERT_TRUE(writeColorPackage(root.path(), childId, parentId, {
        {QStringLiteral("panel.background"), QStringLiteral("#445566")},
        {QStringLiteral("text.primary"), QStringLiteral("#ddeeff")}}));
    const auto parent = VNX::loadThemePackageCandidate(root.path(), parentId);
    const auto child = VNX::loadThemePackageCandidate(root.path(), childId);
    ASSERT_TRUE(parent.ok());
    ASSERT_TRUE(child.ok());
    ASSERT_EQ(child.candidate->palettes.size(), parent.candidate->palettes.size());
    for (const auto& palette : child.candidate->palettes) {
        EXPECT_EQ(palette.colors.value(QStringLiteral("surface")), QColor("#445566"));
        EXPECT_EQ(palette.snapshot->domains.curve.colors.canvasBackground, QColor("#445566"));
        EXPECT_EQ(palette.snapshot->ui.colors.textPrimary, QColor("#ddeeff"));
        EXPECT_EQ(palette.snapshot->domains.curve.colors.grid.alpha(), 96);
    }
    EXPECT_NE(parent.candidate->palettes.front().colors.value(QStringLiteral("surface")),
              parent.candidate->palettes.back().colors.value(QStringLiteral("surface")));
    EXPECT_NE(parent.candidate->snapshot->ui.colors.textPrimary, QColor("#ddeeff"));
}

TEST(ThemePackageLoader, RejectsUnknownAndAliasedDuplicateBindingTargets)
{
    const QList<QJsonArray> bindings{
        QJsonArray{QJsonObject{{QStringLiteral("target"), QStringLiteral("custom.missing")}}},
        QJsonArray{QJsonObject{{QStringLiteral("target"), QStringLiteral("curve.line")}},
                   QJsonObject{{QStringLiteral("target"), QStringLiteral("curve.attenuationVolume")}}}};
    for (const auto& targets : bindings) {
        QTemporaryDir root;
        ASSERT_TRUE(root.isValid());
        const QString id = QStringLiteral("theme.invalid");
        ASSERT_TRUE(writeColorPackage(root.path(), id, VNX::builtinThemeId(), {}));
        ASSERT_TRUE(writeColorDefinition(root.path(), id, colorDefinition(QStringLiteral("custom"), targets)));
        const auto loaded = VNX::loadThemePackageCandidate(root.path(), id);
        EXPECT_FALSE(loaded.ok());
        ASSERT_FALSE(loaded.diagnostics.isEmpty());
        EXPECT_TRUE(loaded.diagnostics.front().path.startsWith(QStringLiteral("$.roles[0].bindings[")));
    }
}

TEST(ThemePackageLoader, ExplicitPalettesRejectDirectAssignmentsToTheirBoundTargets)
{
    const QList<std::pair<QString, QString>> assignments{
        {QStringLiteral("tokens.json"), QStringLiteral("panel.background")},
        {QStringLiteral("visuals/curve.json"), QStringLiteral("curve.canvasBackground")}};
    for (const auto& [file, target] : assignments) {
        QTemporaryDir root;
        ASSERT_TRUE(root.isValid());
        const QString id = QStringLiteral("theme.conflict");
        ASSERT_TRUE(writeColorPackage(root.path(), id, VNX::builtinThemeId(), {}));
        ASSERT_TRUE(writeColorDefinition(root.path(), id, colorDefinition(
            QStringLiteral("surface"), QJsonArray{
                QJsonObject{{QStringLiteral("target"), QStringLiteral("panel.background")}},
                QJsonObject{{QStringLiteral("target"), QStringLiteral("curve.canvasBackground")}}})));
        const QJsonObject document{{QStringLiteral("colors"), QJsonObject{
            {target, QStringLiteral("#182432")}}}};
        ASSERT_TRUE(writeFile(QDir(QDir(root.path()).filePath(id)).filePath(file),
                              QJsonDocument(document).toJson()));
        const auto loaded = VNX::loadThemePackageCandidate(root.path(), id);
        EXPECT_FALSE(loaded.ok());
        ASSERT_FALSE(loaded.diagnostics.isEmpty());
        EXPECT_EQ(loaded.diagnostics.front().code, VNX::ThemePackageErrorCode::InvalidValue);
        EXPECT_EQ(loaded.diagnostics.front().path, QStringLiteral("$.colors.") + target);
    }
}

TEST(ThemePackageLoader, InheritedRoleOverridesRequireConsistentValuesWithoutAlphaLoss)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString parentId = QStringLiteral("theme.parent");
    const QString childId = QStringLiteral("theme.child");
    ASSERT_TRUE(writeColorPackage(root.path(), parentId, VNX::builtinThemeId(), {}));
    ASSERT_TRUE(writeColorDefinition(root.path(), parentId, colorDefinition(
        QStringLiteral("surface"), QJsonArray{
            QJsonObject{{QStringLiteral("target"), QStringLiteral("curve.grid")},
                        {QStringLiteral("alpha"), 96}},
            QJsonObject{{QStringLiteral("target"), QStringLiteral("panel.background")}},
            QJsonObject{{QStringLiteral("target"), QStringLiteral("curve.canvasBackground")}}})));
    ASSERT_TRUE(writeColorPackage(root.path(), childId, parentId, {
        {QStringLiteral("panel.background"), QStringLiteral("#445566")}}));
    const QString visualFile = QDir(QDir(root.path()).filePath(childId))
        .filePath(QStringLiteral("visuals/curve.json"));
    ASSERT_TRUE(writeFile(visualFile, R"json({"colors":{"curve.grid":"#445566"}})json"));
    const auto alphaConflict = VNX::loadThemePackageCandidate(root.path(), childId);
    EXPECT_FALSE(alphaConflict.ok());
    ASSERT_FALSE(alphaConflict.diagnostics.isEmpty());
    EXPECT_EQ(alphaConflict.diagnostics.front().code, VNX::ThemePackageErrorCode::InvalidValue);

    ASSERT_TRUE(writeFile(visualFile, R"json({"colors":{"curve.canvasBackground":"#abcdef"}})json"));
    const auto valueConflict = VNX::loadThemePackageCandidate(root.path(), childId);
    EXPECT_FALSE(valueConflict.ok());
    ASSERT_FALSE(valueConflict.diagnostics.isEmpty());
    EXPECT_EQ(valueConflict.diagnostics.front().code, VNX::ThemePackageErrorCode::InvalidValue);

    ASSERT_TRUE(writeFile(visualFile, R"json({"colors":{"curve.canvasBackground":"#445566"}})json"));
    const auto consistent = VNX::loadThemePackageCandidate(root.path(), childId);
    ASSERT_TRUE(consistent.ok());
    for (const auto& palette : consistent.candidate->palettes) {
        EXPECT_EQ(palette.colors.value(QStringLiteral("surface")), QColor("#445566"));
        EXPECT_EQ(palette.snapshot->ui.colors.panelBackground, QColor("#445566"));
        EXPECT_EQ(palette.snapshot->domains.curve.colors.canvasBackground, QColor("#445566"));
        EXPECT_EQ(palette.snapshot->domains.curve.colors.grid.alpha(), 96);
    }
}
