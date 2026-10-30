#include "Core/ColorPalette.h"
#include "Core/FontManager.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/QSettingsStorage.h"
#include "Core/ThemeColorCatalog.h"
#include "Core/ThemeColorResolver.h"
#include "Core/ThemeManager.h"
#include "Core/WidgetAppearance.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWidget>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <thread>

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

bool writePackage(const QString& themesRoot,
                  const QString& directoryName,
                  const QString& id,
                  const QString& accent,
                  const QString& curveLine)
{
    const QString packageRoot = QDir(themesRoot).filePath(directoryName);
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":6,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"Test " + id.toUtf8()
        + "\",\"version\":\"1.0.0\"" + ","
          "\"compatibility\":{\"harkenVersion\":\">=0.1.0 <1.0.0\","
          "\"capabilities\":[\"curve-v1\",\"meter-v1\",\"waveform-v1\"]},"
          "\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\","
          "\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\"},"
          "\"assetMap\":\"assets.json\"}}";
    const QByteArray tokens = QByteArray("{\"colors\":{\"accent.primary\":\"")
        + accent.toUtf8() + "\"}}";
    const QByteArray curve = QByteArray("{\"colors\":{\"curve.line\":\"")
        + curveLine.toUtf8() + "\"}}";
    return writeFile(QDir(packageRoot).filePath(QStringLiteral("manifest.json")),
                     manifest)
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("tokens.json")),
                     tokens)
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/curve.json")), curve)
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/meter.json")),
                     R"json({"colors":{"meter.green":"#ff22aa44"}})json")
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/waveform.json")),
                     R"json({"colors":{"waveform.line":"#ff334455"}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("assets.json")),
                     R"json({"assets":{}})json");
}

std::unique_ptr<VNX::ISettingsStorage> memoryStorage()
{
    return std::make_unique<VNX::MemorySettingsStorage>();
}

QVariantMap storedValues(const VNX::ISettingsStorage& storage)
{
    QVariantMap values;
    for (const QString& key : storage.allKeys())
        values.insert(key, storage.value(key));
    return values;
}

class WritableSettingsStorage : public VNX::MemorySettingsStorage
{
public:
    bool isWritable() const override { return writable; }
    bool writable = true;
};

}

TEST(ThemeManager, ReloadDoesNotMutatePreviouslyPublishedSnapshot)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("mutable"),
                             QStringLiteral("theme.mutable"),
                             QStringLiteral("#ff111111"),
                             QStringLiteral("#ff222222")));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.mutable")).ok());
    const auto oldSnapshot = manager.snapshot();
    int fonts = 0;
    int structures = 0;
    QObject::connect(&VNX::FontManager::instance(), &VNX::FontManager::fontChanged,
                     &manager, [&] { ++fonts; });
    QObject::connect(&manager, &VNX::ThemeManager::themeChanged,
                     &manager, [&] { ++structures; });
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("mutable"),
                             QStringLiteral("theme.mutable"),
                             QStringLiteral("#ff333333"),
                             QStringLiteral("#ff444444")));
    const int pointSize = oldSnapshot->ui.typography.body.pointSize + 2;
    ASSERT_TRUE(writeFile(QDir(root.path()).filePath(QStringLiteral("mutable/tokens.json")),
        QStringLiteral("{\"colors\":{\"accent.primary\":\"#ff333333\"},"
                       "\"fonts\":{\"body\":{\"pointSize\":%1}}}")
            .arg(pointSize).toUtf8()));

    ASSERT_TRUE(manager.reloadTheme().ok());

    EXPECT_EQ(oldSnapshot->ui.colors.accent,
              QColor(QStringLiteral("#ff111111")));
    EXPECT_EQ(manager.snapshot()->ui.colors.accent,
              QColor(QStringLiteral("#ff333333")));
    EXPECT_EQ(manager.snapshot()->ui.typography.body.pointSize, pointSize);
    EXPECT_NE(oldSnapshot->ui.typography.body.pointSize, pointSize);
    EXPECT_EQ(fonts, 1);
    EXPECT_EQ(structures, 1);
}

TEST(ThemeManager, RejectsPublicationFromAnotherThread)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.reloadTheme().ok());
    const auto before = manager.snapshot();
    VNX::ThemeManagerOperationResult result;

    std::thread worker([&]() { result = manager.reloadTheme(); });
    worker.join();

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(manager.snapshot(), before);
}

TEST(ThemeManager, PreviewCancellationKeepsCommittedStateWithoutReloadingFiles)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString packageId = QStringLiteral("theme.preview");
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff112233"),
                             QStringLiteral("#ff334455")));
    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    auto* stored = storage.get();
    VNX::ThemeManager manager(root.path(), std::move(storage));
    ASSERT_TRUE(manager.selectTheme(packageId).ok());
    const auto committed = manager.appearance();
    const auto original = manager.snapshot();
    const auto settings = storedValues(*stored);

    auto draft = committed;
    draft.themeId = VNX::builtinThemeId();
    draft.paletteId = VNX::defaultPaletteId(draft.themeId);
    const QColor accent(QStringLiteral("#ffbc1234"));
    const QColor text(QStringLiteral("#ffc2d3e4"));
    draft.colors[draft.paletteId].insert(QStringLiteral("accent.primary"), accent);
    draft.colors[draft.paletteId].insert(QStringLiteral("text.secondary"), text);
    ASSERT_TRUE(manager.previewAppearance(draft).ok());
    const auto preview = manager.snapshot();
    EXPECT_EQ(manager.appearance(), committed);
    EXPECT_EQ(manager.currentThemeId(), packageId);
    EXPECT_EQ(storedValues(*stored), settings);
    EXPECT_EQ(preview->ui.colors.accent, accent);
    EXPECT_EQ(preview->qtPalette.highlight, accent);
    EXPECT_EQ(preview->domains.curve.colors.pointBorder, accent);
    EXPECT_EQ(preview->domains.nodeGraphNode.contentLabel, text);
    EXPECT_EQ(original->ui.colors.accent, QColor(QStringLiteral("#ff112233")));

    ASSERT_TRUE(QDir(root.path()).rename(QStringLiteral("preview"),
                                         QStringLiteral("unavailable")));
    ASSERT_TRUE(QFile::remove(QDir(root.path()).filePath(
        QStringLiteral("unavailable/manifest.json"))));
    manager.cancelAppearancePreview();

    EXPECT_EQ(manager.appearance(), committed);
    EXPECT_EQ(manager.currentThemeId(), packageId);
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, original->ui.colors.accent);
    EXPECT_EQ(manager.snapshot()->domains.curve.colors.attenuationVolume,
              original->domains.curve.colors.attenuationVolume);
    EXPECT_EQ(preview->ui.colors.accent, accent);
    EXPECT_EQ(storedValues(*stored), settings);
}

