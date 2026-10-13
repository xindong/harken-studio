#include "Packaging/PackageVerifier.h"

#include "Packaging/PackageReader.h"
#include "Platform/PlatformPaths.h"
#include "ZipTestUtils.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using VNH::ExpectedPackage;
using VNH::PackageReader;
using VNH::PackageVerifier;
using HarkenHubTest::TestEntry;
using HarkenHubTest::writeArchive;

namespace {

QByteArray sha256Hex(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

struct PackageSpec
{
    QString version = QStringLiteral("1.4.0");
    QString buildId = QStringLiteral("run12.abcd1234");
    QString channel = QStringLiteral("stable");
    bool includeCapabilities = true;
    bool includeUnlistedFile = false;
    bool lieAboutSize = false;
    bool invalidHashFormat = false;
    QJsonObject capabilityOverrides;
    bool addLegacyFields = false;
    bool wrongProduct = false;
    QString binaryPath = VNH::studioBinaryRelativePath();

};

QString buildPackage(const QString& path, const PackageSpec& spec)
{
    const QByteArray binary = QByteArrayLiteral("stub-binary");
    const QByteArray asset = QByteArrayLiteral("payload-data");

    QJsonArray files;
    QJsonObject binaryRecord;
    binaryRecord[QStringLiteral("path")] = spec.binaryPath;
    binaryRecord[QStringLiteral("size")] =
        spec.lieAboutSize ? binary.size() + 7 : binary.size();
    binaryRecord[QStringLiteral("sha256")] =
        spec.invalidHashFormat ? QStringLiteral("not-a-hash")
                               : QString::fromLatin1(sha256Hex(binary));
    files.append(binaryRecord);
    QJsonObject assetRecord;
    assetRecord[QStringLiteral("path")] = QStringLiteral("data/asset.bin");
    assetRecord[QStringLiteral("size")] = asset.size();
    assetRecord[QStringLiteral("sha256")] = QString::fromLatin1(sha256Hex(asset));
    files.append(assetRecord);

    QJsonObject manifest;
    manifest[QStringLiteral("product")] = QStringLiteral("HarkenStudio");
    manifest[QStringLiteral("version")] = spec.version;
    manifest[QStringLiteral("buildId")] = spec.buildId;
    manifest[QStringLiteral("channel")] = spec.channel;
    manifest[QStringLiteral("files")] = files;

    QJsonObject capabilities;
    capabilities[QStringLiteral("studioVersion")] = spec.version;
    capabilities[QStringLiteral("buildId")] = spec.buildId;
    capabilities[QStringLiteral("capabilitiesVersion")] = 2;
    if (spec.addLegacyFields) {
        capabilities.remove(QStringLiteral("capabilitiesVersion"));
        capabilities[QStringLiteral("readableSchemaMin")] = 1;
        capabilities[QStringLiteral("readableSchemaMax")] = 2;
        capabilities[QStringLiteral("writableSchema")] = 2;
    }
    for (auto it = spec.capabilityOverrides.begin(); it != spec.capabilityOverrides.end(); ++it)
        capabilities.insert(it.key(), it.value());
    if (spec.wrongProduct)
        manifest[QStringLiteral("product")] = QStringLiteral("AnotherProduct");

    std::vector<TestEntry> entries = {
        {spec.binaryPath.toUtf8(), binary},
        {QByteArrayLiteral("data/asset.bin"), asset},
        {QByteArrayLiteral("package-manifest.json"),
         QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
    };
    if (spec.includeCapabilities)
        entries.push_back({QByteArrayLiteral("capabilities.json"),
                           QJsonDocument(capabilities).toJson(QJsonDocument::Compact)});
    if (spec.includeUnlistedFile)
        entries.push_back({QByteArrayLiteral("extra/unlisted.bin"),
                           QByteArrayLiteral("surprise")});

    if (!writeArchive(path, entries))
        return QString();
    return path;
}

ExpectedPackage expectedFor(const QString& archivePath, const PackageSpec& spec)
{
    ExpectedPackage expected;
    expected.buildId = spec.buildId;
    expected.semanticVersion = spec.version;
    expected.sha256Hex = PackageReader::fileSha256Hex(archivePath);
    expected.sizeBytes = QFileInfo(archivePath).size();
    return expected;
}

}

TEST(PackageVerifier, ValidPackagePasses)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    const auto result = PackageVerifier::verify(archive, expectedFor(archive, spec));
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.channel, QStringLiteral("stable"));
    QString error;
    EXPECT_TRUE(PackageVerifier::checkHostPlatform(archive, &error)) << error.toStdString();
}

TEST(PackageVerifier, ForeignPlatformIsRejectedBeforeManifestLookup)
{
    QTemporaryDir dir;
    PackageSpec spec;
#ifdef Q_OS_WIN
    spec.binaryPath = QStringLiteral("HarkenStudio.app/Contents/MacOS/HarkenStudio");
#else
    spec.binaryPath = QStringLiteral("HarkenStudio.exe");
#endif
    const auto archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());
    QString error;
    EXPECT_FALSE(PackageVerifier::checkHostPlatform(archive, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("macOS")));
    EXPECT_TRUE(error.contains(QStringLiteral("Windows")));
    const auto result = PackageVerifier::verify(archive, expectedFor(archive, spec));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, error);
}

