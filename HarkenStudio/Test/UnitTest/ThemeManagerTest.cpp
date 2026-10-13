#include "Core/MemorySettingsStorage.h"
#include "Core/QSettingsStorage.h"
#include "Core/QtAdsThemeAdapter.h"
#include "Core/ThemeManager.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QStyleHints>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void expectPlatformColorScheme(Qt::ColorScheme expected)
{
    if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
        return;
    EXPECT_EQ(qApp->styleHints()->colorScheme(), expected);
}

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
                  const QString& curveLine,
                  const std::optional<QString>& colorScheme = std::nullopt)
{
    const QString packageRoot = QDir(themesRoot).filePath(directoryName);
    const QByteArray scheme = colorScheme
        ? QByteArray(",\"colorScheme\":\"") + colorScheme->toUtf8() + '"'
        : QByteArray();
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":1,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"Test " + id.toUtf8()
        + "\",\"version\":\"1.0.0\"" + scheme + ","
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

QStringList themeIds(const QList<VNX::ThemePackageDescriptor>& themes)
{
    QStringList ids;
    for (const auto& theme : themes)
        ids.append(theme.id);
    return ids;
}

}

TEST(ThemeManager, DiscoversBuiltinAndUserThemesByStableId)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("zeta"),
                             QStringLiteral("theme.zeta"),
                             QStringLiteral("#ff112233"),
                             QStringLiteral("#ff223344")));
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("alpha"),
                             QStringLiteral("theme.alpha"),
                             QStringLiteral("#ff445566"),
                             QStringLiteral("#ff556677")));

    VNX::ThemeManager manager(root.path(), memoryStorage());

    EXPECT_EQ(themeIds(manager.availableThemes()),
              QStringList({VNX::builtinThemeId(),
                           QStringLiteral("theme.alpha"),
                           QStringLiteral("theme.zeta")}));
    EXPECT_TRUE(manager.availableThemes().front().builtin);
}

TEST(ThemeManager, PublishesOneGenerationAndUpdatesApplicationSurfaces)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("violet"),
                             QStringLiteral("theme.violet"),
                             QStringLiteral("#ff8844cc"),
                             QStringLiteral("#ff123456")));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    const auto before = manager.snapshot();
    int notifications = 0;
    VNX::ThemeSnapshotPtr observed;
    bool applicationUpdatedBeforeSignal = false;
    QObject::connect(&manager, &VNX::ThemeManager::themeChanged,
                     &manager, [&](VNX::ThemeSnapshotPtr snapshot) {
        ++notifications;
        observed = std::move(snapshot);
        applicationUpdatedBeforeSignal =
            qApp->palette().color(QPalette::Highlight)
                == QColor(QStringLiteral("#ff8844cc"))
            && qApp->styleSheet().contains(QStringLiteral("#8844CC"));
    });

    const auto result = manager.selectTheme(QStringLiteral("theme.violet"));

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.published);
    ASSERT_NE(observed, nullptr);
    EXPECT_EQ(notifications, 1);
    EXPECT_EQ(observed, manager.snapshot());
    EXPECT_GT(observed->generation, before->generation);
    EXPECT_EQ(observed->domains.curve.colors.attenuationVolume,
              QColor(QStringLiteral("#ff123456")));
    EXPECT_TRUE(applicationUpdatedBeforeSignal);
    EXPECT_TRUE(VNX::QtAdsThemeAdapter(observed).styleSheet().contains(
        QStringLiteral("#8844CC")));
}

TEST(ThemeManager, PublishesAndRestoresPlatformColorScheme)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("light"),
                             QStringLiteral("theme.light"),
                             QStringLiteral("#ff8844cc"),
                             QStringLiteral("#ff123456"),
                             QStringLiteral("light")));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    expectPlatformColorScheme(Qt::ColorScheme::Dark);

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.light")).ok());

    EXPECT_EQ(manager.snapshot()->colorScheme, VNX::ThemeColorScheme::Light);
    expectPlatformColorScheme(Qt::ColorScheme::Light);
    EXPECT_TRUE(qApp->styleSheet().contains(QStringLiteral("#ff8844cc")));

    ASSERT_TRUE(manager.selectTheme(VNX::builtinThemeId()).ok());

    EXPECT_EQ(manager.snapshot()->colorScheme, VNX::ThemeColorScheme::Dark);
    expectPlatformColorScheme(Qt::ColorScheme::Dark);
}

