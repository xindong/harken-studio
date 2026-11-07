
#include "ToggleSwitch.h"
#include "../../Core/ThemeManager.h"
#include "../../Core/QtControlSkin.h"
#include <QPainter>
#include <QMouseEvent>
#include <QHBoxLayout>

namespace VNX {

static constexpr int ANIMATION_DURATION_MS = 150;

ToggleSwitch::ToggleSwitch(const QString& labelText, QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ToggleSwitch");
    auto& theme = ThemeManager::instance();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    
    if (!labelText.isEmpty())
    {
        label_ = new QLabel(labelText, this);
        label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(label_);
    }
    else
    {
        label_ = nullptr;
    }
    
    switchSpace_ = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addSpacerItem(switchSpace_);
    layout->addStretch();
    
    animation_ = new QPropertyAnimation(this, "position", this);
    animation_->setDuration(ANIMATION_DURATION_MS);
    animation_->setEasingCurve(QEasingCurve::InOutQuad);
    
    setCursor(Qt::PointingHandCursor);

    applyTheme();
    connect(&theme, &ThemeManager::themeChanged, this, &ToggleSwitch::applyTheme);
    connect(&theme, &ThemeManager::appearanceChanged, this, [this]() {
        update();
    });
}

ToggleSwitch::~ToggleSwitch() = default;

void ToggleSwitch::applyTheme()
{
    const auto& metrics = ThemeManager::instance().metrics();
    layout()->setSpacing(metrics.layout.controlGap);
    switchSpace_->changeSize(metrics.toggle.width, metrics.toggle.height,
                            QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout()->invalidate();
    if (label_) {
        label_->setMinimumWidth(metrics.parameter.labelMinimumWidth);
        QtControlSkin::setSemanticProperty(label_, "textRole",
                                          QStringLiteral("formLabel"));
    }
    updateGeometry();
    update();
}

QSize ToggleSwitch::sizeHint() const
{
    return layout()->sizeHint();
}

QSize ToggleSwitch::minimumSizeHint() const
{
    return layout()->minimumSize();
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
    
    const auto& metrics = theme.metrics();
    const auto& toggle = metrics.toggle;
    const int switchX = switchSpace_->geometry().x();
    const int switchY = (height() - toggle.height) / 2;
    
    const qreal inset = metrics.borderWidth / 2.0;
    const QRectF track = QRectF(switchX, switchY, toggle.width, toggle.height)
        .adjusted(inset, inset, -inset, -inset);
    
    QColor offColor = theme.colors().borderPrimary;
    QColor onColor = theme.colors().accent;
    if (!isEnabled()) {
        offColor = theme.colors().borderSecondary;
        onColor = theme.colors().textDisabled;
    }
    
    int r = static_cast<int>(offColor.red() + position_ * (onColor.red() - offColor.red()));
    int g = static_cast<int>(offColor.green() + position_ * (onColor.green() - offColor.green()));
    int b = static_cast<int>(offColor.blue() + position_ * (onColor.blue() - offColor.blue()));
    
    const int a = qRound(offColor.alpha() + position_ * (onColor.alpha() - offColor.alpha()));
    QColor trackColor(r, g, b, a);
    painter.setPen(QPen(theme.colors().borderSecondary, metrics.borderWidth));
    painter.setBrush(trackColor);
    painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    
    const qreal thumbX = switchX + toggle.thumbPadding + position_ * (toggle.width - toggle.height);
    const qreal thumbY = switchY + toggle.thumbPadding;
    const qreal thumbSize = toggle.height - toggle.thumbPadding * 2;
    
    QRectF thumb(thumbX, thumbY, thumbSize, thumbSize);
    
    QRadialGradient gradient(thumb.center(), thumbSize / 2);
    QColor thumbTop = theme.colors().textPrimary;
    QColor thumbMid = theme.colors().stateColor(theme.colors().bgTertiary, ThemeStateLayer::Emphasis);
    QColor thumbBottom = theme.colors().stateColor(theme.colors().bgSecondary, ThemeStateLayer::Emphasis);
    const QColor thumbBorder = theme.colors().borderSecondary;
    if (!isEnabled()) {
        thumbTop = theme.colors().textDisabled;
        thumbMid = theme.colors().textDisabled;
        thumbBottom = theme.colors().bgHover;
    }

    gradient.setColorAt(0.0, thumbTop);
    gradient.setColorAt(0.8, thumbMid);
    gradient.setColorAt(1.0, thumbBottom);
    
    painter.setBrush(gradient);
    painter.setPen(QPen(thumbBorder, metrics.borderWidth));
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
