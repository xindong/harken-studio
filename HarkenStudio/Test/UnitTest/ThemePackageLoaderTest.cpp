#include "Core/ThemePackageLoader.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <filesystem>

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

}

TEST(ThemePackageLoader, RejectsSymbolicLinkEscapeWhenSupported)
{
    QTemporaryDir root;
    QTemporaryDir outside;
    const QString packageRoot = QDir(root.path()).filePath(QStringLiteral("linked"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("manifest.json")),
        R"json({
            "schemaVersion":1,"id":"theme.linked","displayName":"Test theme.linked","version":"1.0.0",
            "compatibility":{"harkenVersion":">=0.1.0 <1.0.0","capabilities":["curve-v1","meter-v1","waveform-v1"]},
            "entries":{"tokens":"tokens.json","visuals":{"curve":"visuals/curve.json","meter":"visuals/meter.json","waveform":"visuals/waveform.json"},"assetMap":"assets.json"}
        })json"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("tokens.json")),
        R"json({"colors":{"accent.primary":"#ff00f0ff"},"metrics":{"spacing.medium":12}})json"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/curve.json")),
        R"json({"colors":{"curve.line":"#ff112233"},"metrics":{"curve.lineWidth":2}})json"));
    ASSERT_TRUE(writeFile(QDir(packageRoot).filePath(QStringLiteral("visuals/meter.json")),
        R"json({"colors":{"meter.green":"#ff22aa44","meter.spectrogram.grid":"#ff102030"},"metrics":{"meter.spectrogram.plotInsetLeft":42}})json"));
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
