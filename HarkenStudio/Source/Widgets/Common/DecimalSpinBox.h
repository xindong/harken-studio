#pragma once

#include "Core/NumericFormat.h"

#include <QDoubleSpinBox>

namespace harken::studio {

class DecimalSpinBox : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    QString textFromValue(double value) const override
    {
        QLocale numberLocale = locale();
        if (!isGroupSeparatorShown())
            numberLocale.setNumberOptions(numberLocale.numberOptions()
                                          | QLocale::OmitGroupSeparator);
        return formatDecimal(value, decimals(), numberLocale);
    }
};

}