TEST(PackageVerifier, RejectsMalformedProtocolAndIdentityMismatches)
{
    for (const QJsonObject& overrides : {
             QJsonObject{{QStringLiteral("capabilitiesVersion"), -1}},
             QJsonObject{{QStringLiteral("capabilitiesVersion"), QStringLiteral("2")}},
             QJsonObject{{QStringLiteral("capabilitiesVersion"), 2.5}},
             QJsonObject{{QStringLiteral("studioVersion"), QStringLiteral("9.0.0")}},
             QJsonObject{{QStringLiteral("buildId"), QStringLiteral("another-build")}}}) {
        QTemporaryDir dir;
        PackageSpec spec;
        spec.capabilityOverrides = overrides;
        const auto archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
        ASSERT_FALSE(archive.isEmpty());
        EXPECT_FALSE(PackageVerifier::verify(archive, expectedFor(archive, spec)).ok);
    }
}

TEST(PackageVerifier, AcceptsHistoricalAndNewerCapabilitiesWithoutRequiringNewHubFeatures)
{
    for (const int protocol : {0, 1, 2, 3}) {
        QTemporaryDir dir;
        PackageSpec spec;
        spec.addLegacyFields = protocol == 0;
        if (protocol > 0)
            spec.capabilityOverrides = {{QStringLiteral("capabilitiesVersion"), protocol},
                                       {QStringLiteral("projectSchema"), QJsonObject{}}};
        const auto archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
        ASSERT_FALSE(archive.isEmpty());
        const auto result = PackageVerifier::verify(archive, expectedFor(archive, spec));
        ASSERT_TRUE(result.ok) << result.error.toStdString();
        EXPECT_EQ(result.capabilitiesVersion, protocol);
    }
}

TEST(PackageVerifier, RejectsWrongProduct)
{
    QTemporaryDir dir;
    PackageSpec spec;
    spec.wrongProduct = true;
    const auto archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());
    EXPECT_FALSE(PackageVerifier::verify(archive, expectedFor(archive, spec)).ok);
}

TEST(PackageVerifier, IncompleteExpectedIdentityFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    auto noHash = expectedFor(archive, spec);
    noHash.sha256Hex.clear();
    EXPECT_FALSE(PackageVerifier::verify(archive, noHash).ok);

    auto noSize = expectedFor(archive, spec);
    noSize.sizeBytes = -1;
    EXPECT_FALSE(PackageVerifier::verify(archive, noSize).ok);
}

TEST(PackageVerifier, Sha256MismatchFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    auto expected = expectedFor(archive, spec);
    expected.sha256Hex = QString(64, QLatin1Char('0'));
    EXPECT_FALSE(PackageVerifier::verify(archive, expected).ok);
}

TEST(PackageVerifier, SizeMismatchFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    auto expected = expectedFor(archive, spec);
    expected.sizeBytes += 1;
    EXPECT_FALSE(PackageVerifier::verify(archive, expected).ok);
}

TEST(PackageVerifier, BuildIdMismatchFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    auto expected = expectedFor(archive, spec);
    expected.buildId = QStringLiteral("run99.ffff9999");
    EXPECT_FALSE(PackageVerifier::verify(archive, expected).ok);
}

TEST(PackageVerifier, MissingCapabilitiesFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    spec.includeCapabilities = false;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    EXPECT_FALSE(PackageVerifier::verify(archive, expectedFor(archive, spec)).ok);
}

TEST(PackageVerifier, ManifestSizeLieFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    spec.lieAboutSize = true;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    const auto result = PackageVerifier::verify(archive, expectedFor(archive, spec));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("size")));
}

TEST(PackageVerifier, InvalidManifestHashFormatFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    spec.invalidHashFormat = true;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    EXPECT_FALSE(PackageVerifier::verify(archive, expectedFor(archive, spec)).ok);
}

TEST(PackageVerifier, ResultCarriesPerFileHashesForExtraction)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    const auto result = PackageVerifier::verify(archive, expectedFor(archive, spec));
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_EQ(result.fileSha256ByPath.size(), 2);

    QString error;
    QByteArray manifestData;
    QByteArray capabilitiesData;
    ASSERT_TRUE(PackageReader::readEntry(archive, QStringLiteral("package-manifest.json"),
                                         &manifestData, &error));
    ASSERT_TRUE(PackageReader::readEntry(archive, QStringLiteral("capabilities.json"),
                                         &capabilitiesData, &error));

    QHash<QString, QString> verified = result.fileSha256ByPath;
    verified.insert(QStringLiteral("package-manifest.json"),
                    QString::fromLatin1(sha256Hex(manifestData)));
    verified.insert(QStringLiteral("capabilities.json"),
                    QString::fromLatin1(sha256Hex(capabilitiesData)));
    EXPECT_TRUE(PackageReader::extractAllVerified(
        archive, dir.filePath(QStringLiteral("outOk")), verified, &error))
        << error.toStdString();

    QHash<QString, QString> tampered = verified;
    tampered.insert(VNH::studioBinaryRelativePath(), QString(64, QLatin1Char('0')));
    EXPECT_FALSE(PackageReader::extractAllVerified(
        archive, dir.filePath(QStringLiteral("outTampered")), tampered, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("SHA-256")));
}

TEST(PackageVerifier, UnlistedPayloadFileFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PackageSpec spec;
    spec.includeUnlistedFile = true;
    const QString archive = buildPackage(dir.filePath(QStringLiteral("p.zip")), spec);
    ASSERT_FALSE(archive.isEmpty());

    const auto result = PackageVerifier::verify(archive, expectedFor(archive, spec));
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("unlisted")));
}
