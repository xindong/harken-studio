#include "Integrations/OfficialUnitySource.h"

#include "Download/Downloader.h"
#include "Integrations/IntegrationProgress.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Packaging/PackageReader.h"
#include "Packaging/PackageVerifier.h"
#include "Platform/PlatformPaths.h"
#include "Transactions/InstallPackageCache.h"
#include "Update/ServerEndpoints.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTimer>

namespace VNH {
namespace {

bool matchesFile(const QString& path, qint64 size, const QString& hash)
{
    IntegrationProgress::checkpoint();
    QFile file(path);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    return QFileInfo(path).isFile() && file.size() == size && file.open(QIODevice::ReadOnly)
        && IntegrationProgress::addFileData(file, digest)
        && QString::fromLatin1(digest.result().toHex()).compare(hash, Qt::CaseInsensitive) == 0;
}

bool obtain(const QList<QUrl>& urls, const QString& path, qint64 size,
            const QString& hash, QString* error)
{
    for (const auto& suffix : {QString(), QStringLiteral(".part"), QStringLiteral(".part.meta"), QStringLiteral(".stale")})
        if (!VNX::UnityInstallation::validatePath(path + suffix, error)) return false;
    if (matchesFile(path, size, hash)) return true;
    if (urls.isEmpty()) {
        *error = QStringLiteral("The exact release has no approved download location");
        return false;
    }
    Downloader downloader;
    QEventLoop loop;
    DownloadResult result;
    QObject::connect(&downloader, &Downloader::finished, &loop, [&](const auto& value) {
        result = value;
        loop.quit();
    });
    QTimer cancel;
    QObject::connect(&cancel, &QTimer::timeout, &loop, [&]() {
        if (IntegrationProgress::cancelled()) downloader.cancel();
    });
    cancel.start(100);
    DownloadRequest download;
    download.url = urls.front();
    download.fallbackUrls = urls.mid(1);
    download.targetPath = path;
    download.expectedSizeBytes = size;
    download.expectedSha256 = hash;
    download.maxAttempts = 2;
    if (!downloader.start(download, error)) return false;
    loop.exec();
    IntegrationProgress::checkpoint();
    if (!result.ok) {
        *error = result.error;
        return false;
    }
    if (!matchesFile(path, size, hash)) {
        *error = QStringLiteral("Downloaded release content failed verification");
        return false;
    }
    return true;
}

bool copyUnity(const QString& source, const QString& target, const PackageInventory& inventory,
               QString* error)
{
    if (!VNX::UnityInstallation::validateTree(source, error)) return false;
    const QString prefix = inventory.unityRoot + QLatin1Char('/');
    for (auto it = inventory.files.cbegin(); it != inventory.files.cend(); ++it) {
        if (!it.key().startsWith(prefix)) continue;
        const QString relative = it.key().mid(prefix.size());
        const QString from = QDir(source).filePath(relative);
        const QString to = QDir(target).filePath(relative);
        IntegrationProgress::inspectedEntry();
        if (!matchesFile(from, it->size, it->sha256)
            || !QDir().mkpath(QFileInfo(to).absolutePath()) || !QFile::copy(from, to)
            || !matchesFile(to, it->size, it->sha256)) {
            *error = QStringLiteral("Integration Package source does not match the official file manifest: %1").arg(relative);
            return false;
        }
    }
    QFile package(QDir(target).filePath(QStringLiteral("package.json")));
    if (!package.open(QIODevice::ReadOnly) || package.size() > 1024 * 1024) {
        *error = QStringLiteral("The official Unity component identity is inconsistent");
        return false;
    }
    const auto identity = QJsonDocument::fromJson(package.readAll()).object();
    if (identity.value(QStringLiteral("name")).toString() != QStringLiteral("com.harken.core")
        || identity.value(QStringLiteral("version")).toString() != inventory.unityVersion) {
        *error = QStringLiteral("The official Unity component identity is inconsistent");
        return false;
    }
    return true;
}

}

OfficialUnitySourceResult OfficialUnitySource::prepare(const OfficialUnitySourceRequest& request,
                                                       const QByteArray& publicKey)
{
    OfficialUnitySourceResult result;
    if (request.release.updateManifest.isEmpty()) {
        if (!ManifestVerifier::isSafeFileToken(request.buildId)) {
            result.error = QStringLiteral("The original release build could not be identified. Select the original release through Change Integration Package.");
            return result;
        }
        ServerVersionCatalog catalog;
        QEventLoop loop;
        ServerVersionList list;
        QObject::connect(&catalog, &ServerVersionCatalog::finished, &loop, [&](const auto& value) { list = value; loop.quit(); });
        QTimer cancel;
        QObject::connect(&cancel, &QTimer::timeout, &loop, [&]() { if (IntegrationProgress::cancelled()) loop.quit(); });
        cancel.start(100);
        const auto base = request.baseUrl.isEmpty() ? ServerEndpoints::resolvedBaseUrl() : request.baseUrl;
        if (!catalog.fetch(ServerEndpoints::manifestUrls(base, QStringLiteral("harken"), ServerEndpoints::hostChannel()),
                           QStringLiteral("harken"), ServerEndpoints::hostChannel())) {
            result.error = QStringLiteral("The official release catalog could not be requested");
            return result;
        }
        loop.exec();
        IntegrationProgress::checkpoint();
        if (!list.ok) { result.error = list.error; return result; }
        OfficialUnitySourceRequest resolved = request;
        QString packageHash;
        for (const auto& entry : list.entries) {
            const auto candidate = ManifestVerifier::verify(entry.updateManifest, publicKey, QDateTime::currentDateTimeUtc(), true);
            if (!candidate.ok || candidate.manifest.product != QStringLiteral("HarkenStudio") || candidate.manifest.buildId != request.buildId) continue;
            if (!packageHash.isEmpty() && packageHash != candidate.manifest.sha256Hex) {
                result.error = QStringLiteral("The release catalog contains conflicting packages for this build");
                return result;
            }
            resolved.release = entry;
            packageHash = candidate.manifest.sha256Hex;
        }
        if (packageHash.isEmpty()) {
            result.error = QStringLiteral("The exact installed build is not available in the official release catalog. No newer build was selected.");
            return result;
        }
        return prepare(resolved, publicKey);
    }
    const auto verified = ManifestVerifier::verify(request.release.updateManifest, publicKey,
                                                   QDateTime::currentDateTimeUtc(), true);
    if (!verified.ok) {
        result.error = verified.error;
        return result;
    }
    result.release = verified.manifest;
    const auto& release = result.release;
    if (request.buildId.isEmpty() || release.buildId != request.buildId
        || release.product != QStringLiteral("HarkenStudio") || request.release.version != release.version
        || !ServerEndpoints::isReleaseObjectPath(request.release.objectPath, QStringLiteral("harken"),
                                                ServerEndpoints::hostChannel(), release.version)) {
        result.error = QStringLiteral("The official release does not match the requested build");
        return result;
    }
    const QString cache = QDir(request.cacheRoot.isEmpty()
        ? userDataDir() + QStringLiteral("/integration-packages") : request.cacheRoot)
        .filePath(release.sha256Hex.toLower());
    if (!VNX::UnityInstallation::validatePath(cache, &result.error) || !QDir().mkpath(cache)
        || !VNX::UnityInstallation::validateTree(cache, &result.error)) {
        if (result.error.isEmpty()) result.error = QStringLiteral("Cannot create a verified Integration Package staging directory");
        return result;
    }
    QLockFile lock(QDir(cache).filePath(QStringLiteral("source.lock")));
    if (!lock.tryLock()) {
        result.error = QStringLiteral("This release is already being prepared");
        return result;
    }
    const auto baseUrl = request.baseUrl.isEmpty() ? ServerEndpoints::resolvedBaseUrl() : request.baseUrl;
    const auto urls = ServerEndpoints::downloadUrls(baseUrl, request.release.objectPath, request.release.releaseTag);
    const ExpectedPackage expected{release.buildId, release.version, release.sha256Hex, release.packageSize};
    QString archivePath = InstallPackageCache::packagePath(release.buildId);
    bool archiveVerified = false;
    PackageVerifyResult archive;
    const auto obtainArchive = [&]() {
        if (archiveVerified) return true;
        if (!VNX::UnityInstallation::validatePath(archivePath, &result.error)
            || !matchesFile(archivePath, release.packageSize, release.sha256Hex)) {
            archivePath = QDir(cache).filePath(QStringLiteral("package.zip"));
            if (!obtain(urls, archivePath, release.packageSize, release.sha256Hex, &result.error)) return false;
        }
        archive = PackageVerifier::verify(archivePath, expected);
        archiveVerified = archive.ok;
        if (!archive.ok) result.error = archive.error;
        return archive.ok;
    };
    QByteArray data;
    if (!release.packageManifest.fileName.isEmpty()) {
        const auto& reference = release.packageManifest;
        const QString objectPath = request.release.objectPath.left(request.release.objectPath.lastIndexOf(QLatin1Char('/')) + 1)
            + reference.fileName;
        const auto manifestUrls = ServerEndpoints::downloadUrls(baseUrl, objectPath, request.release.releaseTag);
        const QString manifestPath = QDir(cache).filePath(reference.fileName);
        if (!obtain(manifestUrls, manifestPath, reference.size, reference.sha256Hex, &result.error)) return result;
        QFile manifest(manifestPath);
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size() != reference.size) {
            result.error = QStringLiteral("Cannot read the verified package file manifest");
            return result;
        }
        data = manifest.read(PackageInventory::maximumBytes + 1);
        if (data.size() != reference.size || QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != reference.sha256Hex.toLatin1()) {
            result.error = QStringLiteral("Package file manifest changed after verification");
            return result;
        }
    } else {
        if (!obtainArchive() || !PackageReader::readEntry(archivePath, QStringLiteral("package-manifest.json"), &data, &result.error)) return result;
    }
    result.inventory = PackageInventory::parse(data, release.version, release.buildId, release.channel);
    result.inventoryHash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    if (!result.inventory.ok || result.inventory.unityRoot.isEmpty()
        || request.packageVersion.isEmpty() || request.packageVersion != result.inventory.unityVersion) {
        result.error = result.inventory.ok ? QStringLiteral("The release does not identify the installed Unity Integration Package version") : result.inventory.error;
        return result;
    }
    result.staging = std::make_shared<QTemporaryDir>(QDir(cache).filePath(QStringLiteral("source-XXXXXX")));
    if (!result.staging->isValid()) {
        result.error = QStringLiteral("Cannot create a verified Integration Package staging directory");
        return result;
    }
    result.sourcePath = result.staging->filePath(QStringLiteral("local"));
    if (!request.localSource.isEmpty() && copyUnity(request.localSource, result.sourcePath, result.inventory, &result.error)) {
        result.reusedLocalSource = true;
    } else {
        if (!obtainArchive()) return result;
        QByteArray embedded;
        if (!PackageReader::readEntry(archivePath, QStringLiteral("package-manifest.json"), &embedded, &result.error)) return result;
        if (embedded != data) {
            result.error = QStringLiteral("The release archive and signed file manifest disagree");
            return result;
        }
        const QString extracted = result.staging->filePath(QStringLiteral("archive"));
        auto extractionHashes = archive.fileSha256ByPath;
        extractionHashes.insert(QStringLiteral("package-manifest.json"), result.inventoryHash);
        QByteArray capabilities;
        if (!PackageReader::readEntry(archivePath, QStringLiteral("capabilities.json"), &capabilities, &result.error)) return result;
        extractionHashes.insert(QStringLiteral("capabilities.json"),
            QString::fromLatin1(QCryptographicHash::hash(capabilities, QCryptographicHash::Sha256).toHex()));
        if (!PackageReader::extractAllVerified(archivePath, extracted, extractionHashes, &result.error)) return result;
        result.sourcePath = result.staging->filePath(QStringLiteral("official"));
        if (!copyUnity(QDir(extracted).filePath(result.inventory.unityRoot), result.sourcePath, result.inventory, &result.error)) return result;
    }
    result.error.clear();
    result.ok = true;
    return result;
}

}
