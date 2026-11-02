#include "Integrations/OfficialUnitySource.h"
#include "Packaging/PackageReader.h"
#include "Support/ManifestTestUtils.h"
#include "Support/RangeHttpServer.h"
#include "Platform/PlatformPaths.h"
#include "ZipTestUtils.h"

#include <gtest/gtest.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

QString digest(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

class OfficialUnitySourceTest : public ::testing::Test
{
protected:
    QTemporaryDir directory;
    HarkenHubTest::RangeHttpServer server;
    HarkenHubTest::TestKeypair keys = HarkenHubTest::makeKeypair();
    VNH::OfficialUnitySourceRequest request;
    QJsonObject inventory;
    QJsonObject payload = HarkenHubTest::baseManifestPayload();
    QByteArray package = QByteArrayLiteral("{\"name\":\"com.harken.core\",\"version\":\"0.2.0\"}");
    QByteArray code = QByteArrayLiteral("official runtime bytes");

    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        ASSERT_TRUE(server.listen());
        const QString root = QStringLiteral("HarkenIntegration/Unity/Harken");
        inventory = QJsonObject{{QStringLiteral("product"), QStringLiteral("HarkenStudio")},
            {QStringLiteral("version"), QStringLiteral("1.4.0")},
            {QStringLiteral("buildId"), QStringLiteral("run12.abcd1234")},
            {QStringLiteral("channel"), QStringLiteral("stable")},
            {QStringLiteral("components"), QJsonArray{QJsonObject{
                {QStringLiteral("product"), QStringLiteral("HarkenIntegration")},
                {QStringLiteral("version"), QStringLiteral("0.2.0")},
                {QStringLiteral("artifacts"), QJsonObject{{QStringLiteral("unity"), root}}}}}},
            {QStringLiteral("files"), QJsonArray{
                QJsonObject{{QStringLiteral("path"), root + QStringLiteral("/package.json")},
                    {QStringLiteral("size"), package.size()}, {QStringLiteral("sha256"), digest(package)}},
                QJsonObject{{QStringLiteral("path"), root + QStringLiteral("/Runtime.bin")},
                    {QStringLiteral("size"), code.size()}, {QStringLiteral("sha256"), digest(code)}}}}};
        request.buildId = QStringLiteral("run12.abcd1234");
        request.packageVersion = QStringLiteral("0.2.0");
        request.localSource = directory.filePath(QStringLiteral("local"));
        request.cacheRoot = directory.filePath(QStringLiteral("cache"));
        request.baseUrl = server.baseUrl();
        request.release.version = QStringLiteral("1.4.0");
        request.release.objectPath = QStringLiteral("release/harken/%1/1.4.0/run12/studio.zip").arg(VNH::ServerEndpoints::hostChannel());
        ASSERT_TRUE(writeFile(QDir(request.localSource).filePath(QStringLiteral("package.json")), package));
        ASSERT_TRUE(writeFile(QDir(request.localSource).filePath(QStringLiteral("Runtime.bin")), code));
        publish();
    }

    void publish()
    {
        const QByteArray data = QJsonDocument(inventory).toJson(QJsonDocument::Compact);
        payload[QStringLiteral("packageManifest")] = QJsonObject{
            {QStringLiteral("fileName"), QStringLiteral("1.4.0-run12.abcd1234.files.json")},
            {QStringLiteral("size"), data.size()}, {QStringLiteral("sha256"), digest(data)}};
        request.release.updateManifest = HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(), keys);
        server.setPayload(data, QByteArrayLiteral("inventory"));
    }

    QByteArray publishArchive(bool includeSidecar)
    {
        const QByteArray binary = QByteArrayLiteral("studio fixture");
        auto files = inventory.value(QStringLiteral("files")).toArray();
        files.append(QJsonObject{{QStringLiteral("path"), VNH::studioBinaryRelativePath()},
            {QStringLiteral("size"), binary.size()}, {QStringLiteral("sha256"), digest(binary)}});
        inventory[QStringLiteral("files")] = files;
        const QByteArray data = QJsonDocument(inventory).toJson(QJsonDocument::Compact);
        const QByteArray capabilities = QJsonDocument(QJsonObject{
            {QStringLiteral("studioVersion"), QStringLiteral("1.4.0")},
            {QStringLiteral("buildId"), request.buildId}, {QStringLiteral("capabilitiesVersion"), 2}}).toJson();
        const QString path = directory.filePath(QStringLiteral("release.zip"));
        if (!HarkenHubTest::writeArchive(path, {
            {QByteArrayLiteral("HarkenIntegration/Unity/Harken/package.json"), package},
            {QByteArrayLiteral("HarkenIntegration/Unity/Harken/Runtime.bin"), code},
            {VNH::studioBinaryRelativePath().toUtf8(), binary},
            {QByteArrayLiteral("package-manifest.json"), data},
            {QByteArrayLiteral("capabilities.json"), capabilities}})) return {};
        QFile archive(path);
        if (!archive.open(QIODevice::ReadOnly)) return {};
        const auto bytes = archive.readAll();
        payload[QStringLiteral("packageSize")] = bytes.size();
        payload[QStringLiteral("sha256")] = digest(bytes);
        publish();
        if (includeSidecar) {
            const QString cache = QDir(request.cacheRoot).filePath(digest(bytes));
            if (!writeFile(QDir(cache).filePath(QStringLiteral("1.4.0-run12.abcd1234.files.json")), data)) return {};
        } else {
            payload.remove(QStringLiteral("packageManifest"));
            request.release.updateManifest = HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(), keys);
        }
        server.setPayload(bytes, QByteArrayLiteral("archive"));
        return bytes;
    }
};

