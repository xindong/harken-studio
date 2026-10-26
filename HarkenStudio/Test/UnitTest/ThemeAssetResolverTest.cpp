#include "Core/BuiltinTheme.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeAssetResolver.h"
#include "Core/ThemeManager.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

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

bool writeImage(const QString& path, const QColor& color)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage image(QSize(8, 8), QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image.save(path);
}

bool writePackage(const QString& themesRoot,
                  const QString& directoryName,
                  const QString& id,
                  const QByteArray& assets)
{
    const QString packageRoot = QDir(themesRoot).filePath(directoryName);
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":1,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"Test " + id.toUtf8()
        + "\",\"version\":\"1.0.0\""
        + ",\"compatibility\":{\"harkenVersion\":\">=0.1.0 <1.0.0\","
          "\"capabilities\":[\"curve-v1\",\"meter-v1\",\"waveform-v1\"]},"
          "\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\","
          "\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\"},"
          "\"assetMap\":\"assets.json\"}}";
    return writeFile(QDir(packageRoot).filePath(QStringLiteral("manifest.json")),
                     manifest)
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("tokens.json")),
                     R"json({"colors":{"accent.primary":"#ff00f0ff"}})json")
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/curve.json")),
                     R"json({"colors":{"curve.line":"#ff112233"}})json")
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/meter.json")),
                     R"json({"colors":{"meter.green":"#ff22aa44"}})json")
        && writeFile(QDir(packageRoot).filePath(
                         QStringLiteral("visuals/waveform.json")),
                     R"json({"colors":{"waveform.line":"#ff334455"}})json")
        && writeFile(QDir(packageRoot).filePath(QStringLiteral("assets.json")),
                     assets);
}

std::unique_ptr<VNX::ISettingsStorage> memoryStorage()
{
    return std::make_unique<VNX::MemorySettingsStorage>();
}

}

TEST(ThemeAssetResolver, ReloadSeparatesCacheAndKeepsOldHandleAlive)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"meter.peak.cap":{"type":"image","sources":[{"path":"cap.png"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("reload"),
                             QStringLiteral("theme.reload"), assets));
    const QString imagePath = QDir(root.path()).filePath(
        QStringLiteral("reload/cap.png"));
    ASSERT_TRUE(writeImage(imagePath, Qt::red));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.reload")).ok());
    const auto oldSnapshot = manager.snapshot();
    const auto oldResource = manager.assetResolver().resolveImage(
        oldSnapshot, QStringLiteral("meter.peak.cap")).resource;
    ASSERT_NE(oldResource, nullptr);
    ASSERT_TRUE(writeImage(imagePath, Qt::blue));

    ASSERT_TRUE(manager.reloadTheme().ok());
    const auto currentResource = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("meter.peak.cap")).resource;

    ASSERT_NE(currentResource, nullptr);
    EXPECT_NE(currentResource, oldResource);
    EXPECT_EQ(oldResource->image.pixelColor(0, 0), QColor(Qt::red));
    EXPECT_EQ(currentResource->image.pixelColor(0, 0), QColor(Qt::blue));
}

TEST(ThemeAssetResolver, RejectsRuntimePathEscape)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString packageRoot = QDir(root.path()).filePath(QStringLiteral("theme"));
    ASSERT_TRUE(QDir().mkpath(packageRoot));
    ASSERT_TRUE(writeImage(QDir(root.path()).filePath(
        QStringLiteral("outside.png")), Qt::red));
    VNX::ThemeSnapshot snapshotValue = VNX::createBuiltinDarkTheme();
    snapshotValue.generation = 42;
    const auto snapshot = std::make_shared<const VNX::ThemeSnapshot>(snapshotValue);
    VNX::ThemeAssetMapDto assets;
    VNX::ThemeAssetDto asset;
    asset.sources.append({QStringLiteral("theme.test/../outside.png"), 1.0});
    assets.assets.insert(QStringLiteral("audition.play"), asset);
    VNX::ThemeAssetResolver resolver;
    resolver.registerSnapshot(snapshot, assets,
                              {{QStringLiteral("theme.test"), packageRoot}});

    const auto result = resolver.resolveImage(
        snapshot, QStringLiteral("audition.play"),
        VNX::ThemeAssetVariant{QSize(20, 20)});

    ASSERT_FALSE(result.diagnostics.isEmpty());
    EXPECT_EQ(result.diagnostics.front().code,
              VNX::ThemePackageErrorCode::PathEscape);
}

TEST(ThemeAssetResolver, CreatesQtIconOnlyOnGuiThread)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    VNX::ThemeManager manager(root.path(), memoryStorage());

    const auto result = manager.assetResolver().resolveIcon(
        manager.snapshot(), QStringLiteral("audition.play"),
        VNX::ThemeAssetVariant{QSize(20, 20), 1.0, 0, Qt::cyan});

    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.icon.isNull());

    const auto snapshot = manager.snapshot();
    std::optional<VNX::ThemeIconResolution> workerResult;
    std::thread worker([&] {
        workerResult = manager.assetResolver().resolveIcon(
            snapshot, QStringLiteral("audition.play"),
            VNX::ThemeAssetVariant{QSize(20, 20), 1.0, 0, Qt::cyan});
    });
    worker.join();

    ASSERT_TRUE(workerResult.has_value());
    EXPECT_FALSE(workerResult->ok());
}
