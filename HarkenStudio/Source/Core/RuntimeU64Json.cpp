#include "RuntimeU64Json.h"

#include <QString>

#include <limits>

namespace VNX {

bool parseRuntimeU64DecimalStringV1(const QJsonValue& value,
                                    uint64_t* result) noexcept
{
    if (!result || !value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > 20) return false;
    if (text.size() > 1 && text.front() == QLatin1Char('0')) return false;

    uint64_t parsed = 0;
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    for (const QChar character : text) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9'))
            return false;
        const uint64_t digit = static_cast<uint64_t>(
            character.unicode() - QLatin1Char('0').unicode());
        if (parsed > (maximum - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    *result = parsed;
    return true;
}

QJsonValue formatRuntimeU64DecimalStringV1(uint64_t value)
{
    return QJsonValue(QString::number(value));
}

}
