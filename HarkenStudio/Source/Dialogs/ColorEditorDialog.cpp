#include "ColorEditorDialog.h"

#include "Core/QtControlSkin.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/LinearSliderPresenter.h"
#include "Widgets/Common/ParameterBinding.h"

#include <QCursor>
#include <QDoubleSpinBox>
#include <QGuiApplication>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QSignalBlocker>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>

#include <functional>

namespace VNX {

class ColorPicker final : public QWidget
{
public:
    explicit ColorPicker(QWidget* parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("colorPicker"));
        setAccessibleName(tr("Color picker"));
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(280, 200);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setHsv(qreal hue, qreal saturation, qreal value)
    {
        hue_ = hue;
        saturation_ = saturation;
        value_ = value;
        update();
    }

    std::function<void(qreal, qreal, qreal)> changed;

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide
            || event->type() == QEvent::WindowDeactivate)
            dragging_ = false;
        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const QRect plane = planeRect();
        QLinearGradient saturation(plane.topLeft(), plane.topRight());
        saturation.setColorAt(0.0, Qt::white);
        saturation.setColorAt(1.0, QColor::fromHsvF(hue_, 1.0, 1.0));
        painter.fillRect(plane, saturation);
        QLinearGradient brightness(plane.topLeft(), plane.bottomLeft());
        brightness.setColorAt(0.0, Qt::transparent);
        brightness.setColorAt(1.0, Qt::black);
        painter.fillRect(plane, brightness);

        const QRect hue = hueRect();
        QLinearGradient spectrum(hue.topLeft(), hue.bottomLeft());
        for (int i = 0; i <= 6; ++i)
            spectrum.setColorAt(i / 6.0, QColor::fromHsvF(i / 6.0, 1.0, 1.0));
        painter.fillRect(hue, spectrum);
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(plane.adjusted(0, 0, -1, -1));
        painter.drawRect(hue.adjusted(0, 0, -1, -1));

        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF point(plane.left() + saturation_ * (plane.width() - 1),
                            plane.top() + (1.0 - value_) * (plane.height() - 1));
        painter.setPen(QPen(Qt::black, 3));
        painter.drawEllipse(point, 5, 5);
        painter.setPen(QPen(Qt::white, 1));
        painter.drawEllipse(point, 5, 5);
        const qreal y = hue.top() + hue_ * (hue.height() - 1);
        const QRectF marker(hue.left() - 2, y - 2, hue.width() + 4, 4);
        painter.setPen(QPen(Qt::black, 3));
        painter.drawRect(marker);
        painter.setPen(QPen(Qt::white, 1));
        painter.drawRect(marker);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) return;
        const QPoint point = event->position().toPoint();
        if (!planeRect().contains(point) && !hueRect().contains(point)) return;
        dragging_ = true;
        editingHue_ = hueRect().contains(point);
        setFocus(Qt::MouseFocusReason);
        selectPosition(event->position());
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) selectPosition(event->position());
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !dragging_) return;
        dragging_ = false;
        selectPosition(event->position());
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        const qreal step = event->modifiers() & Qt::ShiftModifier ? 0.1 : 0.01;
        qreal horizontal = 0.0;
        qreal vertical = 0.0;
        switch (event->key()) {
        case Qt::Key_Left: horizontal = -step; break;
        case Qt::Key_Right: horizontal = step; break;
        case Qt::Key_Up: vertical = -step; break;
        case Qt::Key_Down: vertical = step; break;
        default: QWidget::keyPressEvent(event); return;
        }
        if (editingHue_) hue_ = qBound(0.0, hue_ + horizontal + vertical, 359.0 / 360.0);
        else {
            saturation_ = qBound(0.0, saturation_ + horizontal, 1.0);
            value_ = qBound(0.0, value_ - vertical, 1.0);
        }
        changed(hue_, saturation_, value_);
    }

private:
    QRect planeRect() const { return rect().adjusted(6, 6, -42, -6); }
    QRect hueRect() const { return QRect(width() - 26, 6, 20, height() - 12); }

    void selectPosition(const QPointF& point)
    {
        if (editingHue_) {
            const QRect hue = hueRect();
            hue_ = qBound(0.0, (point.y() - hue.top()) / (hue.height() - 1), 359.0 / 360.0);
        } else {
            const QRect plane = planeRect();
            saturation_ = qBound(0.0, (point.x() - plane.left()) / (plane.width() - 1), 1.0);
            value_ = 1.0 - qBound(0.0, (point.y() - plane.top()) / (plane.height() - 1), 1.0);
        }
        changed(hue_, saturation_, value_);
    }

    qreal hue_ = 0.0;
    qreal saturation_ = 0.0;
    qreal value_ = 0.0;
    bool editingHue_ = false;
    bool dragging_ = false;
};

