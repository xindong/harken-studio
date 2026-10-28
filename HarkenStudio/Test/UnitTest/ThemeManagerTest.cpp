#include "Core/MemorySettingsStorage.h"
#include "Core/QSettingsStorage.h"
#include "Core/ThemeColorCatalog.h"
#include "Core/ThemeColorResolver.h"
#include "Core/ThemeManager.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <memory>
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
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("mutable"),
                             QStringLiteral("theme.mutable"),
                             QStringLiteral("#ff333333"),
                             QStringLiteral("#ff444444")));

    ASSERT_TRUE(manager.reloadTheme().ok());

    EXPECT_EQ(oldSnapshot->ui.colors.accent,
              QColor(QStringLiteral("#ff111111")));
    EXPECT_EQ(manager.snapshot()->ui.colors.accent,
              QColor(QStringLiteral("#ff333333")));
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
    const QColor accent(QStringLiteral("#ffbc1234"));
    const QColor text(QStringLiteral("#ffc2d3e4"));
    draft.colors[draft.themeId].insert(QStringLiteral("accent.primary"), accent);
    draft.colors[draft.themeId].insert(QStringLiteral("text.secondary"), text);
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

TEST(ThemeManager, PersistsOverridesPerThemeAndResetsToCurrentPackageDefaults)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString packageId = QStringLiteral("theme.custom");
    const QString storagePath = QDir(root.path()).filePath(QStringLiteral("settings.ini"));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("custom"), packageId,
                             QStringLiteral("#ff112233"),
                             QStringLiteral("#ff334455")));
    const QColor packageOverride(QStringLiteral("#f1234567"));
    const QColor builtinOverride(QStringLiteral("#ff765432"));
    {
        VNX::ThemeManager manager(root.path(),
            std::make_unique<VNX::QSettingsStorage>(storagePath));
        auto draft = manager.appearance();
        draft.themeId = packageId;
        draft.colors[packageId].insert(QStringLiteral("accent.primary"), packageOverride);
        draft.colors[VNX::builtinThemeId()].insert(
            QStringLiteral("accent.primary"), builtinOverride);
        ASSERT_TRUE(manager.applyAppearance(draft).ok());
    }
    {
        VNX::ThemeManager manager(root.path(),
            std::make_unique<VNX::QSettingsStorage>(storagePath));
        EXPECT_EQ(manager.currentThemeId(), packageId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, packageOverride);
        const auto base = manager.loadBaseTheme(packageId);
        ASSERT_TRUE(base.ok());
        EXPECT_EQ(base.candidate->snapshot->ui.colors.accent,
                  QColor(QStringLiteral("#ff112233")));
        ASSERT_TRUE(manager.selectTheme(VNX::builtinThemeId()).ok());
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, builtinOverride);
        ASSERT_TRUE(manager.selectTheme(packageId).ok());
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, packageOverride);

        ASSERT_TRUE(writePackage(root.path(), QStringLiteral("custom"), packageId,
                                 QStringLiteral("#ff445566"),
                                 QStringLiteral("#ff556677")));
        ASSERT_TRUE(manager.reloadTheme().ok());
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, packageOverride);
        auto reset = manager.appearance();
        reset.colors.remove(packageId);
        ASSERT_TRUE(manager.applyAppearance(reset).ok());
        EXPECT_EQ(manager.snapshot()->ui.colors.accent,
                  QColor(QStringLiteral("#ff445566")));
    }
    VNX::ThemeManager restored(root.path(),
        std::make_unique<VNX::QSettingsStorage>(storagePath));
    EXPECT_EQ(restored.currentThemeId(), packageId);
    EXPECT_EQ(restored.snapshot()->ui.colors.accent,
              QColor(QStringLiteral("#ff445566")));
    EXPECT_FALSE(restored.appearance().colors.contains(packageId));
    ASSERT_TRUE(restored.selectTheme(VNX::builtinThemeId()).ok());
    EXPECT_EQ(restored.snapshot()->ui.colors.accent, builtinOverride);
}