TEST(ThemeManager, ContinuousPreviewRetainsBaseWithoutAccumulatingOverrides)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString packageId = QStringLiteral("theme.preview");
    const QString accentKey = QStringLiteral("accent.primary");
    const QString textKey = QStringLiteral("text.secondary");
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff112233"),
                             QStringLiteral("#ff334455")));
    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    auto* stored = storage.get();
    VNX::ThemeManager manager(root.path(), std::move(storage));
    ASSERT_TRUE(manager.selectTheme(packageId).ok());
    const auto committed = manager.appearance();
    const auto original = manager.snapshot();
    const auto settings = storedValues(*stored);
    auto draft = committed;
    draft.colors[VNX::defaultPaletteId(packageId)].insert(accentKey, QColor(QStringLiteral("#80abcdef")));
    draft.colors[VNX::defaultPaletteId(packageId)].insert(textKey, QColor(QStringLiteral("#ffdefabc")));
    ASSERT_TRUE(manager.previewAppearance(draft).ok());
    const auto firstPreview = manager.snapshot();

    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff445566"),
                             QStringLiteral("#ff778899")));
    ASSERT_TRUE(QFile::remove(QDir(root.path()).filePath(
        QStringLiteral("preview/manifest.json"))));
    draft.colors.remove(VNX::defaultPaletteId(packageId));
    ASSERT_TRUE(manager.previewAppearance(draft).ok());
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, original->ui.colors.accent);
    EXPECT_EQ(manager.snapshot()->ui.colors.textSecondary, original->ui.colors.textSecondary);
    EXPECT_EQ(manager.snapshot()->domains.curve.colors.attenuationVolume,
              original->domains.curve.colors.attenuationVolume);
    EXPECT_EQ(firstPreview->ui.colors.accent, QColor(QStringLiteral("#80abcdef")));
    EXPECT_EQ(manager.appearance(), committed);
    EXPECT_EQ(storedValues(*stored), settings);

    manager.cancelAppearancePreview();
    EXPECT_FALSE(manager.previewAppearance(draft).ok());
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff445566"),
                             QStringLiteral("#ff778899")));
    ASSERT_TRUE(manager.previewAppearance(draft).ok());
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, QColor(QStringLiteral("#ff445566")));
    EXPECT_EQ(manager.snapshot()->domains.curve.colors.attenuationVolume,
              QColor(QStringLiteral("#ff778899")));

    draft.themeId = VNX::builtinThemeId();
    draft.paletteId = VNX::defaultPaletteId(draft.themeId);
    ASSERT_TRUE(manager.previewAppearance(draft).ok());
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff778899"),
                             QStringLiteral("#ff112233")));
    draft.themeId = packageId;
    draft.paletteId = VNX::defaultPaletteId(packageId);
    ASSERT_TRUE(manager.previewAppearance(draft).ok());
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, QColor(QStringLiteral("#ff778899")));
    manager.cancelAppearancePreview();
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, original->ui.colors.accent);
    EXPECT_EQ(storedValues(*stored), settings);
}

TEST(ThemeManager, CommitAndReloadStartNewPreviewSessions)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString packageId = QStringLiteral("theme.preview");
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff112233"),
                             QStringLiteral("#ff334455")));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(packageId).ok());
    auto draft = manager.appearance();
    const QColor text(QStringLiteral("#ffabcdef"));
    draft.colors[VNX::defaultPaletteId(packageId)].insert(QStringLiteral("text.primary"), text);
    ASSERT_TRUE(manager.previewAppearance(draft).ok());

    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff223344"),
                             QStringLiteral("#ff334455")));
    ASSERT_TRUE(manager.applyAppearance(draft).ok());
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, QColor(QStringLiteral("#ff223344")));
    EXPECT_EQ(manager.snapshot()->ui.colors.textPrimary, text);

    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff334455"),
                             QStringLiteral("#ff334455")));
    ASSERT_TRUE(manager.previewAppearance(draft).ok());
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, QColor(QStringLiteral("#ff334455")));

    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff445566"),
                             QStringLiteral("#ff334455")));
    ASSERT_TRUE(manager.reloadTheme().ok());
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, QColor(QStringLiteral("#ff445566")));
    EXPECT_EQ(manager.appearance(), draft);

    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("preview"), packageId,
                             QStringLiteral("#ff556677"),
                             QStringLiteral("#ff334455")));
    ASSERT_TRUE(manager.previewAppearance(draft).ok());
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, QColor(QStringLiteral("#ff556677")));
    manager.cancelAppearancePreview();
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, QColor(QStringLiteral("#ff445566")));
    EXPECT_EQ(manager.snapshot()->ui.colors.textPrimary, text);
}

