#include "Cli/HubCli.h"

#include "Download/Downloader.h"
#include "App/SingleInstanceGuard.h"
#include "Projects/ProjectStore.h"
#include "Packaging/PackageVerifier.h"
#include "Packaging/PackageReader.h"
#include "Platform/Elevation.h"
#include "Platform/PlatformPaths.h"
#include "ReleaseNotes/VersionReleaseNotes.h"
#include "Security/ManifestVerifier.h"
#include "Security/TrustAnchors.h"
#include "Transactions/HandoffArea.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/InstallRequest.h"
#include "Transactions/RepairExecutor.h"
#include "Transactions/TransactionDiagnostics.h"
#include "Update/HubSelfUpdate.h"
#include "Update/BridgeDispatcher.h"
#include "Update/ServerEndpoints.h"
#include "Update/ServerVersionCatalog.h"
#include "Update/UpdateChecker.h"
#include "Update/UpdateCoordinator.h"
#include "Update/UpdateStateStore.h"
#include "Versions/IntegrityChecker.h"
#include "Versions/LocalBuildRegistry.h"
#include "Versions/VersionScanner.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTimer>
#include <algorithm>

namespace VNH::Cli {
namespace {

QJsonObject manifestJson(const ReleaseManifest& manifest)
{
    return {{"product", manifest.product}, {"version", manifest.version}, {"buildId", manifest.buildId},
        {"channel", manifest.channel}, {"sha256", manifest.sha256Hex}, {"size", manifest.packageSize},
        {"releaseSequence", manifest.releaseSequence}, {"expiresAt", manifest.expiresAt.toString(Qt::ISODate)}};
}

QJsonObject stateJson(const UpdateStateRecord& state)
{
    return {{"found", state.found}, {"product", state.product}, {"state", state.state}, {"version", state.version},
        {"buildId", state.buildId}, {"package", state.downloadPath}, {"manifest", state.manifestEnvelopePath},
        {"transactionId", state.transactionId}, {"failureCode", state.failureCode}, {"failureDetail", state.failureDetail}};
}

QJsonObject versionJson(const InstalledVersion& version)
{
    return {{"buildId", version.buildId}, {"version", version.semanticVersion}, {"channel", version.channel},
        {"path", version.installPath}, {"executable", version.executablePath}, {"localBuild", version.isLocalBuild()},
        {"available", version.available}, {"error", version.availabilityError}};
}

QJsonObject packageReview(const QString& path, const ReleaseManifest& manifest)
{
    const auto verified = PackageVerifier::verify(path,
        {manifest.buildId, manifest.version, manifest.sha256Hex, manifest.packageSize});
    require(verified.ok, verified.error);
    require(verified.channel == manifest.channel, "Package channel does not match its signed release");
    QJsonObject files;
    for (auto it = verified.fileSha256ByPath.cbegin(); it != verified.fileSha256ByPath.cend(); ++it)
        files[it.key()] = it.value();
    for (const QString& name : verified.metadataEntries) {
        QByteArray bytes;
        QString error;
        require(PackageReader::readEntry(path, name, &bytes, &error), error);
        files[name] = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    }
    const QString base = QDir(studioVersionsRoot()).filePath(QStringLiteral("%1-%2-%3-%4")
        .arg(manifest.version, manifest.channel, manifest.platform.toLower(), manifest.architecture.toLower()));
    const auto index = InstallIndex::snapshot();
    require(index.ok, index.error);
    const auto occupied = [&](const QString& path) {
        const QFileInfo candidate(path);
        return candidate.exists() || candidate.isSymbolicLink() || candidate.isJunction()
            || std::any_of(index.records.begin(), index.records.end(), [&](const auto& record) {
                return QFileInfo(record.installPath).absoluteFilePath().compare(candidate.absoluteFilePath(), Qt::CaseInsensitive) == 0;
            });
    };
    QString destination = base;
    for (int suffix = 2; occupied(destination); ++suffix) destination = base + QStringLiteral("-%1").arg(suffix);
    return {{"destination", destination}, {"files", files}, {"index", fileHash(InstallIndex::filePath())}};
}

QJsonArray affectedProjects(const QString& build)
{
    ProjectStore store;
    requireStore(store, store.open(userDataDir() + "/projects.db"));
    QJsonArray result;
    for (const auto& project : store.listProjects())
        if (project.preferredStudioBuildId == build || project.lastKnownGoodStudioBuildId == build)
            result.append(QJsonObject{{"path", project.canonicalPath}, {"preferredBuild", project.preferredStudioBuildId},
                {"lastKnownGoodBuild", project.lastKnownGoodStudioBuildId}});
    requireStore(store, store.lastError().isEmpty());
    return result;
}

ServerVersionList serverVersions()
{
    ServerVersionCatalog catalog;
    QEventLoop loop;
    ServerVersionList result;
    QObject::connect(&catalog, &ServerVersionCatalog::finished, &loop, [&](const auto& response) { result = response; loop.quit(); });
    require(catalog.fetch(ServerEndpoints::manifestUrls(ServerEndpoints::resolvedBaseUrl(), "harken", ServerEndpoints::hostChannel()), "harken", ServerEndpoints::hostChannel()), "Could not start server version lookup");
    loop.exec();
    require(result.ok, result.error);
    return result;
}

void waitForElevated(const ElevationLaunchResult& launch, const QString& transaction = {})
{
    require(launch.ok, launch.error, launch.canceled ? "ElevationDeclined" : "ElevationFailed");
    QEventLoop loop;
    QTimer poll;
    int exitCode = 1;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (pollElevatedInstall(launch.processHandle, &exitCode)) loop.quit();
    });
    poll.start(250);
    loop.exec();
    releaseElevatedInstall(launch.processHandle);
    require(exitCode == 0, transaction.isEmpty() ? QStringLiteral("Elevated operation failed with exit code %1").arg(exitCode)
        : TransactionDiagnostics::failureMessage(transaction, exitCode));
}

