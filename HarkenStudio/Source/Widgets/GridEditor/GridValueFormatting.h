#pragma once

#include <QString>

#include <array>
#include <charconv>

namespace harken::studio {

inline QString formatGridValue(float value)
{
    std::array<char, 64> text;
    const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
    QString valueText = QString::fromLatin1(text.data(), static_cast<qsizetype>(result.ptr - text.data()));
    if (!valueText.contains(QLatin1Char('.'))) {
        const qsizetype exponent = valueText.indexOf(QLatin1Char('e'));
        valueText.insert(exponent < 0 ? valueText.size() : exponent, QStringLiteral(".0"));
    }
    return valueText;
}

}