TEST_F(OfficialUnitySourceTest, SignedInventoryReusesLocalBytesWithoutDownloadingTheArchive)
{
    ASSERT_TRUE(writeFile(QDir(request.localSource).filePath(QStringLiteral("custom.txt")), QByteArrayLiteral("custom")));
    const auto result = VNH::OfficialUnitySource::prepare(request, keys.publicKey);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.reusedLocalSource);
    EXPECT_EQ(VNH::PackageReader::fileSha256Hex(QDir(result.sourcePath).filePath(QStringLiteral("Runtime.bin"))), digest(code));
    EXPECT_FALSE(QFileInfo::exists(QDir(result.sourcePath).filePath(QStringLiteral("custom.txt"))));
    EXPECT_EQ(server.bodyBytesSent(), QJsonDocument(inventory).toJson(QJsonDocument::Compact).size());
    const int requests = server.requestCount();
    EXPECT_TRUE(VNH::OfficialUnitySource::prepare(request, keys.publicKey).ok);
    EXPECT_EQ(server.requestCount(), requests);
}

TEST_F(OfficialUnitySourceTest, AChangedInventoryCannotAuthorizeUnchangedLocalFiles)
{
    server.setPayload(QJsonDocument(inventory).toJson(QJsonDocument::Compact) + ' ', QByteArrayLiteral("changed"));
    const auto result = VNH::OfficialUnitySource::prepare(request, keys.publicKey);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.sourcePath.isEmpty());
}

TEST_F(OfficialUnitySourceTest, LegacyReleasesObtainTheirInventoryFromTheVerifiedArchive)
{
    const auto archive = publishArchive(false);
    ASSERT_FALSE(archive.isEmpty());
    const auto result = VNH::OfficialUnitySource::prepare(request, keys.publicKey);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.reusedLocalSource);
    EXPECT_EQ(server.bodyBytesSent(), archive.size());
}

TEST_F(OfficialUnitySourceTest, DamagedLocalSourceFallsBackToTheVerifiedArchive)
{
    const auto archive = publishArchive(true);
    ASSERT_FALSE(archive.isEmpty());
    ASSERT_TRUE(writeFile(QDir(request.localSource).filePath(QStringLiteral("Runtime.bin")), QByteArrayLiteral("broken")));
    const auto result = VNH::OfficialUnitySource::prepare(request, keys.publicKey);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_FALSE(result.reusedLocalSource);
    EXPECT_EQ(VNH::PackageReader::fileSha256Hex(QDir(result.sourcePath).filePath(QStringLiteral("Runtime.bin"))), digest(code));
    EXPECT_EQ(server.bodyBytesSent(), archive.size());
}

TEST_F(OfficialUnitySourceTest, CachedInventoriesAreReverifiedAndAdvertisedMissingInventoriesDoNotFallBack)
{
    ASSERT_TRUE(VNH::OfficialUnitySource::prepare(request, keys.publicKey).ok);
    const QString cached = QDir(request.cacheRoot).filePath(payload.value(QStringLiteral("sha256")).toString()
        + QStringLiteral("/1.4.0-run12.abcd1234.files.json"));
    ASSERT_TRUE(writeFile(cached, QByteArrayLiteral("tampered cache")));
    HarkenHubTest::RangeHttpServer::Script unavailable;
    unavailable.mode = HarkenHubTest::RangeHttpServer::Script::Mode::Status;
    unavailable.status = 404;
    server.setDefaultScript(unavailable);
    EXPECT_FALSE(VNH::OfficialUnitySource::prepare(request, keys.publicKey).ok);
    EXPECT_FALSE(QFileInfo::exists(QDir(request.cacheRoot).filePath(payload.value(QStringLiteral("sha256")).toString() + QStringLiteral("/package.zip"))));
}