QJsonObject download(const ReleaseManifest& manifest, const QByteArray& envelope, const QString& objectPath, const QString& releaseTag)
{
    require(ManifestVerifier::isSafeFileToken(manifest.buildId), "Invalid build id in manifest");
    const QString fileName = QFileInfo(QUrl(objectPath).path()).fileName();
    require(ManifestVerifier::isSafeFileToken(fileName), "Invalid download file name");
    const QString root = userDataDir() + "/downloads/cli/" + manifest.product + '/' + manifest.buildId;
    require(QDir().mkpath(root), "Could not create download directory");
    QString error;
    const QString envelopePath = root + '/' + fileName + ".manifest";
    auto lease = DownloadLease::acquire(envelopePath, &error);
    require(bool(lease), error);
    QSaveFile manifestFile(envelopePath);
    require(manifestFile.open(QIODevice::WriteOnly) && manifestFile.write(envelope) == envelope.size() && manifestFile.commit(), "Could not save the signed manifest");
    DownloadRequest request;
    auto urls = ServerEndpoints::downloadUrls(ServerEndpoints::resolvedBaseUrl(), objectPath, releaseTag);
    require(!urls.isEmpty(), "No valid package download URLs");
    request.url = urls.takeFirst();
    request.fallbackUrls = urls;
    request.targetPath = root + '/' + fileName;
    request.expectedSizeBytes = manifest.packageSize;
    request.expectedSha256 = manifest.sha256Hex;
    request.lease = lease;
    Downloader downloader;
    DownloadResult result;
    QEventLoop loop;
    QObject::connect(&downloader, &Downloader::finished, &loop, [&](const auto& response) { result = response; loop.quit(); });
    const bool started = downloader.start(request, &error);
    require(started, error.isEmpty() ? QStringLiteral("The download request could not be started") : error);
    loop.exec();
    require(result.ok, result.error);
    if (manifest.product == "HarkenStudio") {
        const auto verified = PackageVerifier::verify(result.filePath,
            {manifest.buildId, manifest.version, manifest.sha256Hex, manifest.packageSize});
        require(verified.ok, verified.error);
        require(verified.channel == manifest.channel, "Package channel does not match the signed manifest");
    } else {
        require(HubSelfUpdate::verifyInstaller(result.filePath, manifest.sha256Hex, manifest.packageSize, &error), error);
    }
    return {{"package", result.filePath}, {"manifest", envelopePath}, {"release", manifestJson(manifest)}};
}

