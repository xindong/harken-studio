#include "Versions/LocalBuildRefreshCoordinator.h"

#include "Versions/LocalBuildRegistry.h"
#include "Versions/VersionScanner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>

namespace VNH {

namespace {

constexpr qint64 kRetryIntervalMs = 30000;

struct RefreshRequest
{
    LocalBuildRecord record;
    QString fingerprint;
};

QString normalizedExecutablePath(const QString& path)
{
    const QFileInfo executable(path);
    QString identity = executable.canonicalFilePath();
    if (identity.isEmpty())
        identity = executable.absoluteFilePath();
    identity = QDir::cleanPath(QDir::fromNativeSeparators(identity));
#ifdef Q_OS_WIN
    identity = identity.toLower();
#endif
    return identity;
}

}

LocalBuildRefreshCoordinator::LocalBuildRefreshCoordinator(QObject* parent)
    : QObject(parent)
{
    timer_ = new QTimer(this);
    timer_->setInterval(1500);
    connect(timer_, &QTimer::timeout, this,
            &LocalBuildRefreshCoordinator::checkForChanges);
}

void LocalBuildRefreshCoordinator::start()
{
    if (timer_->isActive())
        return;
    timer_->start();
    QTimer::singleShot(0, this,
                       &LocalBuildRefreshCoordinator::checkForChanges);
}

void LocalBuildRefreshCoordinator::registerBuild(
    const QString& executablePath)
{
    const QFileInfo executable(executablePath);
    if (!executable.exists() || !executable.isFile()) {
        emit buildRegistrationFailed(
            executablePath,
            QStringLiteral("The HarkenStudio executable no longer exists"));
        return;
    }

    const QString identity = normalizedExecutablePath(executablePath);
    const std::vector<InstalledVersion> installed =
        VersionScanner::scanInstalled();
    if (std::any_of(installed.begin(), installed.end(),
                    [&identity](const InstalledVersion& version) {
                        return normalizedExecutablePath(
                                   version.executablePath) == identity;
                    })) {
        emit buildReady(executable.absoluteFilePath());
        return;
    }

    QString registryError;
    const auto records = LocalBuildRegistry::load(&registryError);
    if (!registryError.isEmpty()) {
        emit buildRegistrationFailed(executablePath, registryError);
        return;
    }
    const auto existing = std::find_if(records.begin(), records.end(),
        [&identity](const LocalBuildRecord& record) {
            return normalizedExecutablePath(record.executablePath) == identity;
        });
    const bool wasRegistered = existing != records.end();
    if (wasRegistered && LocalBuildRegistry::metadataMatchesFile(*existing)
        && LocalBuildRegistry::fileFingerprint(executablePath) == existing->executableFingerprint) {
        emit buildReady(executable.absoluteFilePath());
        return;
    }

    if (registeringPaths_.contains(identity))
        return;
    registeringPaths_.insert(identity);

    const QString path = executable.absoluteFilePath();
    const QPointer<LocalBuildRefreshCoordinator> self(this);
    QThreadPool::globalInstance()->start([self, identity, path, wasRegistered]() {
        LocalBuildRecord record;
        QString error;
        const bool registered =
            LocalBuildRegistry::probe(path, &record, &error) &&
            (wasRegistered ? LocalBuildRegistry::updateExisting(record, &error)
                           : LocalBuildRegistry::upsert(record, &error));
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, identity, path, registered, error]() {
                if (!self)
                    return;
                self->registeringPaths_.remove(identity);
                if (registered) {
                    emit self->buildsChanged();
                    emit self->buildReady(path);
                } else {
                    emit self->buildRegistrationFailed(path, error);
                }
            },
            Qt::QueuedConnection);
    });
}

void LocalBuildRefreshCoordinator::checkForChanges()
{
    if (refreshing_)
        return;

    QString loadError;
    const std::vector<LocalBuildRecord> records =
        LocalBuildRegistry::load(&loadError);
    if (!loadError.isEmpty())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    std::vector<RefreshRequest> requests;
    for (const auto& record : records) {
        if (LocalBuildRegistry::metadataMatchesFile(record))
            continue;
        const QString fingerprint =
            LocalBuildRegistry::fileFingerprint(record.executablePath);
        if (fingerprint.isEmpty())
            continue;
        if (attemptedFingerprints_.value(record.executablePath) == fingerprint &&
            now - lastAttemptMs_.value(record.executablePath) <
                kRetryIntervalMs)
            continue;
        attemptedFingerprints_[record.executablePath] = fingerprint;
        lastAttemptMs_[record.executablePath] = now;
        requests.push_back({record, fingerprint});
    }
    if (requests.empty())
        return;

    refreshing_ = true;
    emit buildsChanged();
    const QPointer<LocalBuildRefreshCoordinator> self(this);
    QThreadPool::globalInstance()->start([self, requests]() {
        QStringList refreshedPaths;
        bool changed = false;
        for (const auto& request : requests) {
            if (LocalBuildRegistry::fileFingerprint(
                    request.record.executablePath) != request.fingerprint)
                continue;
            LocalBuildRecord refreshed;
            QString error;
            if (!LocalBuildRegistry::probe(request.record.executablePath,
                                           &refreshed, &error)) {
                auto failed = request.record;
                failed.lastProbeError = error;
                if (failed.lastProbeError != request.record.lastProbeError
                    && LocalBuildRegistry::fileFingerprint(failed.executablePath) == request.fingerprint)
                    changed = LocalBuildRegistry::updateExisting(failed, &error) || changed;
                continue;
            }
            if (LocalBuildRegistry::fileFingerprint(
                    request.record.executablePath) != request.fingerprint)
                continue;
            if (LocalBuildRegistry::updateExisting(refreshed, &error)) {
                refreshedPaths.append(request.record.executablePath);
                changed = true;
            }
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, refreshedPaths, changed]() {
                if (!self)
                    return;
                self->refreshing_ = false;
                for (const QString& path : refreshedPaths) {
                    self->attemptedFingerprints_.remove(path);
                    self->lastAttemptMs_.remove(path);
                }
                if (changed)
                    emit self->buildsChanged();
            },
            Qt::QueuedConnection);
    });
}

}