TEST_F(OfficialUnitySourceTest, ReplacedLocalBytesAreNeverReturnedAsAnOfficialSource)
{
    ASSERT_TRUE(writeFile(QDir(request.localSource).filePath(QStringLiteral("Runtime.bin")), QByteArray(code.size(), 'x')));
    const auto result = VNH::OfficialUnitySource::prepare(request, keys.publicKey);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.reusedLocalSource);
}

TEST_F(OfficialUnitySourceTest, ExactBuildAndPackageIdentityAreRequired)
{
    request.buildId = QStringLiteral("different-build");
    EXPECT_FALSE(VNH::OfficialUnitySource::prepare(request, keys.publicKey).ok);
    EXPECT_EQ(server.requestCount(), 0);
    request.buildId = QStringLiteral("run12.abcd1234");
    request.packageVersion = QStringLiteral("0.3.0");
    EXPECT_FALSE(VNH::OfficialUnitySource::prepare(request, keys.publicKey).ok);
}

TEST_F(OfficialUnitySourceTest, SignatureStillProtectsHistoricalReleaseAndInventoryReference)
{
    payload[QStringLiteral("expiresAt")] = QStringLiteral("2020-01-01T00:00:00Z");
    publish();
    EXPECT_TRUE(VNH::OfficialUnitySource::prepare(request, keys.publicKey).ok);
    EXPECT_FALSE(VNH::OfficialUnitySource::prepare(request, HarkenHubTest::makeKeypair(0x23).publicKey).ok);
    auto reference = payload.value(QStringLiteral("packageManifest")).toObject();
    reference[QStringLiteral("sha256")] = QString(64, QLatin1Char('0'));
    payload[QStringLiteral("packageManifest")] = reference;
    auto envelope = QJsonDocument::fromJson(request.release.updateManifest).object();
    envelope[QStringLiteral("payloadBase64")] = QString::fromLatin1(QJsonDocument(payload).toJson().toBase64());
    request.release.updateManifest = QJsonDocument(envelope).toJson();
    EXPECT_FALSE(VNH::OfficialUnitySource::prepare(request, keys.publicKey).ok);
}

TEST_F(OfficialUnitySourceTest, MalformedAdvertisedInventoryDoesNotBecomeALegacyRelease)
{
    for (const auto& value : {QJsonValue(QJsonValue::Null), QJsonValue(QJsonObject{}), QJsonValue(QStringLiteral("missing"))}) {
        payload[QStringLiteral("packageManifest")] = value;
        const auto envelope = HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(), keys);
        EXPECT_FALSE(VNH::ManifestVerifier::verify(envelope, keys.publicKey, QDateTime::currentDateTimeUtc()).ok);
    }
    payload.remove(QStringLiteral("packageManifest"));
    const auto envelope = HarkenHubTest::signEnvelope(QJsonDocument(payload).toJson(), keys);
    EXPECT_TRUE(VNH::ManifestVerifier::verify(envelope, keys.publicKey, QDateTime::currentDateTimeUtc()).ok);
}

TEST_F(OfficialUnitySourceTest, InventoryRejectsTraversalAliasesAndFileDirectoryCollisions)
{
    const auto original = inventory.value(QStringLiteral("files")).toArray();
    for (const auto& path : {QStringLiteral("../outside"), QStringLiteral("C:/outside"),
                            QStringLiteral("folder/file:stream"), QStringLiteral("folder/NUL.txt"),
                            QStringLiteral("folder/file. "), QStringLiteral("folder\\file"),
                            original[0].toObject().value(QStringLiteral("path")).toString().toUpper(),
                            QStringLiteral("HarkenIntegration/Unity/Harken")}) {
        auto files = original;
        auto record = original[0].toObject();
        record[QStringLiteral("path")] = path;
        files.append(record);
        inventory[QStringLiteral("files")] = files;
        EXPECT_FALSE(VNH::PackageInventory::parse(QJsonDocument(inventory).toJson(),
            QStringLiteral("1.4.0"), request.buildId).ok) << path.toStdString();
    }
}

}
