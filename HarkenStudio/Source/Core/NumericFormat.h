#pragma once

#include <QLocale>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace VNX {

inline int axisDecimalsForStep(double step, int maximumDecimals = 9)
{
    if (step <= 0.0 || !std::isfinite(step))
        return 0;
    return static_cast<int>(std::clamp(std::ceil(-std::log10(step) - 1e-9),
        0.0, double(maximumDecimals)));
}

inline int axisReadoutDecimalsForStep(double step, int maximumDecimals = 9)
{
    return std::min(maximumDecimals, axisDecimalsForStep(step, maximumDecimals) + 1);
}

inline QStringList compactAxisLabels(QStringList labels)
{
    static const QRegularExpression number(
        QStringLiteral("^([+-]?[0-9]+)(?:\\.([0-9]+))?((?:[eE][+-]?[0-9]+)?[^0-9.]*)$"));
    int decimals = 0;
    for (const QString& label : labels) {
        const auto match = number.match(label);
        if (!match.hasMatch()) continue;
        QString fraction = match.captured(2);
        while (fraction.endsWith(QLatin1Char('0')))
            fraction.chop(1);
        decimals = std::max(decimals, static_cast<int>(fraction.size()));
    }
    for (QString& label : labels) {
        const auto match = number.match(label);
        if (!match.hasMatch()) continue;
        QString integer = match.captured(1);
        const QString fraction = match.captured(2).leftJustified(decimals, QLatin1Char('0'), true);
        if (integer == QStringLiteral("-0") && fraction.toDouble() == 0.0)
            integer = QStringLiteral("0");
        label = integer + (decimals > 0 ? QLatin1Char('.') + fraction : QString())
            + match.captured(3);
    }
    return labels;
}

inline QStringList formatAxisValues(const QVector<double>& values, int maximumDecimals)
{
    QStringList labels;
    labels.reserve(values.size());
    for (double value : values)
        labels.append(QString::number(value, 'f', maximumDecimals));
    return compactAxisLabels(std::move(labels));
}

inline QString formatAxisValue(double value, int maximumDecimals)
{
    return formatAxisValues({value}, maximumDecimals).front();
}

inline QString formatDecimal(double value, int decimals,
                             const QLocale& locale = QLocale::c())
{
    QString text = locale.toString(value, 'f', decimals);
    if (decimals <= 1) return text;
    const QString point = locale.decimalPoint();
    const qsizetype pointIndex = text.indexOf(point);
    if (pointIndex < 0) return text;
    const QString zero = locale.zeroDigit();
    const qsizetype minimumSize = pointIndex + point.size() + zero.size();
    while (text.size() > minimumSize && text.endsWith(zero))
        text.chop(zero.size());
    return text;
}

}
