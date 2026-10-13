#pragma once

#include "ParameterControlHost.h"

namespace VNX {

class ParameterSlider : public ParameterControlHost
{
    Q_OBJECT
    Q_PROPERTY(double value READ value WRITE setValue NOTIFY valueChanged)
    Q_PROPERTY(double minimum READ minimum WRITE setMinimum)
    Q_PROPERTY(double maximum READ maximum WRITE setMaximum)
    Q_PROPERTY(QString suffix READ suffix WRITE setSuffix)

public:
    explicit ParameterSlider(const QString& label, QWidget* parent = nullptr,
                             ThemeManager* themeManager = nullptr);
    ~ParameterSlider() override;

    double value() const;
    double minimum() const;
    double maximum() const;
    QString suffix() const;

    void setRange(double minimum, double maximum, int decimals = 2);
    void setMinimum(double minimum);
    void setMaximum(double maximum);
    void setSuffix(const QString& suffix);
    void setDecimals(int decimals);

    void setMixedValue(bool mixed);
    bool mixedValue() const { return mixedValue_; }
    void cancelPendingInput();

public slots:
    void setValue(double value);
    void simulateValueChange(double value);

signals:
    void valueChanged(double value);
    void interactiveEditBegin(double startValue);
    void interactivePreview(double currentValue);
    void interactiveEditEnd(double startValue, double endValue, bool committed);

protected:
    void presenterInstalled() override;

private:
    void applyMixedDisplay();
    void clearMixedDisplay();
    void onMixedInteractionStarted();

    bool mixedValue_ = false;
    bool mixedTextEdited_ = false;
};

}
