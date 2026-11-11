
#pragma once

#include "ParameterSlider.h"

namespace VNX {

class VVariable;

class RehearsalSlider : public ParameterSlider
{
    Q_OBJECT

public:
    explicit RehearsalSlider(QWidget* parent = nullptr);

    void configure(const VVariable& variable);
    float rawValue() const;
    void setRawValue(float value);
};

}
