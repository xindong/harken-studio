
#include "RangeField.h"
#include "Core/FontManager.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"

#include "DecimalSpinBox.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>

namespace VNX {

namespace {

QDoubleSpinBox* makeBound(QWidget* parent, const QString& objectName)
{
    auto* spin = new DecimalSpinBox(parent);
    spin->setObjectName(objectName);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    spin->setKeyboardTracking(false);
    spin->setFont(FontManager::instance().getFont(FontRole::Mono));
    spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return spin;
}

}

RangeField::RangeField(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("RangeField"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    low_ = makeBound(this, QStringLiteral("rangeFieldLow"));
    high_ = makeBound(this, QStringLiteral("rangeFieldHigh"));
    separator_ = new QLabel(QStringLiteral("–"), this);
    separator_->setObjectName(QStringLiteral("rangeFieldSeparator"));
    separator_->setAlignment(Qt::AlignCenter);
    separator_->setFixedWidth(
        ThemeManager::instance().metrics().inspector.rangeSeparatorWidth);
    separator_->setProperty("textRole", QStringLiteral("muted"));

    layout->addWidget(low_, 1);
    layout->addWidget(separator_);
    layout->addWidget(high_, 1);

    setDomain(-1000000.0, 1000000.0, 2, 1.0);

    const auto commit = [this] {
        emit valuesCommitted(low_->value(), high_->value());
    };
    connect(low_, &QDoubleSpinBox::valueChanged, this, commit);
    connect(high_, &QDoubleSpinBox::valueChanged, this, commit);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] { applyTheme(); });
}

void RangeField::setDomain(double minimum, double maximum, int decimals,
                           double singleStep)
{
    const QSignalBlocker lowBlock(low_);
    const QSignalBlocker highBlock(high_);
    for (QDoubleSpinBox* spin : {low_, high_})
    {
        spin->setDecimals(decimals);
        spin->setRange(minimum, maximum);
        spin->setSingleStep(singleStep);
    }
}

void RangeField::setValues(double low, double high)
{
    const QSignalBlocker lowBlock(low_);
    const QSignalBlocker highBlock(high_);
    low_->setValue(low);
    high_->setValue(high);
}

double RangeField::lowValue() const
{
    return low_->value();
}

double RangeField::highValue() const
{
    return high_->value();
}

void RangeField::applyTheme()
{
    QtControlSkin::setSemanticProperty(low_, "fieldRole",
                                      QStringLiteral("inspectorValue"));
    QtControlSkin::setSemanticProperty(high_, "fieldRole",
                                      QStringLiteral("inspectorValue"));
}

}