TEST(ThemeManager, MigratesLegacyColorsIntoSharedPalettesAndPersistsSelection)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString packageId = QStringLiteral("theme.custom");
    const QString packagePaletteId = VNX::defaultPaletteId(packageId);
    const QString builtinPaletteId = VNX::defaultPaletteId(VNX::builtinThemeId());
    const QString amberPaletteId = VNX::builtinAmberPaletteId();
    const QString storagePath = QDir(root.path()).filePath(QStringLiteral("settings.ini"));
    const QString key = QStringLiteral("accent.primary");
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("custom"), packageId,
                             QStringLiteral("#ff112233"),
                             QStringLiteral("#ff334455")));
    const QColor packageOverride(QStringLiteral("#f1234567"));
    const QColor builtinOverride(QStringLiteral("#ff765432"));
    const QColor amberOverride(QStringLiteral("#ff987654"));
    {
        VNX::QSettingsStorage storage(storagePath);
        storage.setValue(QStringLiteral("Appearance/ThemeId"), QStringLiteral("harken.builtin.amber"));
        storage.setValue(QStringLiteral("Appearance/Colors"), QVariantMap{
            {packageId, QVariantMap{{key, packageOverride.name(QColor::HexArgb)}}},
            {VNX::builtinThemeId(), QVariantMap{{key, builtinOverride.name(QColor::HexArgb)}}},
            {QStringLiteral("harken.builtin.amber"), QVariantMap{{key, amberOverride.name(QColor::HexArgb)}}}
        });
        storage.sync();
    }
    {
        VNX::ThemeManager manager(root.path(), std::make_unique<VNX::QSettingsStorage>(storagePath));
        EXPECT_EQ(manager.currentThemeId(), VNX::builtinThemeId());
        EXPECT_EQ(manager.currentPaletteId(), amberPaletteId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, amberOverride);
        EXPECT_EQ(manager.appearance().colors.value(packagePaletteId).value(key), packageOverride);
        EXPECT_EQ(manager.appearance().colors.value(builtinPaletteId).value(key), builtinOverride);

        ASSERT_TRUE(manager.selectTheme(packageId).ok());
        EXPECT_EQ(manager.currentPaletteId(), packagePaletteId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, packageOverride);
        auto shared = manager.appearance();
        shared.paletteId = amberPaletteId;
        ASSERT_TRUE(manager.applyAppearance(shared).ok());
        EXPECT_EQ(manager.currentThemeId(), packageId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, amberOverride);
        ASSERT_TRUE(manager.reloadTheme().ok());
        EXPECT_EQ(manager.currentPaletteId(), amberPaletteId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, amberOverride);

        ASSERT_TRUE(manager.selectTheme(VNX::builtinThemeId()).ok());
        EXPECT_EQ(manager.currentPaletteId(), builtinPaletteId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, builtinOverride);
        ASSERT_TRUE(manager.selectTheme(packageId).ok());
        EXPECT_EQ(manager.currentPaletteId(), packagePaletteId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, packageOverride);
        ASSERT_TRUE(writePackage(root.path(), QStringLiteral("custom"), packageId,
                                 QStringLiteral("#ff445566"),
                                 QStringLiteral("#ff556677")));
        ASSERT_TRUE(manager.reloadTheme().ok());
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, packageOverride);
        auto reset = manager.appearance();
        reset.colors.remove(packagePaletteId);
        ASSERT_TRUE(manager.applyAppearance(reset).ok());
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, QColor(QStringLiteral("#ff445566")));
        EXPECT_EQ(manager.appearance().colors.value(amberPaletteId).value(key), amberOverride);
        reset.paletteId = amberPaletteId;
        ASSERT_TRUE(manager.applyAppearance(reset).ok());
    }
    VNX::ThemeManager restored(root.path(), std::make_unique<VNX::QSettingsStorage>(storagePath));
    EXPECT_EQ(restored.currentThemeId(), packageId);
    EXPECT_EQ(restored.currentPaletteId(), amberPaletteId);
    EXPECT_EQ(restored.snapshot()->ui.colors.accent, amberOverride);
    EXPECT_FALSE(restored.appearance().colors.contains(packagePaletteId));
    ASSERT_TRUE(restored.selectTheme(VNX::builtinThemeId()).ok());
    EXPECT_EQ(restored.currentPaletteId(), builtinPaletteId);
    EXPECT_EQ(restored.snapshot()->ui.colors.accent, builtinOverride);
}

TEST(ThemeManager, MissingThemePreservesSharedPalettesUntilPackageReturns)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString packageId = QStringLiteral("theme.returning");
    const QString storagePath = QDir(root.path()).filePath(QStringLiteral("settings.ini"));
    const QString manifestPath = QDir(root.path()).filePath(
        QStringLiteral("returning/manifest.json"));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("returning"), packageId,
                             QStringLiteral("#ff112233"),
                             QStringLiteral("#ff334455")));
    const QColor packageOverride(QStringLiteral("#ff345678"));
    const QColor builtinOverride(QStringLiteral("#ff876543"));
    VNX::ColorPaletteCustomizations expected;
    expected[VNX::defaultPaletteId(packageId)].insert(QStringLiteral("accent.primary"), packageOverride);
    expected[VNX::defaultPaletteId(VNX::builtinThemeId())].insert(
        QStringLiteral("accent.primary"), builtinOverride);
    {
        VNX::ThemeManager manager(root.path(),
            std::make_unique<VNX::QSettingsStorage>(storagePath));
        auto draft = manager.appearance();
        draft.themeId = packageId;
        draft.colors = expected;
        ASSERT_TRUE(manager.applyAppearance(draft).ok());
    }
    ASSERT_TRUE(QFile::remove(manifestPath));
    {
        VNX::ThemeManager manager(root.path(),
            std::make_unique<VNX::QSettingsStorage>(storagePath));
        EXPECT_EQ(manager.currentThemeId(), VNX::builtinThemeId());
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, builtinOverride);
        EXPECT_EQ(manager.appearance().colors, expected);
        ASSERT_TRUE(manager.applyAppearance(manager.appearance()).ok());
    }
    VNX::ThemeManager restored(root.path(),
        std::make_unique<VNX::QSettingsStorage>(storagePath));
    EXPECT_EQ(restored.appearance().colors, expected);
    EXPECT_EQ(restored.snapshot()->ui.colors.accent, builtinOverride);
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("returning"), packageId,
                             QStringLiteral("#ffabcdef"),
                             QStringLiteral("#ff556677")));
    restored.refreshThemes();
    ASSERT_TRUE(restored.selectTheme(packageId).ok());
    EXPECT_EQ(restored.snapshot()->ui.colors.accent, packageOverride);
    EXPECT_EQ(restored.appearance().colors, expected);
}

