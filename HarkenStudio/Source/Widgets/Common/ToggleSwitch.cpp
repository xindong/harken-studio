
#include "ToggleSwitch.h"
#include "../../Core/ThemeManager.h"
#include "../../Core/QtControlSkin.h"
#include <QPainter>
#include <QMouseEvent>
#include <QHBoxLayout>

namespace VNX {

static constexpr int SWITCH_LABEL_GAP = 16;
static constexpr int SWITCH_NO_LABEL_OFFSET = 8;
static constexpr int THUMB_PADDING = 3;
static constexpr int ANIMATION_DURATION_MS = 150;

ToggleSwitch::ToggleSwitch(const QString& labelText, QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ToggleSwitch");
    auto& theme = ThemeManager::instance();
    const auto& metrics = theme.metrics();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(metrics.spacingSmall);
    
    if (!labelText.isEmpty())
    {
        label_ = new QLabel(labelText, this);
        label_->setMinimumWidth(100 + metrics.spacingSmall);
        label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(label_);
    }
    else
    {
        label_ = nullptr;
    }
    
    layout->addSpacing(switchWidth_ + metrics.spacingSmall);
    layout->addStretch();
    
    animation_ = new QPropertyAnimation(this, "position", this);
    animation_->setDuration(ANIMATION_DURATION_MS);
    animation_->setEasingCurve(QEasingCurve::InOutQuad);
    
    setCursor(Qt::PointingHandCursor);

    applyTheme();
    connect(&theme, &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
        update();
    });
}

ToggleSwitch::~ToggleSwitch() = default;

void ToggleSwitch::applyTheme()
{
    if (label_) {
        QtControlSkin::setSemanticProperty(label_, "textRole",
                                          QStringLiteral("formLabel"));
    }
}

void ToggleSwitch::setChecked(bool checked)
{
    if (checked_ != checked)
    {
        checked_ = checked;
        animateToggle();
        emit toggled(checked_);
    }
}

void ToggleSwitch::toggle()
{
    setChecked(!checked_);
}

void ToggleSwitch::setPosition(qreal pos)
{
    position_ = pos;
    update();
}

void ToggleSwitch::animateToggle()
{
    animation_->stop();
    animation_->setStartValue(position_);
    animation_->setEndValue(checked_ ? 1.0 : 0.0);
    animation_->start();
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& theme = ThemeManager::instance();
    
    int switchX = label_ ? label_->width() + SWITCH_LABEL_GAP : SWITCH_NO_LABEL_OFFSET;
    int switchY = (height() - switchHeight_) / 2;
    
    QRectF track(switchX, switchY, switchWidth_, switchHeight_);
    
    QColor offColor = theme.colors().borderPrimary;
    QColor onColor = theme.colors().accent;
    if (!isEnabled()) {
        offColor = theme.colors().borderSecondary;
        onColor = theme.colors().textDisabled;
    }
    
    int r = static_cast<int>(offColor.red() + position_ * (onColor.red() - offColor.red()));
    int g = static_cast<int>(offColor.green() + position_ * (onColor.green() - offColor.green()));
    int b = static_cast<int>(offColor.blue() + position_ * (onColor.blue() - offColor.blue()));
    
    QColor trackColor(r, g, b);
    painter.setPen(QPen(theme.colors().borderSecondary, 1));
    painter.setBrush(trackColor);
    painter.drawRoundedRect(track, switchHeight_ / 2, switchHeight_ / 2);
    
    float thumbX = switchX + THUMB_PADDING + position_ * (switchWidth_ - switchHeight_);
    float thumbY = switchY + THUMB_PADDING;
    float thumbSize = switchHeight_ - THUMB_PADDING * 2;
    
    QRectF thumb(thumbX, thumbY, thumbSize, thumbSize);
    
    QRadialGradient gradient(thumb.center(), thumbSize / 2);
    QColor thumbTop = theme.colors().textPrimary;
    QColor thumbMid = theme.colors().bgTertiary.lighter(140);
    QColor thumbBottom = theme.colors().bgSecondary.lighter(140);
    QColor thumbBorder = theme.colors().borderSecondary;
    if (!isEnabled()) {
        thumbTop = theme.colors().textDisabled;
        thumbMid = theme.colors().textDisabled;
        thumbBottom = theme.colors().bgHover;
        thumbBorder = theme.colors().borderPrimary;
    }

    gradient.setColorAt(0.0, thumbTop);
    gradient.setColorAt(0.8, thumbMid);
    gradient.setColorAt(1.0, thumbBottom);
    
    painter.setBrush(gradient);
    painter.setPen(QPen(thumbBorder, 1));
    painter.drawEllipse(thumb);
}

void ToggleSwitch::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        event->accept();
    }
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        toggle();
        event->accept();
    }
}

}
