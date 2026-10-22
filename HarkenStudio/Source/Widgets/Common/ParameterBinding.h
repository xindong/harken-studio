#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace VNX {

class ParameterBinding : public QObject
{
    Q_OBJECT

public:
    explicit ParameterBinding(QObject* parent = nullptr);

    double value() const { return value_; }
    double minimum() const { return minimum_; }
    double maximum() const { return maximum_; }
    double defaultValue() const { return normalizedValue(defaultValue_); }
    int decimals() const { return decimals_; }
    double displayScale() const { return displayScale_; }
    double toDisplayValue(double value) const { return value * displayScale_; }
    double fromDisplayValue(double value) const { return value / displayScale_; }
    double quantum() const { return quantum_; }
    bool discreteSelection() const { return discreteSelection_; }
    QString unit() const { return unit_; }
    QString name() const { return name_; }
    QString annotationFor(double value) const;
    bool isEditing() const { return editing_; }
    bool mixedValue() const { return mixedValue_; }
    void setMixedValue(bool mixed) { mixedValue_ = mixed; }
    double editStartValue() const { return editStartValue_; }

    double valueFromPosition(double position) const;
    double positionFromValue(double value) const;
    // Integer position domain a pointer-tracking editor should span: a
    // quantized parameter gets one position per step, so the editor's own
    // pointer mapping already lands on values instead of being snapped back
    // after it has moved. smoothSteps is the resolution a continuous
    // parameter keeps.
    int positionSteps(int smoothSteps) const;
    int positionOf(double value, int steps) const;

    void setRange(double minimum, double maximum);
    void setMinimum(double minimum);
    void setMaximum(double maximum);
    void setDefaultValue(double value);
    void setDecimals(int decimals);
    void setDisplayScale(double scale);
    // Step size every value snaps to; 0 leaves the parameter continuous.
    void setQuantum(double quantum);
    void setDiscreteSelection(bool enabled);
    void setUnit(const QString& unit);
    void setName(const QString& name);
    // Optional value -> human-readable text (a Variable's label anchors);
    // presenters that can show it read it through annotationFor().
    void setAnnotation(std::function<QString(double)> annotation);

public slots:
    void setValue(double value);
    void commitValue(double value);
    void beginEdit();
    void previewEdit(double value);
    void commitEdit();
    void cancelEdit();

signals:
    void valueUpdated(double value);
    void valueCommitted(double value);
    void metadataChanged();
    void editBegan(double startValue);
    void editPreviewed(double value);
    void editEnded(double startValue, double endValue, bool committed);

private:
    double normalizedValue(double value) const;
    bool updateValue(double value);
    void interruptEdit();

    double value_ = 0.0;
    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double defaultValue_ = 0.0;
    int decimals_ = 2;
    double displayScale_ = 1.0;
    double quantum_ = 0.0;
    bool discreteSelection_ = false;
    QString unit_;
    QString name_;
    std::function<QString(double)> annotation_;
    bool editing_ = false;
    bool mixedValue_ = false;
    double editStartValue_ = 0.0;
};

}