TEST(ThemeManager, GlobalPalettesKeepIndependentEditsAcrossPreviewResetAndRestart)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString storagePath = QDir(root.path()).filePath(QStringLiteral("settings.ini"));
    const QString themeId = VNX::builtinThemeId();
    const QString defaultId = VNX::defaultPaletteId(themeId);
    const QString alternateId = VNX::builtinAmberPaletteId();
    const QColor defaultOverride(QStringLiteral("#80123456"));
    const QColor alternateOverride(QStringLiteral("#90765432"));
    const QString key = QStringLiteral("accent.primary");
    VNX::ColorPaletteCustomizations overrides;
    overrides[defaultId].insert(key, defaultOverride);
    overrides[alternateId].insert(key, alternateOverride);
    {
        auto storage = std::make_unique<VNX::QSettingsStorage>(storagePath);
        auto* stored = storage.get();
        VNX::ThemeManager manager(root.path(), std::move(storage));
        ASSERT_EQ(manager.availableThemes().size(), 1);
        QSet<QString> paletteIds;
        for (const auto& palette : manager.availablePalettes()) {
            EXPECT_FALSE(paletteIds.contains(palette.id));
            paletteIds.insert(palette.id);
            const auto loaded = manager.loadPalette(palette.id);
            ASSERT_TRUE(loaded.ok());
            for (const auto& role : VNX::themeColorRoles())
                EXPECT_TRUE(loaded.palette->colors.value(QString::fromLatin1(role.key)).isValid());
        }
        EXPECT_TRUE(paletteIds.contains(defaultId));
        EXPECT_TRUE(paletteIds.contains(alternateId));
        auto committed = manager.appearance();
        committed.colors = overrides;
        ASSERT_TRUE(manager.applyAppearance(committed).ok());
        const auto settings = storedValues(*stored);
        const auto original = manager.snapshot();
        EXPECT_EQ(original->ui.colors.accent, defaultOverride);

        auto preview = committed;
        preview.paletteId = alternateId;
        ASSERT_TRUE(manager.previewAppearance(preview).ok());
        const auto previewed = manager.snapshot();
        EXPECT_EQ(previewed->ui.colors.accent, alternateOverride);
        EXPECT_EQ(manager.appearance(), committed);
        EXPECT_EQ(storedValues(*stored), settings);
        manager.cancelAppearancePreview();
        EXPECT_EQ(manager.currentThemeId(), themeId);
        EXPECT_EQ(manager.currentPaletteId(), defaultId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, defaultOverride);
        EXPECT_EQ(previewed->ui.colors.accent, alternateOverride);
        ASSERT_TRUE(manager.applyAppearance(preview).ok());
    }
    {
        VNX::ThemeManager manager(root.path(), std::make_unique<VNX::QSettingsStorage>(storagePath));
        EXPECT_EQ(manager.currentThemeId(), themeId);
        EXPECT_EQ(manager.currentPaletteId(), alternateId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, alternateOverride);
        EXPECT_EQ(manager.appearance().colors, overrides);
        const auto base = manager.loadPalette(alternateId);
        ASSERT_TRUE(base.ok());
        auto reset = manager.appearance();
        reset.colors.remove(alternateId);
        ASSERT_TRUE(manager.applyAppearance(reset).ok());
        for (const auto& role : VNX::themeColorRoles()) {
            SCOPED_TRACE(role.key);
            EXPECT_EQ(role.read(*manager.snapshot()), base.palette->colors.value(QString::fromLatin1(role.key)));
        }
        EXPECT_EQ(manager.appearance().colors.value(defaultId), overrides.value(defaultId));
    }
    VNX::ThemeManager restored(root.path(), std::make_unique<VNX::QSettingsStorage>(storagePath));
    EXPECT_EQ(restored.currentThemeId(), themeId);
    EXPECT_EQ(restored.currentPaletteId(), alternateId);
    EXPECT_FALSE(restored.appearance().colors.contains(alternateId));
    const auto alternateBase = restored.loadPalette(alternateId);
    ASSERT_TRUE(alternateBase.ok());
    EXPECT_EQ(restored.snapshot()->ui.colors.accent, alternateBase.palette->colors.value(key));
    auto selectDefault = restored.appearance();
    selectDefault.paletteId = defaultId;
    ASSERT_TRUE(restored.applyAppearance(selectDefault).ok());
    EXPECT_EQ(restored.snapshot()->ui.colors.accent, defaultOverride);
}

TEST(ThemeManager, InvalidCustomizationsDoNotChangePublishedOrPersistedState)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    auto storage = std::make_unique<VNX::MemorySettingsStorage>();
    auto* stored = storage.get();
    VNX::ThemeManager manager(root.path(), std::move(storage));
    const auto committed = manager.appearance();
    const auto original = manager.snapshot();
    const auto settings = storedValues(*stored);

    QList<VNX::ThemeAppearance> invalid;
    auto unknown = committed;
    unknown.colors[unknown.paletteId].insert(
        QStringLiteral("unknown.color"), QColor(QStringLiteral("#123456")));
    invalid.append(unknown);
    auto invalidColor = committed;
    invalidColor.colors[invalidColor.paletteId].insert(
        QStringLiteral("accent.primary"), QColor());
    invalid.append(invalidColor);
    auto missingTheme = committed;
    missingTheme.themeId = QStringLiteral("theme.missing");
    invalid.append(missingTheme);
    auto missingPalette = committed;
    missingPalette.paletteId = QStringLiteral("palette.missing");
    invalid.append(missingPalette);
    auto invalidInactive = committed;
    invalidInactive.colors[QStringLiteral("theme.inactive")].insert(
        QStringLiteral("unknown.color"), QColor(QStringLiteral("#123456")));
    invalid.append(invalidInactive);

    for (const auto& draft : invalid)
    {
        EXPECT_FALSE(manager.previewAppearance(draft).ok());
        EXPECT_FALSE(manager.applyAppearance(draft).ok());
        EXPECT_EQ(manager.snapshot(), original);
        EXPECT_EQ(manager.appearance(), committed);
        EXPECT_EQ(storedValues(*stored), settings);
    }
}

TEST(ThemeManager, UnwritableCommitPreservesPreviewAndCommittedSettings)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    auto storage = std::make_unique<WritableSettingsStorage>();
    auto* stored = storage.get();
    VNX::ThemeManager manager(root.path(), std::move(storage));
    const auto committed = manager.appearance();
    const auto original = manager.snapshot();
    auto draft = committed;
    draft.colors[draft.paletteId].insert(
        QStringLiteral("accent.primary"), QColor(QStringLiteral("#ff123456")));
    ASSERT_TRUE(manager.previewAppearance(draft).ok());
    const auto preview = manager.snapshot();
    const auto settings = storedValues(*stored);

    stored->writable = false;
    const auto result = manager.applyAppearance(draft);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.first().code,
              VNX::ThemePackageErrorCode::PersistenceUnavailable);
    EXPECT_EQ(manager.snapshot(), preview);
    EXPECT_EQ(manager.appearance(), committed);
    EXPECT_EQ(storedValues(*stored), settings);
    manager.cancelAppearancePreview();
    EXPECT_EQ(manager.snapshot()->ui.colors.accent, original->ui.colors.accent);
}

TEST(ThemeManager, RejectsAppearanceChangesFromAnotherThread)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());
    const auto committed = manager.appearance();
    const auto original = manager.snapshot();
    auto draft = committed;
    draft.colors[draft.paletteId].insert(
        QStringLiteral("accent.primary"), QColor(QStringLiteral("#ff123456")));
    VNX::ThemeManagerOperationResult preview;
    VNX::ThemeManagerOperationResult applied;
    std::thread worker([&]() {
        preview = manager.previewAppearance(draft);
        applied = manager.applyAppearance(draft);
    });
    worker.join();

    EXPECT_FALSE(preview.ok());
    EXPECT_FALSE(applied.ok());
    EXPECT_EQ(manager.snapshot(), original);
    EXPECT_EQ(manager.appearance(), committed);
}