UpdateCheckResult checkUpdate(const QString& product)
{
    UpdateChecker checker;
    UpdateChecker::Options options;
    auto urls = ServerEndpoints::manifestUrls(ServerEndpoints::resolvedBaseUrl(), product, ServerEndpoints::hostChannel());
    require(!urls.isEmpty(), "No valid update manifest URLs");
    options.latestUrl = urls.takeFirst();
    options.fallbackUrls = urls;
    options.trustedHttpOrigin = ServerEndpoints::resolvedBaseUrl();
    options.product = product;
    options.channel = ServerEndpoints::hostChannel();
    options.manifestPublicKey = TrustAnchors::manifestPublicKey();
    QEventLoop loop;
    UpdateCheckResult result;
    QObject::connect(&checker, &UpdateChecker::checkFinished, &loop, [&](const auto& response) { result = response; loop.quit(); });
    require(checker.start(options), "Could not start update lookup");
    loop.exec();
    checker.stop();
    require(result.ok, result.error);
    return result;
}

QJsonObject installPackage(const QString& package, const QString& manifest, bool allowExpired, const QJsonObject& review,
                           const QString& manifestHash)
{
    const auto preparation = InstallRequest::prepare(package, manifest, TrustAnchors::manifestPublicKey(), QDateTime::currentDateTimeUtc(), allowExpired);
    require(preparation.ok, preparation.error);
    const auto launch = launchElevatedInstall(preparation.transactionId, true, review["destination"].toString(), review["index"].toString(), manifestHash);
    if (!launch.ok) HandoffArea::discard(preparation.transactionId);
    waitForElevated(launch, preparation.transactionId);
    return {{"buildId", preparation.buildId}, {"version", preparation.version}, {"transactionId", preparation.transactionId}};
}

}

