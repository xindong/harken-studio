#include "Download/BackoffPolicy.h"

#include <QRandomGenerator>

#include <algorithm>

namespace VNH {

int BackoffPolicy::delayMsForAttempt(int attempt, double jitterUnit) const
{
    const int base = std::max(baseDelayMs, 1);
    const int cap = std::max(maxDelayMs, base);
    const double unit = std::clamp(jitterUnit, 0.0, 1.0);
    const double factor = std::clamp(jitterFactor, 0.0, 1.0);

    qint64 delay = base;
    for (int i = 0; i < std::max(attempt, 0) && delay < cap; ++i)
        delay *= 2;
    delay = std::min<qint64>(delay, cap);

    const double scaled = double(delay) * (1.0 - factor * unit);
    return std::max(1, int(scaled));
}

int BackoffPolicy::sampleDelayMs(int attempt) const
{
    return delayMsForAttempt(attempt, QRandomGenerator::global()->generateDouble());
}

}
