#include "AmplitudeCurvePopup.h"

#include "DecimalSpinBox.h"
#include "DirectJumpSlider.h"
#include "Core/QtControlSkin.h"

#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QScreen>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace VNX {

QAction* addAmplitudePowerLawAction(QMenu* menu, QWidget* owner, const QPoint& globalPosition,
    const QRect& globalAxisBounds, double exponent, std::function<void(double)> changed)
{
    auto* action = menu->addAction(QObject::tr("Level scale"));
    action->setObjectName(QStringLiteral("amplitudeCurveAction"));
    QObject::connect(action, &QAction::triggered, owner,
        [owner, globalPosition, globalAxisBounds, exponent, changed = std::move(changed)] {
        auto* popup = new QWidget(owner, Qt::Popup);
        popup->setObjectName(QStringLiteral("amplitudeCurvePopup"));
        popup->setAttribute(Qt::WA_DeleteOnClose);
        popup->setAttribute(Qt::WA_StyledBackground);
        QtControlSkin::setSemanticProperty(popup, "surfaceRole", QStringLiteral("popup"));

        auto* layout = new QVBoxLayout(popup);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(6);
        auto* decibels = new QLabel(QObject::tr("Linear\ndecibels"), popup);
        decibels->setAlignment(Qt::AlignCenter);
        QtControlSkin::setSemanticProperty(decibels, "textRole", QStringLiteral("secondary"));
        layout->addWidget(decibels);

        auto* slider = new DirectJumpSlider(Qt::Vertical, popup);
        slider->setObjectName(QStringLiteral("amplitudeCurveSlider"));
        slider->setAccessibleName(QObject::tr("Level scale"));
        slider->setRange(0, 100);
        slider->setSingleStep(1);
        slider->setPageStep(10);
        slider->setInvertedAppearance(true);
        slider->setInvertedControls(true);
        slider->setValue(int(std::lround(std::clamp(exponent, 0.0, 1.0) * 100.0)));
        slider->setMinimumHeight(120);
        QtControlSkin::setSemanticProperty(slider, "sliderRole", QStringLiteral("parameter"));
        layout->addWidget(slider, 1, Qt::AlignHCenter);

        auto* amplitude = new QLabel(QObject::tr("Linear\namplitude"), popup);
        amplitude->setAlignment(Qt::AlignCenter);
        QtControlSkin::setSemanticProperty(amplitude, "textRole", QStringLiteral("secondary"));
        layout->addWidget(amplitude);
        auto* value = new DecimalSpinBox(popup);
        value->setObjectName(QStringLiteral("amplitudeCurveValue"));
        value->setAccessibleName(QObject::tr("Level scale"));
        value->setRange(0.0, 1.0);
        value->setDecimals(2);
        value->setSingleStep(0.01);
        value->setButtonSymbols(QAbstractSpinBox::NoButtons);
        value->setAlignment(Qt::AlignCenter);
        value->setKeyboardTracking(false);
        value->setValue(slider->value() / 100.0);
        QtControlSkin::setSemanticProperty(value, "fieldRole", QStringLiteral("compact"));
        layout->addWidget(value);
        QObject::connect(slider, &QSlider::valueChanged, value, [value](int position) {
            value->setValue(position / 100.0);
        });
        QObject::connect(value, &QDoubleSpinBox::valueChanged, popup, [slider, changed](double next) {
            const QSignalBlocker blocker(slider);
            slider->setValue(int(std::lround(next * 100.0)));
            changed(next);
        });
        popup->adjustSize();
        popup->resize(popup->width(), std::clamp(globalAxisBounds.height(), popup->height(),
            std::max(320, popup->height())));
        QPoint position(globalAxisBounds.left() - popup->width() - 4,
            globalPosition.y() - popup->height() / 2);
        if (const auto* screen = QGuiApplication::screenAt(globalPosition)) {
            const QRect available = screen->availableGeometry();
            position.setX(std::clamp(position.x(), available.left(),
                std::max(available.left(), available.right() - popup->width() + 1)));
            position.setY(std::clamp(position.y(), available.top(),
                std::max(available.top(), available.bottom() - popup->height() + 1)));
        }
        popup->move(position);
        popup->show();
        slider->setFocus();
    });
    return action;
}

void showAmplitudeCurveMenu(QWidget* owner, const QPoint& globalPosition,
    const QRect& globalAxisBounds, double exponent, std::function<void(double)> changed)
{
    auto* menu = new QMenu(owner);
    menu->setObjectName(QStringLiteral("amplitudeCurveMenu"));
    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    addAmplitudePowerLawAction(menu, owner, globalPosition, globalAxisBounds, exponent, std::move(changed));
    menu->popup(globalPosition);
}

}
