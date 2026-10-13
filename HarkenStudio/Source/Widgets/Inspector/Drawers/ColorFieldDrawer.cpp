#include "ColorFieldDrawer.h"

#include "Core/QtControlSkin.h"
#include "Widgets/Common/LayoutDebugTint.h"

#include <QColor>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVariant>

namespace VNX {

namespace {

void applyVisual(QPushButton* swatch, uint32_t argb)
{
    const QColor color = QColor::fromRgba(argb);
    const QString hex = QStringLiteral("0x%1")
        .arg(argb, 8, 16, QChar('0')).toUpper();
    QtControlSkin::setSemanticProperty(swatch, "fieldRole",
                                      QStringLiteral("colorSwatch"));
    swatch->setStyleSheet(QtControlSkin::colorSwatchStyle(color));
    swatch->setText(hex);
    swatch->setProperty("harkenColor",
                        QVariant::fromValue<qint64>(static_cast<qint64>(argb)));
}

}

QWidget* ColorFieldDrawer::build(const PropertyDescriptor& d, QWidget* parent)
{
    auto* container = new QWidget(parent);
    DebugTint::tintContainer(container, DebugTint::Role::Flex);
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* swatch = new QPushButton(container);
    swatch->setFixedWidth(72);
    swatch->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    swatch->setObjectName(QStringLiteral("prop_") + d.field);
    container->setObjectName(QStringLiteral("prop_") + d.field + QStringLiteral("_container"));

    applyVisual(swatch, 0xFF808080u);

    QObject::connect(swatch, &QPushButton::clicked, swatch, [swatch]() {
        const qint64 cur = swatch->property("harkenColor").toLongLong();
        const QColor before = QColor::fromRgba(static_cast<uint32_t>(cur));
        const QColor picked = QColorDialog::getColor(
            before, swatch, QObject::tr("Pick Color"),
            QColorDialog::ShowAlphaChannel);
        if (!picked.isValid()) return;
        applyVisual(swatch, picked.rgba());
    });

    layout->addWidget(swatch, 0, Qt::AlignLeft);
    return container;
}

void ColorFieldDrawer::bind(QWidget* widget, const QVariant& value, ProjectItem*)
{
    if (!widget) return;
    auto* swatch = widget->findChild<QPushButton*>();
    if (!swatch) return;
    const qint64 raw = value.toLongLong();
    swatch->blockSignals(true);
    applyVisual(swatch, static_cast<uint32_t>(raw));
    swatch->blockSignals(false);
}

void ColorFieldDrawer::connectChanged(QWidget* widget,
                                      std::function<void(const QVariant&)> onChange)
{
    if (!widget) return;
    auto* swatch = widget->findChild<QPushButton*>();
    if (!swatch) return;
    QObject::connect(swatch, &QPushButton::clicked, swatch,
        [swatch, onChange = std::move(onChange)]() {
            const qint64 raw = swatch->property("harkenColor").toLongLong();
            onChange(QVariant::fromValue<qint64>(raw));
        });
}

}