TEST(ThemeManager, MissingThemePreservesCustomizationsUntilPackageReturns)
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
    VNX::ThemeColorCustomizations expected;
    expected[packageId].insert(QStringLiteral("accent.primary"), packageOverride);
    expected[VNX::builtinThemeId()].insert(
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

TEST(ThemeManager, BuiltinPresetsKeepIndependentOverridesAcrossPreviewResetAndRestart)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString storagePath = QDir(root.path()).filePath(QStringLiteral("settings.ini"));
    const QString defaultId = VNX::builtinThemeId();
    QString alternateId;
    for (const auto& theme : VNX::discoverThemePackages(root.path()).themes) {
        if (theme.builtin && theme.id != defaultId) {
            alternateId = theme.id;
            break;
        }
    }
    ASSERT_FALSE(alternateId.isEmpty());
    const QColor defaultOverride(QStringLiteral("#80123456"));
    const QColor alternateOverride(QStringLiteral("#90765432"));
    const QString key = QStringLiteral("accent.primary");
    VNX::ThemeColorCustomizations overrides;
    overrides[defaultId].insert(key, defaultOverride);
    overrides[alternateId].insert(key, alternateOverride);
    {
        auto storage = std::make_unique<VNX::QSettingsStorage>(storagePath);
        auto* stored = storage.get();
        VNX::ThemeManager manager(root.path(), std::move(storage));
        auto committed = manager.appearance();
        committed.colors = overrides;
        ASSERT_TRUE(manager.applyAppearance(committed).ok());
        const auto settings = storedValues(*stored);
        const auto original = manager.snapshot();
        EXPECT_EQ(original->ui.colors.accent, defaultOverride);

        auto preview = committed;
        preview.themeId = alternateId;
        ASSERT_TRUE(manager.previewAppearance(preview).ok());
        const auto previewed = manager.snapshot();
        EXPECT_EQ(previewed->ui.colors.accent, alternateOverride);
        EXPECT_EQ(manager.appearance(), committed);
        EXPECT_EQ(storedValues(*stored), settings);
        manager.cancelAppearancePreview();
        EXPECT_EQ(manager.currentThemeId(), defaultId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, defaultOverride);
        EXPECT_EQ(previewed->ui.colors.accent, alternateOverride);
        ASSERT_TRUE(manager.applyAppearance(preview).ok());
    }
    {
        VNX::ThemeManager manager(root.path(),
            std::make_unique<VNX::QSettingsStorage>(storagePath));
        EXPECT_EQ(manager.currentThemeId(), alternateId);
        EXPECT_EQ(manager.snapshot()->ui.colors.accent, alternateOverride);
        EXPECT_EQ(manager.appearance().colors, overrides);
        const auto base = manager.loadBaseTheme(alternateId);
        ASSERT_TRUE(base.ok());
        auto reset = manager.appearance();
        reset.colors.remove(alternateId);
        ASSERT_TRUE(manager.applyAppearance(reset).ok());
        for (const auto& role : VNX::themeColorRoles()) {
            SCOPED_TRACE(role.key);
            EXPECT_EQ(role.read(*manager.snapshot()), role.read(*base.candidate->snapshot));
        }
        EXPECT_EQ(manager.appearance().colors.value(defaultId), overrides.value(defaultId));
    }
    VNX::ThemeManager restored(root.path(),
        std::make_unique<VNX::QSettingsStorage>(storagePath));
    EXPECT_EQ(restored.currentThemeId(), alternateId);
    EXPECT_FALSE(restored.appearance().colors.contains(alternateId));
    const auto alternateBase = restored.loadBaseTheme(alternateId);
    ASSERT_TRUE(alternateBase.ok());
    EXPECT_EQ(restored.snapshot()->ui.colors.accent,
              alternateBase.candidate->snapshot->ui.colors.accent);
    ASSERT_TRUE(restored.selectTheme(defaultId).ok());
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
    unknown.colors[unknown.themeId].insert(
        QStringLiteral("unknown.color"), QColor(QStringLiteral("#123456")));
    invalid.append(unknown);
    auto invalidColor = committed;
    invalidColor.colors[invalidColor.themeId].insert(
        QStringLiteral("accent.primary"), QColor());
    invalid.append(invalidColor);
    auto missingTheme = committed;
    missingTheme.themeId = QStringLiteral("theme.missing");
    invalid.append(missingTheme);
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
    draft.colors[draft.themeId].insert(
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
    draft.colors[draft.themeId].insert(
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
    }
}