Reply versions(const Arguments& args)
{
    const auto now = QDateTime::currentDateTimeUtc();
    const auto key = TrustAnchors::manifestPublicKey();
    if (args.operation == "server" || args.operation == "download") {
        const auto catalog = serverVersions();
        QJsonArray entries;
        for (const auto& entry : catalog.entries) {
            const auto verified = ManifestVerifier::verify(entry.updateManifest, key, now, true);
            const bool trusted = verified.ok && verified.manifest.product == "HarkenStudio"
                && verified.manifest.version == entry.version;
            auto row = trusted ? manifestJson(verified.manifest) : QJsonObject{};
            row["version"] = entry.version;
            row["verified"] = trusted;
            row["error"] = verified.ok && !trusted ? "Catalog identity does not match its signed release" : verified.error;
            row["releaseNotes"] = entry.changelog;
            entries.append(row);
            if (args.operation == "download" && trusted && verified.manifest.buildId == args.required("build")) {
                require(ServerEndpoints::isReleaseObjectPath(entry.objectPath, "harken", ServerEndpoints::hostChannel(), verified.manifest.version), "Invalid server package path");
                require(verified.manifest.product == "HarkenStudio", "Unexpected package product");
                return success(download(verified.manifest, entry.updateManifest, entry.objectPath, entry.releaseTag));
            }
        }
        require(args.operation == "server", "No verified server release matches the build id");
        return success({{"versions", entries}});
    }
    if (args.operation == "list") {
        QJsonArray entries;
        const auto index = InstallIndex::snapshot();
        require(index.ok, index.error);
        QString error;
        LocalBuildRegistry::load(&error);
        require(error.isEmpty(), error);
        for (const auto& version : VersionScanner::scanLocalBuilds()) entries.append(versionJson(version));
        for (const auto& record : index.records) {
            InstalledVersion version;
            QString issue;
            const bool available = VersionScanner::inspect(record, &version, &issue) == InstallationIssue::None
                && record.healthState == "Installed";
            auto row = versionJson(version);
            row["buildId"] = record.buildId;
            row["version"] = record.semanticVersion;
            row["path"] = record.installPath;
            row["available"] = available;
            row["healthState"] = record.healthState;
            row["error"] = issue;
            entries.append(row);
        }
        return success({{"versions", entries}});
    }
    if (args.operation == "add-local" || args.operation == "refresh-local" || args.operation == "remove-local") {
        const QString path = QFileInfo(args.required("path")).absoluteFilePath();
        QString error;
        if (args.operation == "remove-local") {
            confirm(args, {{"executable", path}, {"registry", fileHash(LocalBuildRegistry::filePath())}, {"deleteFiles", false}});
            require(LocalBuildRegistry::remove(path, &error), error);
        } else {
            LocalBuildRecord record;
            require(LocalBuildRegistry::probe(path, &record, &error), error);
            require(args.operation == "add-local" ? LocalBuildRegistry::upsert(record, &error) : LocalBuildRegistry::updateExisting(record, &error), error);
        }
        return success({{"path", path}, {"buildId", LocalBuildRegistry::stableIdForPath(path)}});
    }
    if (args.operation == "install") {
        const QString package = QFileInfo(args.required("package")).absoluteFilePath();
        const QString manifest = args.value("manifest", InstallRequest::manifestPathForPackage(package));
        const bool allowExpired = args.flag("allow-expired");
        const auto envelope = readFile(manifest);
        const auto manifestHash = QString::fromLatin1(QCryptographicHash::hash(envelope, QCryptographicHash::Sha256).toHex());
        const auto verified = ManifestVerifier::verify(envelope, key, now, allowExpired);
        require(verified.ok, verified.error);
        require(verified.manifest.product == "HarkenStudio", "Use updates apply for a Hub update");
        const auto hash = fileHash(package);
        require(hash.compare(verified.manifest.sha256Hex, Qt::CaseInsensitive) == 0, "Package hash does not match the signed manifest");
        const auto installation = packageReview(package, verified.manifest);
        confirm(args, {{"release", manifestJson(verified.manifest)}, {"package", package}, {"sha256", hash},
            {"manifestSha256", manifestHash}, {"installation", installation}});
        requireConsent("studio");
        return success(installPackage(package, manifest, allowExpired, installation, manifestHash));
    }
    const QString build = args.required("build");
    const auto available = VersionScanner::scanAvailable();
    const auto version = std::find_if(available.begin(), available.end(), [&](const auto& value) { return value.buildId == build; });
    if (args.operation == "verify" || args.operation == "repair" || args.operation == "uninstall") {
        const auto index = InstallIndex::snapshot();
        require(index.ok, index.error);
        const auto record = std::find_if(index.records.begin(), index.records.end(), [&](const auto& value) { return value.buildId == build; });
        require(record != index.records.end(), "The build is not a managed installation");
        const auto integrity = IntegrityChecker::check(record->installPath, record->packageManifestHash);
        if (args.operation == "verify")
            return {integrity.ok ? 0 : 3, {{"ok", integrity.ok}, {"data", QJsonObject{{"error", integrity.error},
                {"missing", QJsonArray::fromStringList(integrity.missingFiles)}, {"modified", QJsonArray::fromStringList(integrity.mismatchedFiles)}}}}};
        const auto snapshot = IntegrityChecker::snapshot(record->installPath);
        require(snapshot.error.isEmpty(), snapshot.error);
        QJsonObject plan{{"buildId", build}, {"path", record->installPath}, {"index", fileHash(InstallIndex::filePath())},
            {"files", snapshot.files}, {"snapshot", snapshot.hash}, {"affectedProjects", affectedProjects(build)},
            {"missing", QJsonArray::fromStringList(integrity.missingFiles)}, {"modified", QJsonArray::fromStringList(integrity.mismatchedFiles)}};
        RepairSource source;
        if (args.operation == "repair" && !integrity.ok) {
            source = RepairExecutor::verifySource(build, now);
            require(source.ok, source.error);
            QJsonObject files;
            for (auto it = source.files.cbegin(); it != source.files.cend(); ++it) files[it.key()] = it.value();
            plan["repairFiles"] = files;
            plan["manifestSha256"] = source.manifestHash;
        }
        QJsonArray changes;
        for (auto it = snapshot.files.begin(); it != snapshot.files.end(); ++it) {
            QString action = "remove";
            if (args.operation == "repair") {
                const bool directory = it.value().toObject()["directory"].toBool();
                const bool retainedDirectory = directory && std::any_of(source.files.keyBegin(), source.files.keyEnd(),
                    [&](const auto& path) { return path.startsWith(it.key() + '/'); });
                if (integrity.ok || retainedDirectory) action = "preserve";
                else if (source.files.contains(it.key()))
                    action = it.value().toObject()["sha256"].toString() == source.files.value(it.key()) ? "preserve" : "replace";
            }
            changes.append(QJsonObject{{"path", it.key()}, {"action", action}});
        }
        auto repairPaths = source.files.keys();
        repairPaths.sort();
        for (const auto& path : repairPaths)
            if (!snapshot.files.contains(path)) changes.append(QJsonObject{{"path", path}, {"action", "add"}});
        plan["changes"] = changes;
        confirm(args, plan);
        const QString transaction = HandoffArea::newTransactionId();
        waitForElevated(args.operation == "repair" ? launchElevatedRepair(build, transaction, snapshot.hash, source.manifestHash) : launchElevatedUninstall(build, snapshot.hash), args.operation == "repair" ? transaction : QString());
        return success({{"buildId", build}, {"operation", args.operation}});
    }
    require(version != available.end(), "Unknown build id");
    require(!version->isLocalBuild() && ManifestVerifier::isSafeFileToken(version->semanticVersion)
        && version->semanticVersion.size() <= 128, "Release notes are available only for published Studio versions");
    VersionReleaseNotes notes;
    QEventLoop loop;
    QObject::connect(&notes, &VersionReleaseNotes::finished, &loop, &QEventLoop::quit);
    notes.fetch(version->semanticVersion);
    loop.exec();
    return success({{"studio", notes.url(version->semanticVersion, VersionReleaseNotes::Product::Studio).toString()},
        {"unity", notes.url(version->semanticVersion, VersionReleaseNotes::Product::UnityIntegration).toString()}});
}