ColorEditorDialog::ColorEditorDialog(const QColor& color, QWidget* parent)
    : BaseDialog(parent)
{
    resize(380, 430);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    picker_ = new ColorPicker(this);
    picker_->changed = [this](qreal hue, qreal saturation, qreal value) {
        hue_ = hue;
        saturation_ = saturation;
        value_ = value;
        publishHsv();
    };
    layout->addWidget(picker_, 1);

    auto* values = new QGridLayout();
    values->setHorizontalSpacing(8);
    values->setVerticalSpacing(8);
    const std::array<const char*, 6> labels{"H", "S", "V", "R", "G", "B"};
    const std::array<const char*, 6> names{"colorHue", "colorSaturation", "colorValue",
                                         "colorRed", "colorGreen", "colorBlue"};
    const std::array<QString, 6> descriptions{tr("Hue"), tr("Saturation"), tr("Value"),
                                             tr("Red"), tr("Green"), tr("Blue")};
    for (int i = 0; i < 6; ++i) {
        auto* label = new QLabel(QString::fromLatin1(labels[i]), this);
        auto* channel = new QSpinBox(this);
        channels_[i] = channel;
        channel->setObjectName(QString::fromLatin1(names[i]));
        channel->setAccessibleName(descriptions[i]);
        channel->setToolTip(descriptions[i]);
        channel->setRange(0, i == 0 ? 359 : i < 3 ? 100 : 255);
        channel->setKeyboardTracking(false);
        label->setBuddy(channel);
        const int column = (i % 3) * 2;
        values->addWidget(label, i / 3, column);
        values->addWidget(channel, i / 3, column + 1);
        values->setColumnStretch(column + 1, 1);
        connect(channel, &QSpinBox::valueChanged, this, [this, i](int value) {
            if (i < 3) {
                if (i == 0) hue_ = value / 360.0;
                else if (i == 1) saturation_ = value / 100.0;
                else value_ = value / 100.0;
                publishHsv();
            } else {
                QColor next = color_;
                if (i == 3) next.setRed(value);
                else if (i == 4) next.setGreen(value);
                else next.setBlue(value);
                setCurrentColor(next);
            }
        });
    }
    alpha_ = new ParameterBinding(this);
    alpha_->setObjectName(QStringLiteral("colorAlphaBinding"));
    alpha_->setName(QStringLiteral("A"));
    alpha_->setRange(0, 255);
    alpha_->setDecimals(0);
    alpha_->setQuantum(1);
    auto* alpha = new LinearSliderPresenter(alpha_, true, &ThemeManager::instance(), this);
    alpha->setObjectName(QStringLiteral("colorAlphaControl"));
    alpha->setToolTip(tr("Alpha"));
    alpha->setCompactValueColumn(ThemeManager::instance().metrics().parameter.compactValueMinimumWidth);
    auto* alphaLabel = new QLabel(QStringLiteral("A"), this);
    alphaLabel->setBuddy(alpha->valueEditor());
    values->addWidget(alphaLabel, 2, 0);
    values->addWidget(alpha, 2, 1, 1, 5);
    connect(alpha_, &ParameterBinding::valueUpdated, this, [this](double value) {
        if (color_.alpha() == qRound(value)) return;
        QColor next = color_;
        next.setAlpha(qRound(value));
        publishColor(next);
    });
    layout->addLayout(values);
    auto* screenPick = new QToolButton(this);
    screenPick->setObjectName(QStringLiteral("colorScreenPick"));
    screenPick->setAccessibleName(tr("Pick screen color"));
    screenPick->setToolTip(tr("Pick screen color. Click to select; Esc cancels."));
    screenPick->setProperty("flatIconButton", true);
    screenPick->setProperty("iconButtonBackground", true);
    screenPick->setIconSize(QSize(20, 20));
    const auto refreshIcon = [screenPick] {
        auto& theme = ThemeManager::instance();
        screenPick->setIcon(theme.resolveIcon(QString::fromLatin1(ThemeAssetIds::ColorEyedropper)).icon);
        const int extent = QtControlSkin::current().fieldHeight();
        screenPick->setFixedSize(extent, extent);
    };
    refreshIcon();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, screenPick, refreshIcon);
    connect(screenPick, &QToolButton::clicked, this, &ColorEditorDialog::pickScreenColor);
    screenPickTimer_ = new QTimer(this);
    screenPickTimer_->setInterval(30);
    connect(screenPickTimer_, &QTimer::timeout, this, [this] {
        if (!updateScreenColor()) finishScreenPick(false);
    });
    setupStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* actions = new QHBoxLayout();
    actions->addWidget(screenPick);
    actions->addStretch();
    actions->addWidget(buttonBox());
    layout->addLayout(actions);
    setDialogTabOrder({picker_, channels_[0], channels_[1], channels_[2], channels_[3],
                       channels_[4], channels_[5], alpha->primaryEditor(), alpha->valueEditor(), screenPick});
    setCurrentColor(color);
}

