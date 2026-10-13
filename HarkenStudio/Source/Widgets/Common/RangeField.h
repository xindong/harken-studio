
#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QLabel;

namespace VNX {

// Paired low/high numeric editor for a single declared domain (a Variable's
// range). The pair is one value: it reports both bounds together so the owner
// commits them atomically instead of passing through a half-edited domain.
class RangeField : public QWidget
{
    Q_OBJECT

public:
    explicit RangeField(QWidget* parent = nullptr);

    void setDomain(double minimum, double maximum, int decimals, double singleStep);
    void setValues(double low, double high);

    double lowValue() const;
    double highValue() const;

signals:
    void valuesCommitted(double low, double high);

private:
    void applyTheme();

    QDoubleSpinBox* low_ = nullptr;
    QDoubleSpinBox* high_ = nullptr;
    QLabel* separator_ = nullptr;
};

}
