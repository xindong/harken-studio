
#pragma once

#include "ContextInfo.h"

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QTimer>

class QWidget;

namespace VNX {

class ContextInfoService : public QObject
{
    Q_OBJECT

public:
    static ContextInfoService& instance();

    explicit ContextInfoService(QObject* parent = nullptr);
    ~ContextInfoService() override;

    ContextInfoService(const ContextInfoService&) = delete;
    ContextInfoService& operator=(const ContextInfoService&) = delete;

    void setActive(bool active);
    bool isActive() const { return filterInstalled_; }

    void setLocked(bool locked);
    bool isLocked() const { return locked_; }

    void setFreezeRoot(QWidget* widget);

    const ContextTopic& currentTopic() const { return current_; }
    void clearTopic();

signals:
    void topicChanged(const VNX::ContextTopic& topic);
    void lockedChanged(bool locked);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateEventFilter();
    void considerPointer(QWidget* receiver, const QPoint& globalPos, bool allowHoverOptIn, bool immediate);
    bool suspended(QWidget* receiver) const;
    void commitPending();
    void scheduleTopic(const ContextTopic& topic, QWidget* receiver);
    void present(const ContextTopic& topic);
    void presentWidget(QWidget* widget);

    QTimer debounce_;
    ContextTopic current_;
    ContextTopic pending_;
    QPointer<QWidget> pendingReceiver_;
    QPointer<QWidget> freezeRoot_;
    bool active_ = false;
    bool locked_ = false;
    bool filterInstalled_ = false;
};

}
