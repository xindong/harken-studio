
#pragma once

#include <QWidget>
#include <QFileInfo>
#include <QIcon>
#include <QPainter>
#include <QStringList>
#include <QTimer>
#include <QCursor>
#include <QDrag>
#include "../Core/ThemeAssetCatalog.h"
#include "../Core/ThemeManager.h"

namespace VNX {

class DragDropOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit DragDropOverlay(QWidget* parent)
        : QWidget(parent)
    {
        setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                       | Qt::WindowTransparentForInput);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedSize(iconExtent(), iconExtent());
        setVisible(false);

        followTimer_.setInterval(16);
        connect(&followTimer_, &QTimer::timeout, this, &DragDropOverlay::followCursor);
    }

    void showOverlay(const QStringList& paths, Qt::DropAction action, bool layeredFeedback)
    {
        if (paths.isEmpty()) {
            hideOverlay();
            return;
        }
        icon_ = ThemeManager::instance().resolveIcon(QString::fromLatin1(
            QFileInfo(paths.first()).isDir() ? ThemeAssetIds::TreeFolderClosed
                                           : ThemeAssetIds::AudioSource)).icon;
        QDrag drag(this);
        cursor_ = layeredFeedback ? drag.dragCursor(action) : QPixmap();
        iconOffset_ = layeredFeedback ? -drag.hotSpot() : QPoint();
        setFixedSize(layeredFeedback
            ? drag.dragCursor(Qt::CopyAction).deviceIndependentSize().toSize()
                  .expandedTo(drag.dragCursor(Qt::IgnoreAction).deviceIndependentSize().toSize())
                  .expandedTo(QSize(iconExtent() + iconOffset_.x(), iconExtent() + iconOffset_.y()))
            : QSize(iconExtent(), iconExtent()));
        followCursor();
        setVisible(true);
        raise();
        followTimer_.start();
        update();
    }

    void hideOverlay()
    {
        followTimer_.stop();
        setVisible(false);
    }

private slots:
    void followCursor()
    {
        const QPoint cursorPos = QCursor::pos();
        move(cursorPos + QPoint(ThemeManager::instance().metrics().spacingMedium, ThemeManager::instance().metrics().spacingMedium) - iconOffset_);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        icon_.paint(&p, QRect(iconOffset_, QSize(iconExtent(), iconExtent())));
        if (!cursor_.isNull())
            p.drawPixmap(QPoint(), cursor_);
    }

private:
    static int iconExtent() { return ThemeManager::instance().metrics().objectIconExtent; }
    QIcon icon_;
    QPixmap cursor_;
    QPoint iconOffset_;
    QTimer followTimer_;
};

}
