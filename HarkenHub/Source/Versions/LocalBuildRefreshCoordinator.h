#pragma once

#include <QHash>
#include <QObject>
#include <QSet>

class QTimer;

namespace VNH {

class LocalBuildRefreshCoordinator : public QObject
{
    Q_OBJECT

public:
    explicit LocalBuildRefreshCoordinator(QObject* parent = nullptr);

    void start();
    void registerBuild(const QString& executablePath);

signals:
    void buildsChanged();
    void buildReady(const QString& executablePath);
    void buildRegistrationFailed(const QString& executablePath,
                                 const QString& error);

private:
    void checkForChanges();

    QTimer* timer_ = nullptr;
    bool refreshing_ = false;
    QHash<QString, QString> attemptedFingerprints_;
    QHash<QString, qint64> lastAttemptMs_;
    QSet<QString> registeringPaths_;
};

}
