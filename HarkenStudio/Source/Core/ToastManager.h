
#pragma once

#include <QObject>
#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <QPropertyAnimation>
#include <QList>
#include <QPointer>
#include <QString>

namespace VNX {

enum class ToastType {
    Info,
    Success,
    Warning,
    Error
};

class QToastWidget : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal windowOpacity READ windowOpacity WRITE setWindowOpacity)

public:
    explicit QToastWidget(const QString& message, int durationMs,
                          ToastType type, QWidget* parent = nullptr);
    ~QToastWidget() override;

    void showAnimated();

signals:
    void finished();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void startFadeOut();

    QString message_;
    int durationMs_;
    ToastType type_;
    QTimer* holdTimer_ = nullptr;
    QPropertyAnimation* fadeInAnim_ = nullptr;
    QPropertyAnimation* fadeOutAnim_ = nullptr;
};

class ToastManager : public QObject
{
    Q_OBJECT

public:
    static ToastManager& instance();

    ToastManager(const ToastManager&) = delete;
    ToastManager& operator=(const ToastManager&) = delete;

    void setParentWidget(QWidget* parent);

    void showToast(const QString& message, int durationMs = 3000,
                   ToastType type = ToastType::Info);

    static constexpr int kMaxVisibleToasts = 3;

private:
    explicit ToastManager(QObject* parent = nullptr);
    ~ToastManager() override = default;

    void repositionToasts();
    void removeToast(QToastWidget* toast);

    QPointer<QWidget> parentWidget_;
    QList<QToastWidget*> activeToasts_;
};

}