TEST(ThemeManager, EditableColorCatalogContainsUniqueSupportedSourceRoles)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());
    const auto base = manager.loadBaseTheme(VNX::builtinThemeId());
    ASSERT_TRUE(base.ok());
    QSet<QString> keys;
    const QColor probe(QStringLiteral("#cc123456"));
    int appearances = 0;
    int structures = 0;
    int fonts = 0;
    QObject::connect(&manager, &VNX::ThemeManager::appearanceChanged, &manager,
                     [&] { ++appearances; });
    QObject::connect(&manager, &VNX::ThemeManager::themeChanged, &manager,
                     [&] { ++structures; });
    QObject::connect(&VNX::FontManager::instance(), &VNX::FontManager::fontChanged,
                     &manager, [&] { ++fonts; });
    ASSERT_FALSE(VNX::themeColorRoles().isEmpty());
    for (const auto& role : VNX::themeColorRoles())
    {
        const QString key = QString::fromLatin1(role.key);
        SCOPED_TRACE(key.toStdString());
        EXPECT_FALSE(keys.contains(key));
        keys.insert(key);
        ASSERT_NE(role.read, nullptr);
        ASSERT_NE(role.write, nullptr);
        EXPECT_TRUE(role.read(*base.candidate->snapshot).isValid());
        EXPECT_FALSE(QString::fromLatin1(role.group).isEmpty());
        EXPECT_FALSE(QString::fromLatin1(role.label).isEmpty());
        EXPECT_FALSE(QString::fromLatin1(role.description).isEmpty());
        EXPECT_EQ(VNX::findThemeColorRole(key), &role);

        auto throughCatalog = *base.candidate->snapshot;
        role.write(throughCatalog, probe);
        VNX::resolveThemeColors(throughCatalog);
        EXPECT_EQ(role.read(throughCatalog), probe);

        auto throughPackage = *base.candidate->snapshot;
        ASSERT_TRUE(VNX::applyThemeColor(throughPackage, key, probe));
        VNX::resolveThemeColors(throughPackage);
        EXPECT_EQ(role.read(throughPackage), role.read(throughCatalog));

        auto preview = manager.appearance();
        preview.colors[preview.paletteId].insert(key, probe);
        ASSERT_TRUE(manager.previewAppearance(preview).ok());
        EXPECT_EQ(role.read(*manager.snapshot()), probe);
        manager.cancelAppearancePreview();
        EXPECT_EQ(role.read(*manager.snapshot()), role.read(*base.candidate->snapshot));
    }
    EXPECT_EQ(appearances, VNX::themeColorRoles().size() * 2);
    EXPECT_EQ(structures, 0);
    EXPECT_EQ(fonts, 0);

    auto alternate = manager.appearance();
    alternate.paletteId = VNX::builtinAmberPaletteId();
    ASSERT_TRUE(manager.previewAppearance(alternate).ok());
    ASSERT_TRUE(manager.applyAppearance(alternate).ok());
    EXPECT_EQ(structures, 0);
    EXPECT_EQ(fonts, 0);
    EXPECT_EQ(appearances, VNX::themeColorRoles().size() * 2 + 2);
    ASSERT_TRUE(manager.reloadTheme().ok());
    EXPECT_EQ(structures, 1);
    EXPECT_EQ(fonts, 0);
}

