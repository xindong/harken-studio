#include "Core/BuiltinTheme.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeAssetResolver.h"
#include "Core/ThemeManager.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
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
        + "\"schemaVersion\":7,\"id\":\"" + id.toUtf8()
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

    auto preview = manager.appearance();
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("accent.primary"), Qt::green);
    ASSERT_TRUE(manager.previewAppearance(preview).ok());
    EXPECT_EQ(manager.assetResolver().resolveImage(
                  manager.snapshot(), QStringLiteral("meter.peak.cap")).resource,
              oldResource);
    manager.cancelAppearancePreview();
    EXPECT_EQ(manager.assetResolver().resolveImage(
                  manager.snapshot(), QStringLiteral("meter.peak.cap")).resource,
              oldResource);
    ASSERT_TRUE(manager.applyAppearance(preview).ok());
    EXPECT_EQ(manager.assetResolver().resolveImage(
                  manager.snapshot(), QStringLiteral("meter.peak.cap")).resource,
              oldResource);

    ASSERT_TRUE(manager.reloadTheme().ok());
    const auto currentResource = manager.assetResolver().resolveImage(
        manager.snapshot(), QStringLiteral("meter.peak.cap")).resource;

    ASSERT_NE(currentResource, nullptr);
    EXPECT_NE(currentResource, oldResource);
    EXPECT_EQ(oldResource->image.pixelColor(0, 0), QColor(Qt::red));
    EXPECT_EQ(currentResource->image.pixelColor(0, 0), QColor(Qt::blue));
}

TEST(ThemeAssetResolver, RasterTintPreservesPixelCoverageAndDevicePixelRatio)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"meter.peak.cap":{"type":"image","sources":[{"path":"mask.png","scale":2}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("raster"),
                             QStringLiteral("theme.raster"), assets));
    QImage source(QSize(20, 20), QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::transparent);
    for (int y = 5; y < 15; ++y)
        for (int x = 3; x < 18; ++x)
            source.setPixelColor(x, y, QColor(255, 255, 255, (x + y) % 2 ? 96 : 255));
    ASSERT_TRUE(source.save(root.filePath(QStringLiteral("raster/mask.png"))));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.raster")).ok());
    const QColor tint(QStringLiteral("#307090"));
    for (const qreal ratio : {1.0, 2.0})
    {
        SCOPED_TRACE(ratio);
        const int extent = qRound(source.width() / ratio);
        const auto plain = manager.assetResolver().resolveImage(
            manager.snapshot(), QStringLiteral("meter.peak.cap"),
            VNX::ThemeAssetVariant{QSize(extent, extent), ratio});
        const auto colored = manager.assetResolver().resolveImage(
            manager.snapshot(), QStringLiteral("meter.peak.cap"),
            VNX::ThemeAssetVariant{QSize(extent, extent), ratio, 0, tint});
        ASSERT_TRUE(plain.ok());
        ASSERT_TRUE(colored.ok());
        const auto& before = plain.resource->image;
        const auto& after = colored.resource->image;
        ASSERT_EQ(after.size(), before.size());
        EXPECT_EQ(after.devicePixelRatio(), ratio);
        EXPECT_EQ(after.deviceIndependentSize(), before.deviceIndependentSize());
        EXPECT_EQ(after.pixelColor(3, 5), tint);
        for (int y = 0; y < before.height(); ++y)
            for (int x = 0; x < before.width(); ++x)
                EXPECT_EQ(after.pixelColor(x, y).alpha(), before.pixelColor(x, y).alpha());
    }
}

TEST(ThemeAssetResolver, SvgTintAlphaComposesWithAssetOpacityInImagesAndExportedSvg)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray assets = R"json({"assets":{"meter.peak.cap":{"type":"image","sources":[{"path":"mask.svg"}]}}})json";
    ASSERT_TRUE(writePackage(root.path(), QStringLiteral("svg"), QStringLiteral("theme.svg"), assets));
    ASSERT_TRUE(writeFile(root.filePath(QStringLiteral("svg/mask.svg")), R"svg(
        <svg xmlns="http://www.w3.org/2000/svg" width="24" height="8">
          <rect width="8" height="8" fill="currentColor"/>
          <rect x="8" width="8" height="8" fill="currentColor" fill-opacity="0.5"/>
          <rect x="16" width="8" height="8" fill="#ff0000"/>
        </svg>)svg"));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.svg")).ok());
    for (int alpha : {0, 64, 128, 255}) {
        const QColor tint(48, 112, 160, alpha);
        const auto resolved = manager.assetResolver().resolveImage(manager.snapshot(),
            QStringLiteral("meter.peak.cap"), VNX::ThemeAssetVariant{QSize(24, 8), 1.0, 0, tint});
        ASSERT_TRUE(resolved.ok());
        const auto& image = resolved.resource->image;
        EXPECT_NEAR(image.pixelColor(4, 4).alpha(), alpha, 1);
        EXPECT_NEAR(image.pixelColor(12, 4).alpha(), alpha * 0.5, 1);
        EXPECT_NEAR(image.pixelColor(20, 4).alpha(), alpha, 1);
        if (alpha > 0)
            EXPECT_EQ(image.pixelColor(20, 4).rgb(), QColor(Qt::red).rgb());
        QSvgRenderer renderer(resolved.resource->svgData);
        ASSERT_TRUE(renderer.isValid());
        QImage exported(image.size(), QImage::Format_ARGB32_Premultiplied);
        exported.fill(Qt::transparent);
        QPainter painter(&exported);
        renderer.render(&painter);
        painter.end();
        EXPECT_EQ(exported, image);
    }
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
