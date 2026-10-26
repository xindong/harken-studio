#pragma once

#include <QIcon>
#include <QObject>

namespace VNH { class TaskbarIcon; }

namespace VNH::HubIcons {

QIcon applicationIcon(const QColor& color);
QIcon settingsIcon(qreal devicePixelRatio);
QIcon moreIcon(qreal devicePixelRatio);

class ApplicationIcon final : public QObject
{
    Q_OBJECT

public:
    explicit ApplicationIcon(QObject* parent = nullptr);
    QIcon systemIcon() const;

signals:
    void changed(const QIcon& icon);

private:
    void applyColor(const QColor& color);
    QIcon systemIcon_;
#ifdef Q_OS_WIN
    TaskbarIcon* taskbar_ = nullptr;
#endif
};

}
