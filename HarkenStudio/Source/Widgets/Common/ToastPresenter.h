#pragma once

#include <QMargins>
#include <QPropertyAnimation>
#include <QString>
#include <QWidget>

class QGraphicsOpacityEffect;

namespace VNX {

class ToastPresenter final : public QWidget
{
    Q_OBJECT

public:
    explicit ToastPresenter(QWidget* host);

    void showMessage(const QString& message);
    void dismiss();
    void setViewportMargins(const QMargins& margins);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void updatePlacement();

    QString message_;
    QMargins viewportMargins_;
    QGraphicsOpacityEffect* opacity_;
    QPropertyAnimation fadeAnimation_;
};

}
