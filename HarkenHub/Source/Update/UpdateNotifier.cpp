#include "Update/UpdateNotifier.h"

#include "App/SingleInstanceGuard.h"
#include "Update/HubSelfUpdate.h"
#include "Update/UpdateCoordinator.h"

#include <QCoreApplication>
#include <QDebug>
#include <QIcon>
#include <QMenu>
#include <QProcess>
#include <QSystemTrayIcon>
#include <QTimer>

namespace VNH {

UpdateNotifier::UpdateNotifier(QObject* parent)
    : QObject(parent)
{
}

UpdateNotifier::~UpdateNotifier()
{
    delete trayMenu_;
}

void UpdateNotifier::attach(UpdateCoordinator* coordinator)
{
    connect(coordinator, &UpdateCoordinator::stateChanged, this,
            &UpdateNotifier::handleState);
}

void UpdateNotifier::attach(HubSelfUpdate* selfUpdate)
{
    connect(selfUpdate, &HubSelfUpdate::stateChanged, this,
            &UpdateNotifier::handleState);
}

UpdateNotifier::Notification UpdateNotifier::evaluate(
    const UpdateStateRecord& record)
{
    Notification notification;
    if (record.product == QStringLiteral("HarkenStudio") &&
        record.state == QStringLiteral("Installed")) {
        notification.show = true;
        notification.key = record.product + QStringLiteral("/") +
                           record.buildId + QStringLiteral("/installed");
        notification.title = tr("HarkenStudio update installed");
        notification.message =
            tr("HarkenStudio %1 has been installed and is now available in "
               "Versions.")
                .arg(record.version);
    } else if (record.product == QStringLiteral("HarkenStudio") &&
               record.state == QStringLiteral("AwaitingUserInstall")) {
        notification.show = true;
        notification.key = record.product + QStringLiteral("/") +
                           record.buildId + QStringLiteral("/ready");
        notification.title = tr("HarkenStudio update ready");
        notification.message =
            tr("HarkenStudio %1 has been downloaded and verified. Open Harken "
               "Studio Hub to install it.")
                .arg(record.version);
    } else if (record.product == QStringLiteral("HarkenHub") &&
               record.state == QStringLiteral("AwaitingUserApproval")) {
        notification.show = true;
        notification.key = record.product + QStringLiteral("/") +
                           record.buildId + QStringLiteral("/ready");
        notification.title = tr("Harken Studio Hub update ready");
        notification.message =
            tr("Harken Studio Hub %1 is ready to install. Open Harken Studio Hub to approve "
               "the update.")
                .arg(record.version);
    }
    return notification;
}

void UpdateNotifier::notifyHubUpdated(const QString& version)
{
    Notification notification;
    notification.show = true;
    notification.key =
        QStringLiteral("HarkenHub/updated/") + version;
    notification.title = tr("Harken Studio Hub updated");
    notification.message =
        tr("Harken Studio Hub has been updated to %1.").arg(version);
    if (notifiedKeys_.contains(notification.key))
        return;
    notifiedKeys_.insert(notification.key);
    present(notification);
}

QStringList UpdateNotifier::activationArguments()
{
    return {QStringLiteral("--show-versions")};
}

void UpdateNotifier::handleState(const UpdateStateRecord& record)
{
    const Notification notification = evaluate(record);
    if (!notification.show || notifiedKeys_.contains(notification.key))
        return;
    notifiedKeys_.insert(notification.key);
    present(notification);
}

bool UpdateNotifier::ensureTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return false;
    if (!tray_) {
        tray_ = new QSystemTrayIcon(this);
        tray_->setIcon(QIcon(QStringLiteral(":/Resources/icons/HubIcon.ico")));
        tray_->setToolTip(tr("Harken Studio Hub"));
        connect(tray_, &QSystemTrayIcon::messageClicked, this,
                &UpdateNotifier::activateVersionsPage);
        connect(tray_, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger ||
                        reason == QSystemTrayIcon::DoubleClick)
                        activateHubWindow();
                });
        trayMenu_ = new QMenu();
        trayMenu_->setStyleSheet(QStringLiteral(
            "QMenu { background-color: #1D2330; border: 1px solid #3D4554; "
            "border-radius: 0; padding: 6px 0; color: #E0E0E0; } "
            "QMenu::item { padding: 6px 32px 6px 24px; min-width: 120px; "
            "background: transparent; } "
            "QMenu::item:selected { background-color: #00F0FF; "
            "color: #000000; }"));
        openAction_ = trayMenu_->addAction(tr("Open Harken Studio Hub"));
        connect(openAction_, &QAction::triggered, this,
                &UpdateNotifier::activateHubWindow);
        exitAction_ = trayMenu_->addAction(tr("Exit"));
        connect(exitAction_, &QAction::triggered, this, []() {
            SingleInstanceGuard::sendTo(SingleInstanceGuard::guiServerName(),
                                        {QStringLiteral("--quit")});
            QCoreApplication::quit();
        });
        tray_->setContextMenu(trayMenu_);
    }
    tray_->show();
    return true;
}

void UpdateNotifier::showTrayIcon()
{
    if (ensureTray())
        return;
    if (trayRetryTimer_)
        return;
    trayRetryTimer_ = new QTimer(this);
    trayRetryTimer_->setInterval(10000);
    connect(trayRetryTimer_, &QTimer::timeout, this, [this]() {
        ++trayRetryAttempts_;
        const bool ready = ensureTray();
        if (ready)
            flushPendingNotifications();
        if (ready || trayRetryAttempts_ >= 18) {
            pendingNotifications_.clear();
            trayRetryTimer_->stop();
            trayRetryTimer_->deleteLater();
            trayRetryTimer_ = nullptr;
        }
    });
    trayRetryTimer_->start();
}

void UpdateNotifier::retranslateTray()
{
    if (tray_)
        tray_->setToolTip(tr("Harken Studio Hub"));
    if (openAction_)
        openAction_->setText(tr("Open Harken Studio Hub"));
    if (exitAction_)
        exitAction_->setText(tr("Exit"));
}

void UpdateNotifier::flushPendingNotifications()
{
    const QList<Notification> pending = pendingNotifications_;
    pendingNotifications_.clear();
    for (const Notification& notification : pending)
        tray_->showMessage(notification.title, notification.message,
                           QSystemTrayIcon::Information, 10000);
}

void UpdateNotifier::present(const Notification& notification)
{
    if (!ensureTray()) {
        qInfo().noquote()
            << notification.title << "-" << notification.message;
        if (trayRetryTimer_)
            pendingNotifications_.append(notification);
        return;
    }
    flushPendingNotifications();
    tray_->showMessage(notification.title, notification.message,
                       QSystemTrayIcon::Information, 10000);
}

void UpdateNotifier::activateVersionsPage()
{
    activate(activationArguments());
}

void UpdateNotifier::activateHubWindow()
{
    activate({});
}

void UpdateNotifier::activate(const QStringList& arguments)
{
    if (SingleInstanceGuard::sendTo(SingleInstanceGuard::guiServerName(),
                                    arguments))
        return;
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                            arguments);
}

}
