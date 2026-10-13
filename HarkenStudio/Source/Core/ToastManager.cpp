
#include "ToastManager.h"
#include "ThemeManager.h"
#include "FontManager.h"

#include <QPainter>
#include <QPainterPath>
#include <QApplication>

namespace VNX {


QToastWidget::QToastWidget(const QString& message, int durationMs,
                           ToastType type, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , message_(message)
    , durationMs_(durationMs)
    , type_(type)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("toastWidget"));

    QFont font = FontManager::instance().getFont(FontRole::Body);
    QFontMetrics fm(font);
    int textWidth = fm.horizontalAdvance(message_);
    int padding = 24;
    int minWidth = 200;
    int maxWidth = 500;
    int width = qBound(minWidth, textWidth + padding * 2, maxWidth);
    int height = 36;

    if (textWidth > maxWidth - padding * 2) {
        QRect boundingRect = fm.boundingRect(
            QRect(0, 0, maxWidth - padding * 2, 0),
            Qt::TextWordWrap, message_);
        height = boundingRect.height() + padding;
    }

    setFixedSize(width, height);
    setWindowOpacity(0.0);

    holdTimer_ = new QTimer(this);
    holdTimer_->setSingleShot(true);
    connect(holdTimer_, &QTimer::timeout, this, &QToastWidget::startFadeOut);

    fadeInAnim_ = new QPropertyAnimation(this, "windowOpacity", this);
    fadeInAnim_->setDuration(200);
    fadeInAnim_->setStartValue(0.0);
    fadeInAnim_->setEndValue(1.0);
    connect(fadeInAnim_, &QPropertyAnimation::finished, [this]() {
        holdTimer_->start(durationMs_);
    });

    fadeOutAnim_ = new QPropertyAnimation(this, "windowOpacity", this);
    fadeOutAnim_->setDuration(300);
    fadeOutAnim_->setStartValue(1.0);
    fadeOutAnim_->setEndValue(0.0);
    connect(fadeOutAnim_, &QPropertyAnimation::finished, [this]() {
        emit finished();
        close();
    });
}

QToastWidget::~QToastWidget() = default;

void QToastWidget::showAnimated()
{
    show();
    fadeInAnim_->start();
}

void QToastWidget::startFadeOut()
{
    fadeOutAnim_->start();
}

void QToastWidget::paintEvent(QPaintEvent* )
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const auto& colors = ThemeManager::instance().colors();
    const auto& metrics = ThemeManager::instance().metrics();

    QColor accentColor;
    switch (type_) {
    case ToastType::Info:    accentColor = colors.info;    break;
    case ToastType::Success: accentColor = colors.success; break;
    case ToastType::Warning: accentColor = colors.warning; break;
    case ToastType::Error:   accentColor = colors.error;   break;
    }

    QColor bgColor = colors.bgSecondary;
    bgColor.setAlpha(230);

    QPainterPath path;
    path.addRoundedRect(rect(), metrics.radiusMedium, metrics.radiusMedium);
    painter.fillPath(path, bgColor);

    painter.setPen(Qt::NoPen);
    painter.setBrush(accentColor);
    painter.drawRoundedRect(QRectF(0, 0, 3, height()), 1.5, 1.5);

    painter.setPen(colors.textPrimary);
    QFont font = FontManager::instance().getFont(FontRole::Body);
    painter.setFont(font);
    QRect textRect = rect().adjusted(16, 0, -12, 0);
    painter.drawText(textRect, Qt::AlignVCenter | Qt::TextWordWrap, message_);
}


ToastManager& ToastManager::instance()
{
    static ToastManager inst;
    return inst;
}

ToastManager::ToastManager(QObject* parent)
    : QObject(parent)
{
    setObjectName(QStringLiteral("toastManager"));
}

void ToastManager::setParentWidget(QWidget* parent)
{
    parentWidget_ = parent;
}

void ToastManager::showToast(const QString& message, int durationMs,
                             ToastType type)
{
    if (!parentWidget_)
        return;

    while (activeToasts_.size() >= kMaxVisibleToasts) {
        QToastWidget* oldest = activeToasts_.first();
        removeToast(oldest);
        oldest->close();
    }

    auto* toast = new QToastWidget(message, durationMs, type, nullptr);

    connect(toast, &QToastWidget::finished, this, [this, toast]() {
        removeToast(toast);
    });

    activeToasts_.append(toast);
    repositionToasts();
    toast->showAnimated();
}

void ToastManager::repositionToasts()
{
    if (!parentWidget_ || activeToasts_.isEmpty())
        return;

    QRect parentGeom = parentWidget_->geometry();
    int bottomMargin = 40;
    int spacing = 8;

    int currentY = parentGeom.bottom() - bottomMargin;

    for (int i = activeToasts_.size() - 1; i >= 0; --i) {
        QToastWidget* toast = activeToasts_[i];
        currentY -= toast->height();
        int x = parentGeom.x() + (parentGeom.width() - toast->width()) / 2;
        toast->move(x, currentY);
        currentY -= spacing;
    }
}

void ToastManager::removeToast(QToastWidget* toast)
{
    activeToasts_.removeOne(toast);
    repositionToasts();
}

}