Reply updates(const Arguments& args)
{
    UpdateCoordinator::reconcileElevation(QDateTime::currentDateTimeUtc());
    if (args.operation == "status") return success({{"hub", stateJson(UpdateStateStore::load("HarkenHub"))}, {"studio", stateJson(UpdateStateStore::load("HarkenStudio"))}});
    const QString product = args.required("product");
    require(product == "hub" || product == "studio", "--product must be hub or studio", "InvalidArguments");
    const QString identity = product == "hub" ? "HarkenHub" : "HarkenStudio";
    const auto before = UpdateStateStore::load(identity);
    const auto now = QDateTime::currentDateTimeUtc();
    if (args.operation != "check")
        require(!QStringList{"AwaitingBridge", "AwaitingElevation", "Installing", "Downloading", "Verifying"}.contains(before.state),
            "An update operation is already in progress; inspect updates status before retrying", "UpdateBusy");
    if (args.operation == "apply") {
        require(before.found && !before.downloadPath.isEmpty()
            && QStringList{"Verified", "AwaitingRestart", "AwaitingUserApproval", "AwaitingUserInstall", "InstallFailed"}.contains(before.state), "No downloaded update is ready");
        const auto envelope = readFile(before.manifestEnvelopePath);
        const auto manifestHash = QString::fromLatin1(QCryptographicHash::hash(envelope, QCryptographicHash::Sha256).toHex());
        const auto verified = ManifestVerifier::verify(envelope, TrustAnchors::manifestPublicKey(), now);
        require(verified.ok, verified.error);
        const auto& manifest = verified.manifest;
        require(manifest.product == identity && manifest.buildId == before.buildId && manifest.version == before.version
            && manifest.sha256Hex.compare(before.expectedSha256, Qt::CaseInsensitive) == 0
            && manifest.packageSize == before.packageSize && manifest.releaseSequence == before.releaseSequence,
            "Downloaded update state does not match its signed release");
        const auto hash = fileHash(before.downloadPath);
        require(hash.compare(manifest.sha256Hex, Qt::CaseInsensitive) == 0, "Downloaded update hash does not match its signed release");
        QJsonObject plan{{"update", stateJson(before)}, {"release", manifestJson(manifest)}, {"packageSha256", hash},
            {"manifestSha256", manifestHash}};
        if (product == "studio") plan["installation"] = packageReview(before.downloadPath, manifest);
        else plan["closeHubWindowsAndBackgroundService"] = true;
        confirm(args, plan);
        requireConsent(product);
        require(UpdateStateStore::load(identity) == before, "The update state changed; review the update again");
        if (product == "studio") {
            QString error;
            auto lease = DownloadLease::acquire(before.downloadPath, &error);
            require(bool(lease), error);
            const auto prepared = InstallRequest::prepare(before.downloadPath, before.manifestEnvelopePath,
                TrustAnchors::manifestPublicKey(), now, false);
            require(prepared.ok, prepared.error);
            auto pending = before;
            pending.state = "AwaitingElevation";
            pending.transactionId = prepared.transactionId;
            pending.updatedAt = now.toUTC().toString(Qt::ISODateWithMs);
            if (UpdateStateStore::replace(before, pending, now) != UpdateStateStore::WriteResult::Saved) {
                HandoffArea::discard(prepared.transactionId);
                require(false, "The update state changed; review the update again");
            }
            try {
                const auto installation = plan["installation"].toObject();
                const auto launch = launchElevatedInstall(prepared.transactionId, false, installation["destination"].toString(), installation["index"].toString(), manifestHash);
                if (!launch.ok) HandoffArea::discard(prepared.transactionId);
                waitForElevated(launch, prepared.transactionId);
            } catch (const Reply& failure) {
                auto failed = pending;
                failed.state = "InstallFailed";
                failed.failureDetail = failure.body["error"].toObject()["message"].toString();
                UpdateStateStore::replace(pending, failed, QDateTime::currentDateTimeUtc());
                throw;
            }
            require(UpdateStateStore::clear(pending) != UpdateStateStore::WriteResult::Failed, "Update installed, but its pending state could not be cleared");
            return success({{"buildId", prepared.buildId}, {"version", prepared.version}, {"transactionId", prepared.transactionId}});
        }
        require(HubSelfUpdate::isNewerVersion(manifest.version, QCoreApplication::applicationVersion()), "This Hub update is no longer newer than the running Hub");
        HubSelfUpdate updater;
        HubSelfUpdate::Options options;
        options.serverBaseUrl = ServerEndpoints::resolvedBaseUrl();
        options.manifestPublicKey = TrustAnchors::manifestPublicKey();
        options.currentVersion = QCoreApplication::applicationVersion();
        updater.configure(options);
        QString error;
        require(HubSelfUpdate::selfUpdateEnabled() && BridgeDispatcher::available(), "The Hub update bridge is unavailable");
        qint64 stoppingBackground = 0;
        bool dispatched = false;
        const auto resumeOnFailure = qScopeGuard([&] {
            if (!dispatched && stoppingBackground > 0
                && !QProcess::startDetached(QCoreApplication::applicationFilePath(),
                    {"--background", "--resume-after-pid", QString::number(stoppingBackground)}))
                qWarning() << "Could not restart the Hub background service after the update failed";
        });
        for (const auto& server : {SingleInstanceGuard::backgroundServerName(), SingleInstanceGuard::guiServerName()}) {
            if (!SingleInstanceGuard::isServerActive(server)) continue;
            SingleInstanceGuard observed(server);
            if (observed.isPrimary()) continue;
            const auto processId = observed.primaryProcessId();
            require(processId > 0, "Could not identify the running Hub process; close it before applying the update");
            require(SingleInstanceGuard::sendTo(server, {"--quit"}), "Close Hub and its background service, then retry the update");
            if (server == SingleInstanceGuard::backgroundServerName()) stoppingBackground = processId;
            QEventLoop waiting;
            QTimer poll;
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&poll, &QTimer::timeout, &waiting, [&] { if (!SingleInstanceGuard::isServerActive(server)) waiting.quit(); });
            QObject::connect(&deadline, &QTimer::timeout, &waiting, &QEventLoop::quit);
            poll.start(50);
            deadline.start(5000);
            waiting.exec();
            require(!SingleInstanceGuard::isServerActive(server), "Hub is still running; close it before applying the update", "HubStillRunning");
        }
        require(updater.approveViaBridge("background", &error, now, &before), error);
        dispatched = true;
        return success(stateJson(UpdateStateStore::load(identity)));
    }
    const auto result = checkUpdate(product == "studio" ? "harken" : "hub");
    if (result.manifestAbsent) return success({{"available", false}});
    require(result.manifest.product == identity, "Unexpected update product");
    const auto& manifest = result.manifest;
    require(!before.found || before.releaseSequence < 0 || manifest.releaseSequence < 0
        || (manifest.releaseSequence > before.releaseSequence
            || (manifest.releaseSequence == before.releaseSequence && manifest.buildId == before.buildId)),
        "The server release conflicts with a newer or different recorded release", "ReleaseRollback");
    bool available = HubSelfUpdate::isNewerVersion(manifest.version, QCoreApplication::applicationVersion());
    if (product == "studio") {
        const auto index = InstallIndex::snapshot();
        require(index.ok, index.error);
        require(manifest.releaseSequence >= index.lastReleaseSequence, "The server release is older than the installed release sequence", "ReleaseRollback");
        available = std::none_of(index.records.begin(), index.records.end(), [&](const auto& record) {
            return record.buildId == manifest.buildId && record.healthState == "Installed";
        });
    }
    if (args.operation == "check" || !available)
        return success({{"available", available}, {"release", manifestJson(manifest)}});
    requireConsent(product);
    const auto files = download(result.manifest, result.envelope, result.objectPath, result.releaseTag);
    UpdateStateRecord state;
    state.product = identity;
    state.state = product == "hub" ? "AwaitingRestart" : "AwaitingUserInstall";
    state.version = result.manifest.version;
    state.buildId = result.manifest.buildId;
    state.downloadPath = files["package"].toString();
    state.manifestEnvelopePath = files["manifest"].toString();
    state.expectedSha256 = result.manifest.sha256Hex;
    state.packageSize = result.manifest.packageSize;
    state.releaseSequence = result.manifest.releaseSequence;
    require(UpdateStateStore::replace(before, state, now) == UpdateStateStore::WriteResult::Saved, "The update state changed during download. Downloaded files were retained; review updates status.");
    return success(files);
}

}