TEST(WidgetAppearance, CascadesStructuralSelectorsAndInvalidatesDescendants)
{
    QWidget root;
    root.setObjectName(QStringLiteral("surface"));
    root.setProperty("active", true);
    QWidget container(&root);
    QToolButton button(&container);
    button.setObjectName(QStringLiteral("action"));
    VNX::WidgetAppearance appearance;
    appearance.updateStyleSheet(QStringLiteral(R"qss(
        QWidget { color: #111111; background: #222222; }
        #surface[active="true"] QToolButton { color: #333333; }
        #surface > QToolButton { color: #444444; }
        #action { background: #555555; }
        QToolButton { background: #666666; }
        QToolButton { border: 1px solid #777777; }
        QToolButton { border-color: #888888; }
        .QWidget { selection-color: #999999; }
    )qss"));
    auto visual = appearance.visual(&button, QStyle::State_Enabled);
    EXPECT_EQ(visual.foreground, QColor(QStringLiteral("#333333")));
    EXPECT_EQ(visual.background, QColor(QStringLiteral("#555555")));
    EXPECT_EQ(visual.borderColors[0], QColor(QStringLiteral("#888888")));
    EXPECT_FALSE(visual.selectionForeground.isValid());
    EXPECT_EQ(appearance.visual(&container, QStyle::State_Enabled).selectionForeground,
              QColor(QStringLiteral("#999999")));

    root.setProperty("active", false);
    appearance.invalidate(&root);
    EXPECT_EQ(appearance.visual(&button, QStyle::State_Enabled).foreground,
              QColor(QStringLiteral("#111111")));

    button.setParent(&root);
    appearance.invalidate(&button);
    EXPECT_EQ(appearance.visual(&button, QStyle::State_Enabled).foreground,
              QColor(QStringLiteral("#444444")));
}

TEST(WidgetAppearance, CachedResolutionMatchesFreshResolutionAcrossWidgetLifecycles)
{
    QWidget root;
    root.setObjectName(QStringLiteral("surface"));
    root.setProperty("active", true);
    QWidget otherRoot;
    otherRoot.setObjectName(QStringLiteral("other"));
    QWidget container(&root);
    std::optional<QToolButton> button(std::in_place, &container);
    button->setObjectName(QStringLiteral("action"));
    const QString source = QStringLiteral(R"qss(
        QToolButton { color: #111111; background: #222222; border: 1px solid #333333; padding: 2px; }
        #surface[active="true"] QToolButton { color: #445566; }
        #renamed QToolButton { color: #778899; }
        #other QToolButton { background: #aabbcc; }
        #surface > QToolButton { border: 3px solid #6699aa; }
        QToolButton[autoRaise="true"] { border: 4px solid #abcdef; margin: 4px; }
        #action { background: #334455; }
    )qss");
    VNX::WidgetAppearance cached;
    cached.updateStyleSheet(source);
    const auto compare = [&](const char* stage) {
        SCOPED_TRACE(stage);
        const auto actual = cached.visual(&*button, QStyle::State_Enabled);
        VNX::WidgetAppearance fresh;
        fresh.updateStyleSheet(source);
        const auto expected = fresh.visual(&*button, QStyle::State_Enabled);
        EXPECT_EQ(actual.foreground, expected.foreground);
        EXPECT_EQ(actual.background, expected.background);
        EXPECT_EQ(actual.borderSpecified, expected.borderSpecified);
        EXPECT_EQ(actual.borders, expected.borders);
        EXPECT_EQ(actual.borderColors, expected.borderColors);
        EXPECT_EQ(actual.radii, expected.radii);
        EXPECT_EQ(actual.padding, expected.padding);
        EXPECT_EQ(actual.margin, expected.margin);
        EXPECT_EQ(actual.size, expected.size);
        EXPECT_EQ(actual.minimum, expected.minimum);
        EXPECT_EQ(actual.maximum, expected.maximum);
        EXPECT_EQ(actual.font, expected.font);
        return actual;
    };

    EXPECT_FALSE(button->testAttribute(Qt::WA_WState_Polished));
    EXPECT_EQ(compare("before polish").foreground, QColor(QStringLiteral("#445566")));
    button->ensurePolished();
    compare("after polish");

    root.setProperty("active", false);
    EXPECT_EQ(compare("ancestor property").foreground, QColor(QStringLiteral("#111111")));
    root.setObjectName(QStringLiteral("renamed"));
    EXPECT_EQ(compare("ancestor name").foreground, QColor(QStringLiteral("#778899")));
    root.setObjectName(QStringLiteral("surface"));
    root.setProperty("active", true);
    compare("restored ancestor selectors");

    container.setParent(&otherRoot);
    EXPECT_EQ(compare("reparented subtree").background, QColor(QStringLiteral("#aabbcc")));
    button->setParent(&root);
    EXPECT_EQ(compare("direct parent selector").borders, QMargins(3, 3, 3, 3));

    root.setStyleSheet(QStringLiteral("QToolButton { color: #aa1122; border: 2px solid #bb2233; }"));
    EXPECT_EQ(compare("new local scope").foreground, QColor(QStringLiteral("#aa1122")));
    root.setStyleSheet(QStringLiteral("#missing { color: #cc3344; }"));
    EXPECT_EQ(compare("local selector removed").foreground, QColor(QStringLiteral("#445566")));
    root.setStyleSheet(QStringLiteral("#action { color: #dd4455; padding: 6px; }"));
    EXPECT_EQ(compare("local selector replaced").padding, QMargins(6, 6, 6, 6));
    button->setStyleSheet(QStringLiteral("background: #ee5566;"));
    EXPECT_EQ(compare("nested local scope").background, QColor(QStringLiteral("#ee5566")));
    button->setStyleSheet(QString());
    root.setStyleSheet(QString());
    EXPECT_EQ(compare("local scopes cleared").foreground, QColor(QStringLiteral("#445566")));

    button->setAutoRaise(true);
    QEvent styleChanged(QEvent::StyleChange);
    QCoreApplication::sendEvent(&*button, &styleChanged);
    EXPECT_EQ(compare("native property after style change").margin, QMargins(4, 4, 4, 4));
    button->setAutoRaise(false);
    QCoreApplication::sendEvent(&*button, &styleChanged);
    EXPECT_EQ(compare("native property restored").margin, QMargins());

    button->setStyleSheet(QStringLiteral("QToolButton { color: #c0ffee; background: #decade; }"));
    compare("local scope before destruction");
    const auto* previousAddress = &*button;
    button.reset();
    button.emplace(&otherRoot);
    EXPECT_EQ(&*button, previousAddress);
    button->setObjectName(QStringLiteral("replacement"));
    const auto replacement = compare("new widget at reused address");
    EXPECT_EQ(replacement.foreground, QColor(QStringLiteral("#111111")));
    EXPECT_EQ(replacement.background, QColor(QStringLiteral("#aabbcc")));
}

TEST(WidgetAppearance, CachedSelectorsFollowPreviouslyUnmatchedInputsAndAncestorPaths)
{
    QWidget root;
    QWidget container(&root);
    QToolButton button(&container);
    const QString source = QStringLiteral(R"qss(
        QToolButton { color: #112233; }
        QWidget[scopeMode="active"] QToolButton[controlRole="action"] { color: #445566; }
        QWidget[windowTitle="Native scope"] QToolButton { color: #778899; }
        QWidget[armed="true"][mode="active"] QToolButton { selection-color: #abcdef; }
    )qss");
    VNX::WidgetAppearance cached;
    cached.updateStyleSheet(source);
    const auto check = [&](const char* stage, const QColor& expected) {
        SCOPED_TRACE(stage);
        const auto actual = cached.visual(&button, QStyle::State_Enabled);
        VNX::WidgetAppearance fresh;
        fresh.updateStyleSheet(source);
        const auto uncached = fresh.visual(&button, QStyle::State_Enabled);
        EXPECT_EQ(actual.foreground, uncached.foreground);
        EXPECT_EQ(actual.selectionForeground, uncached.selectionForeground);
        EXPECT_EQ(actual.foreground, expected);
        return actual;
    };

    check("unmatched inputs", QColor(QStringLiteral("#112233")));
    button.setProperty("controlRole", QStringLiteral("action"));
    check("matching target only", QColor(QStringLiteral("#112233")));
    container.setProperty("scopeMode", QStringLiteral("active"));
    check("new matching ancestor", QColor(QStringLiteral("#445566")));
    root.setProperty("scopeMode", QStringLiteral("active"));
    check("both ancestors match", QColor(QStringLiteral("#445566")));
    container.setProperty("scopeMode", QStringLiteral("inactive"));
    check("outer ancestor still matches", QColor(QStringLiteral("#445566")));
    root.setProperty("scopeMode", QVariant());
    check("matching ancestor removed", QColor(QStringLiteral("#112233")));
    container.setProperty("scopeMode", QStringLiteral("active"));
    button.setProperty("controlRole", QVariant());
    check("matching target removed", QColor(QStringLiteral("#112233")));

    root.setWindowTitle(QStringLiteral("Native scope"));
    QEvent styleChanged(QEvent::StyleChange);
    QCoreApplication::sendEvent(&root, &styleChanged);
    check("native ancestor property", QColor(QStringLiteral("#778899")));
    root.setWindowTitle(QString());
    QCoreApplication::sendEvent(&root, &styleChanged);
    check("native ancestor restored", QColor(QStringLiteral("#112233")));

    root.setProperty("mode", QStringLiteral("active"));
    EXPECT_FALSE(check("compound selector still unmatched", QColor(QStringLiteral("#112233")))
                     .selectionForeground.isValid());
    root.setProperty("armed", true);
    EXPECT_EQ(check("compound selector begins matching", QColor(QStringLiteral("#112233")))
                  .selectionForeground, QColor(QStringLiteral("#abcdef")));
    root.setProperty("mode", QVariant());
    EXPECT_FALSE(check("newly observed input removed", QColor(QStringLiteral("#112233")))
                     .selectionForeground.isValid());
    root.setProperty("armed", false);
    root.setProperty("mode", QStringLiteral("active"));
    EXPECT_FALSE(check("compound selector changes together", QColor(QStringLiteral("#112233")))
                     .selectionForeground.isValid());
    root.setProperty("armed", true);
    EXPECT_EQ(check("compound selector restored", QColor(QStringLiteral("#112233")))
                  .selectionForeground, QColor(QStringLiteral("#abcdef")));
}

TEST(WidgetAppearance, ReplacedGlobalAndManagedSelectorsTrackTheirNewInputs)
{
    QWidget root;
    QWidget container(&root);
    QToolButton button(&container);
    QString source = QStringLiteral("QToolButton { color: #112233; background: #223344; }");
    QString localSource = QStringLiteral(
        "QWidget[scopeMode=\"active\"] QToolButton { color: #445566; }");
    VNX::WidgetAppearance cached;
    cached.updateStyleSheet(source);
    cached.updateWidgetStyle(&root, localSource);
    const auto check = [&](const char* stage, const QColor& expected) {
        SCOPED_TRACE(stage);
        const auto actual = cached.visual(&button, QStyle::State_Enabled);
        VNX::WidgetAppearance fresh;
        fresh.updateStyleSheet(source);
        fresh.updateWidgetStyle(&root, localSource);
        const auto uncached = fresh.visual(&button, QStyle::State_Enabled);
        EXPECT_EQ(actual.foreground, uncached.foreground);
        EXPECT_EQ(actual.background, uncached.background);
        EXPECT_EQ(actual.foreground, expected);
    };

    check("unmatched managed selector", QColor(QStringLiteral("#112233")));
    root.setProperty("scopeMode", QStringLiteral("active"));
    check("managed ancestor input", QColor(QStringLiteral("#445566")));
    localSource = QStringLiteral(
        "QToolButton[replacementRole=\"selected\"] { color: #778899; }");
    cached.updateWidgetStyle(&root, localSource);
    check("managed selector replaced", QColor(QStringLiteral("#112233")));
    button.setProperty("replacementRole", QStringLiteral("selected"));
    check("new managed input", QColor(QStringLiteral("#778899")));
    button.setProperty("replacementRole", QVariant());
    check("new managed input removed", QColor(QStringLiteral("#112233")));
    localSource.clear();
    cached.updateWidgetStyle(&root, localSource);
    check("managed scope cleared", QColor(QStringLiteral("#112233")));

    source += QStringLiteral(
        "QWidget[replacementScope=\"active\"] QToolButton { color: #99aabb; }");
    cached.updateStyleSheet(source);
    check("global selector replaced", QColor(QStringLiteral("#112233")));
    container.setProperty("replacementScope", QStringLiteral("active"));
    check("new global input", QColor(QStringLiteral("#99aabb")));
    container.setProperty("replacementScope", QVariant());
    check("new global input removed", QColor(QStringLiteral("#112233")));
}

TEST(WidgetAppearance, ResolvesSubcontrolStatesWithoutLeakingWidgetPaint)
{
    QWidget widget;
    VNX::WidgetAppearance appearance;
    appearance.updateStyleSheet(QStringLiteral(R"qss(
        QWidget { background: #111111; color: #222222; }
        QWidget::handle { width: 12px; height: 20px; margin: 1px 2px 3px 4px; }
        QWidget::handle:horizontal { background: rgba(20, 30, 40, 1); }
        QWidget::handle:vertical { background: #333333; }
        QWidget::handle:horizontal:!selected { border: 2px solid rgba(50, 60, 70, 0.5); }
        QWidget::handle:horizontal:selected { background: transparent; image: none; }
        QWidget::handle:horizontal:disabled { color: #444444; }
    )qss"));
    auto visual = appearance.visual(&widget, QStyle::State_Enabled, u"handle",
                                     {QStringLiteral("horizontal")});
    EXPECT_EQ(visual.background, QColor(20, 30, 40));
    EXPECT_FALSE(visual.foreground.isValid());
    EXPECT_EQ(visual.borderColors[0], QColor(50, 60, 70, 127));
    EXPECT_EQ(visual.size, QSize(12, 20));
    EXPECT_EQ(visual.minimum, QSize(-1, -1));
    EXPECT_EQ(visual.margin, QMargins(4, 1, 2, 3));
    EXPECT_TRUE(visual.image.isNull());

    visual = appearance.visual(&widget, QStyle::State_Selected, u"handle",
                                {QStringLiteral("horizontal")});
    EXPECT_EQ(visual.background, QColor(Qt::transparent));
    EXPECT_EQ(visual.foreground, QColor(QStringLiteral("#444444")));
    EXPECT_EQ(visual.borders, QMargins());
    EXPECT_TRUE(visual.image.isEmpty());
    EXPECT_FALSE(visual.image.isNull());
    EXPECT_EQ(appearance.visual(&widget, QStyle::State_Enabled | QStyle::State_Horizontal,
                                u"handle", {QStringLiteral("vertical")}).background,
              QColor(QStringLiteral("#333333")));
}

TEST(WidgetAppearance, ColorChangesKeepNativeLayoutAndUpdateCachedRules)
{
    QWidget widget;
    widget.setObjectName(QStringLiteral("panel"));
    VNX::WidgetAppearance appearance;
    QString source = QStringLiteral(R"qss(
        #panel {
            background: #112233;
            border: 1px solid #445566;
            border-bottom: 3px solid #778899;
            border-radius: 8px;
            border-top-left-radius: 2px;
            padding: 3px 5px;
            min-width: 40px;
            max-height: 80px;
            text-align: right;
            font-size: 12pt;
        }
    )qss");
    appearance.updateStyleSheet(source);
    const auto before = appearance.visual(&widget, QStyle::State_Enabled);
    const auto native = appearance.nativeStyleSheet();
    EXPECT_FALSE(native.contains(QStringLiteral("#112233")));
    EXPECT_FALSE(native.contains(QStringLiteral("border-radius")));
    EXPECT_TRUE(native.contains(QStringLiteral("text-align: right")));
    EXPECT_TRUE(native.contains(QStringLiteral("font-size: 12pt")));
    EXPECT_TRUE(native.contains(QStringLiteral("border-bottom-width: 3px")));

    source.replace(QStringLiteral("#112233"), QStringLiteral("#abcdef"));
    source.replace(QStringLiteral("#445566"), QStringLiteral("#fedcba"));
    appearance.updateStyleSheet(source);
    const auto after = appearance.visual(&widget, QStyle::State_Enabled);
    EXPECT_EQ(appearance.nativeStyleSheet(), native);
    EXPECT_EQ(after.background, QColor(QStringLiteral("#abcdef")));
    EXPECT_EQ(after.borderColors[0], QColor(QStringLiteral("#fedcba")));
    EXPECT_EQ(after.borderColors[3], QColor(QStringLiteral("#778899")));
    EXPECT_EQ(after.borders, QMargins(1, 1, 1, 3));
    EXPECT_EQ(after.borders, before.borders);
    EXPECT_EQ(after.padding, QMargins(5, 3, 5, 3));
    EXPECT_EQ(after.radii, (std::array<QSizeF, 4>{QSizeF(2, 2), QSizeF(8, 8), QSizeF(8, 8), QSizeF(8, 8)}));
    EXPECT_EQ(after.minimum, QSize(40, -1));
    EXPECT_EQ(after.maximum, QSize(-1, 80));
    EXPECT_EQ(after.textAlignment, Qt::AlignRight | Qt::AlignVCenter);

    source.replace(QStringLiteral("border: 1px"), QStringLiteral("border: 4px"));
    appearance.updateStyleSheet(source);
    EXPECT_NE(appearance.nativeStyleSheet(), native);
    EXPECT_EQ(appearance.visual(&widget, QStyle::State_Enabled).borders, QMargins(4, 4, 4, 3));

    source.replace(QStringLiteral("#panel"), QStringLiteral("#otherPanel"));
    appearance.updateStyleSheet(source);
    EXPECT_FALSE(appearance.visual(&widget, QStyle::State_Enabled).background.isValid());
}

TEST(WidgetAppearance, LocalScopesOverrideGlobalRulesWithoutLosingUndeclaredColors)
{
    QWidget root;
    QWidget container(&root);
    QToolButton button(&container);
    button.setObjectName(QStringLiteral("action"));
    root.setStyleSheet(QStringLiteral("#action { color: #112233; background: #223344; font-family: 'Arial'; }"));
    container.setStyleSheet(QStringLiteral("QToolButton { color: #334455; font-weight: normal; }"));
    button.setStyleSheet(QStringLiteral("padding: 3px;"));
    VNX::WidgetAppearance appearance;
    appearance.updateStyleSheet(QStringLiteral(
        "#action { color: #445566; border: 2px solid #556677; }"
        "QHeaderView::section, QToolButton { font-size: 13px; font-weight: bold; padding: 1px;"
        "letter-spacing: 2px; text-transform: uppercase; }"));
    auto visual = appearance.visual(&button, QStyle::State_Enabled);
    EXPECT_EQ(visual.foreground, QColor(QStringLiteral("#334455")));
    EXPECT_EQ(visual.background, QColor(QStringLiteral("#223344")));
    EXPECT_EQ(visual.borderColors[0], QColor(QStringLiteral("#556677")));
    EXPECT_EQ(visual.padding, QMargins(3, 3, 3, 3));
    QFont inherited;
    inherited.setItalic(true);
    const auto resolved = visual.font.resolve(inherited);
    EXPECT_EQ(resolved.families(), QStringList{QStringLiteral("Arial")});
    EXPECT_EQ(resolved.pixelSize(), 13);
    EXPECT_EQ(resolved.weight(), QFont::Normal);
    EXPECT_EQ(resolved.letterSpacing(), 2);
    EXPECT_EQ(resolved.capitalization(), QFont::AllUppercase);
    EXPECT_TRUE(resolved.italic());
    const auto& native = appearance.nativeStyleSheet();
    const int section = native.indexOf(QStringLiteral("QHeaderView::section"));
    ASSERT_GE(section, 0);
    EXPECT_FALSE(native.mid(section, native.indexOf(QLatin1Char('}'), section) - section)
                     .contains(QStringLiteral("font-")));
    EXPECT_FALSE(native.mid(section, native.indexOf(QLatin1Char('}'), section) - section)
                     .contains(QStringLiteral("letter-spacing")));
    EXPECT_TRUE(native.contains(QStringLiteral("QToolButton {\n    font-size: 13px")));

    button.setStyleSheet(QStringLiteral("color: #667788;"));
    appearance.invalidate(&button);
    EXPECT_EQ(appearance.visual(&button, QStyle::State_Enabled).foreground,
              QColor(QStringLiteral("#667788")));
    appearance.updateStyleSheet(QStringLiteral("#action { color: #778899; border: 2px solid #8899aa; }"));
    visual = appearance.visual(&button, QStyle::State_Enabled);
    EXPECT_EQ(visual.foreground, QColor(QStringLiteral("#667788")));
    EXPECT_EQ(visual.borderColors[0], QColor(QStringLiteral("#8899aa")));

    button.setStyleSheet(QString());
    container.setStyleSheet(QStringLiteral("QToolButton { color: #99aabb; }"));
    appearance.invalidate(&container);
    EXPECT_EQ(appearance.visual(&button, QStyle::State_Enabled).foreground,
              QColor(QStringLiteral("#99aabb")));
}

TEST(WidgetAppearance, ManagedLocalColorsUpdateWithoutChangingNativeLayout)
{
    QWidget root;
    QToolButton button(&root);
    VNX::WidgetAppearance appearance;
    appearance.updateStyleSheet(QStringLiteral("QToolButton { color: #112233; background: #223344; }"));
    EXPECT_EQ(appearance.visual(&button, QStyle::State_Enabled).foreground,
              QColor(QStringLiteral("#112233")));
    EXPECT_TRUE(appearance.updateWidgetStyle(&root,
        QStringLiteral("QToolButton { color: #334455; }")).isEmpty());
    EXPECT_TRUE(root.styleSheet().isEmpty());
    EXPECT_EQ(appearance.visual(&button, QStyle::State_Enabled).foreground,
              QColor(QStringLiteral("#334455")));

    QString source = QStringLiteral("QToolButton { background: #445566; padding: 4px; font-size: 12px; }"
                                    "QToolButton:hover { background: #556677; }");
    const QString native = appearance.updateWidgetStyle(&button, source);
    button.setStyleSheet(native);
    appearance.invalidate(&button);
    EXPECT_EQ(appearance.visual(&button, QStyle::State_Enabled).background,
              QColor(QStringLiteral("#445566")));
    EXPECT_EQ(appearance.visual(&button, QStyle::State_Enabled | QStyle::State_MouseOver).background,
              QColor(QStringLiteral("#556677")));

    source.replace(QStringLiteral("#445566"), QStringLiteral("#667788"));
    EXPECT_EQ(appearance.updateWidgetStyle(&button, source), native);
    EXPECT_EQ(button.styleSheet(), native);
    const auto visual = appearance.visual(&button, QStyle::State_Enabled);
    EXPECT_EQ(visual.background, QColor(QStringLiteral("#667788")));
    EXPECT_EQ(visual.foreground, QColor(QStringLiteral("#334455")));
    EXPECT_EQ(visual.padding, QMargins(4, 4, 4, 4));
    EXPECT_EQ(visual.font.pixelSize(), 12);
}