bool ColorEditorDialog::event(QEvent* event)
{
    if (screenPicking_ && (event->type() == QEvent::Hide
        || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::UngrabMouse))
        finishScreenPick(false);
    return BaseDialog::event(event);
}

void ColorEditorDialog::mousePressEvent(QMouseEvent* event)
{
    if (!screenPicking_) {
        BaseDialog::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton) finishScreenPick(updateScreenColor());
    else if (event->button() == Qt::RightButton) finishScreenPick(false);
    event->accept();
}

void ColorEditorDialog::keyPressEvent(QKeyEvent* event)
{
    if (!screenPicking_) {
        BaseDialog::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape) finishScreenPick(false);
    else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        finishScreenPick(updateScreenColor());
    event->accept();
}

void ColorEditorDialog::pickScreenColor()
{
    if (screenPicking_) return;
    screenPickOriginal_ = color_;
    screenPickHsv_ = {hue_, saturation_, value_};
    if (!updateScreenColor()) {
        QMessageBox::warning(this, tr("Pick Screen Color"), tr("The screen color could not be read."));
        return;
    }
    screenPicking_ = true;
    grabMouse(Qt::CrossCursor);
    grabKeyboard();
    screenPickTimer_->start();
}

bool ColorEditorDialog::updateScreenColor()
{
    const QPoint position = QCursor::pos();
    auto* screen = QGuiApplication::screenAt(position);
    if (!screen) return false;
    const QPoint local = position - screen->geometry().topLeft();
    const QImage sample = screen->grabWindow(0, local.x(), local.y(), 1, 1).toImage();
    if (sample.isNull()) return false;
    QColor picked = sample.pixelColor(0, 0);
    picked.setAlphaF(screenPickOriginal_.alphaF());
    setCurrentColor(picked);
    return true;
}

void ColorEditorDialog::finishScreenPick(bool accept)
{
    screenPicking_ = false;
    screenPickTimer_->stop();
    releaseMouse();
    releaseKeyboard();
    if (!accept) {
        hue_ = screenPickHsv_[0];
        saturation_ = screenPickHsv_[1];
        value_ = screenPickHsv_[2];
        publishColor(screenPickOriginal_);
    }
}

void ColorEditorDialog::setCurrentColor(const QColor& color)
{
    if (!color.isValid()) return;
    const qreal hue = color.hsvHueF();
    if (hue >= 0.0) hue_ = hue;
    saturation_ = color.hsvSaturationF();
    value_ = color.valueF();
    publishColor(color.toRgb());
}

void ColorEditorDialog::publishHsv()
{
    publishColor(QColor::fromHsvF(hue_, saturation_, value_, color_.alphaF()).toRgb());
}

void ColorEditorDialog::publishColor(const QColor& color)
{
    const bool changed = color_ != color;
    color_ = color;
    const std::array<int, 6> values{qMin(359, qRound(hue_ * 360)), qRound(saturation_ * 100),
        qRound(value_ * 100), color.red(), color.green(), color.blue()};
    for (int i = 0; i < 6; ++i) {
        const QSignalBlocker blocker(channels_[i]);
        channels_[i]->setValue(values[i]);
    }
    if (alpha_->value() != color.alpha()) alpha_->setValue(color.alpha());
    picker_->setHsv(hue_, saturation_, value_);
    if (changed) emit currentColorChanged(color_);
}

}