TEST(ThemeManager, RepeatedSelectionIsNoOpAndReloadPublishesOnce)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("one"),
                             QStringLiteral("theme.one"),
                             QStringLiteral("#ff102030"),
                             QStringLiteral("#ff203040")));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    int notifications = 0;
    QObject::connect(&manager, &VNX::ThemeManager::themeChanged,
                     &manager, [&]() { ++notifications; });
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.one")).ok());
    const auto selected = manager.snapshot();

    const auto repeated = manager.selectTheme(QStringLiteral("theme.one"));
    EXPECT_TRUE(repeated.ok());
    EXPECT_FALSE(repeated.published);
    EXPECT_EQ(manager.snapshot(), selected);
    EXPECT_EQ(notifications, 1);

    const auto reloaded = manager.reloadTheme();
    EXPECT_TRUE(reloaded.ok());
    EXPECT_TRUE(reloaded.published);
    EXPECT_NE(manager.snapshot(), selected);
    EXPECT_GT(manager.snapshot()->generation, selected->generation);
    EXPECT_EQ(notifications, 2);
}

TEST(ThemeManager, FailedSelectionKeepsSnapshotSelectionAndNotificationCount)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());
    const auto before = manager.snapshot();
    int notifications = 0;
    QObject::connect(&manager, &VNX::ThemeManager::themeChanged,
                     &manager, [&]() { ++notifications; });

    const auto result = manager.selectTheme(QStringLiteral("theme.missing"));

    ASSERT_FALSE(result.ok());
    ASSERT_FALSE(result.diagnostics.isEmpty());
    EXPECT_FALSE(result.published);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::FileNotFound);
    EXPECT_EQ(manager.snapshot(), before);
    EXPECT_EQ(manager.currentThemeId(), VNX::builtinThemeId());
    EXPECT_EQ(notifications, 0);
}

TEST(ThemeManager, RestoresPersistedStableIdAtStartup)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("saved"),
                             QStringLiteral("theme.saved"),
                             QStringLiteral("#ffabcdef"),
                             QStringLiteral("#ff102938")));
    const QString settingsPath = QDir(root.path()).filePath(
        QStringLiteral("settings.ini"));
    {
        VNX::ThemeManager manager(
            root.path(), std::make_unique<VNX::QSettingsStorage>(settingsPath));
        ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.saved")).ok());
    }

    VNX::ThemeManager restored(
        root.path(), std::make_unique<VNX::QSettingsStorage>(settingsPath));

    EXPECT_EQ(restored.currentThemeId(), QStringLiteral("theme.saved"));
    EXPECT_EQ(restored.snapshot()->ui.colors.accent,
              QColor(QStringLiteral("#ffabcdef")));
}

TEST(ThemeManager, MissingPersistedThemeRecoversAndPersistsBuiltin)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString settingsPath = QDir(root.path()).filePath(
        QStringLiteral("settings.ini"));
    {
        VNX::QSettingsStorage storage(settingsPath);
        storage.beginGroup(QStringLiteral("Appearance"));
        storage.setValue(QStringLiteral("ThemeId"),
                         QStringLiteral("theme.removed"));
        storage.endGroup();
        storage.sync();
    }

    VNX::ThemeManager manager(
        root.path(), std::make_unique<VNX::QSettingsStorage>(settingsPath));

    EXPECT_EQ(manager.currentThemeId(), VNX::builtinThemeId());
    EXPECT_FALSE(manager.lastDiagnostics().isEmpty());
    VNX::QSettingsStorage persisted(settingsPath);
    persisted.beginGroup(QStringLiteral("Appearance"));
    EXPECT_EQ(persisted.value(QStringLiteral("ThemeId")).toString(),
              VNX::builtinThemeId());
    persisted.endGroup();
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
    const auto before = manager.snapshot();
    VNX::ThemeManagerOperationResult result;

    std::thread worker([&]() { result = manager.reloadTheme(); });
    worker.join();

    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::RuntimeThreadViolation);
    EXPECT_EQ(manager.snapshot(), before);
}
