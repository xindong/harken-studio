#pragma once

#include "Update/UpdateStateStore.h"

#include <QList>
#include <QIcon>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QAction;
class QMenu;
class QSystemTrayIcon;
class QTimer;

namespace VNH {

class HubSelfUpdate;
class UpdateCoordinator;

class UpdateNotifier : public QObject
{
    Q_OBJECT

public:
    struct Notification
    {
        bool show = false;
        QString key;
        QString title;
        QString message;
    };

    explicit UpdateNotifier(QObject* parent = nullptr);
    ~UpdateNotifier() override;

    void attach(UpdateCoordinator* coordinator);
    void attach(HubSelfUpdate* selfUpdate);
    void showTrayIcon();
    void retranslateTray();
    void setTrayIcon(const QIcon& icon);
    void handleState(const UpdateStateRecord& record);
    void notifyHubUpdated(const QString& version);

    static Notification evaluate(const UpdateStateRecord& record);
    static QStringList activationArguments();

protected:
    virtual void present(const Notification& notification);

private:
    void activateVersionsPage();
    void activateHubWindow();
    void activate(const QStringList& arguments);
    bool ensureTray();
    void flushPendingNotifications();

    QSystemTrayIcon* tray_ = nullptr;
    QIcon trayIcon_;
    QMenu* trayMenu_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* exitAction_ = nullptr;
    QTimer* trayRetryTimer_ = nullptr;
    int trayRetryAttempts_ = 0;
    QList<Notification> pendingNotifications_;
    QSet<QString> notifiedKeys_;
};

}
