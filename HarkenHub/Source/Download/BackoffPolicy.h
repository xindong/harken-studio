#pragma once

namespace VNH {

class BackoffPolicy
{
public:
    int baseDelayMs = 1000;
    int maxDelayMs = 60000;
    double jitterFactor = 0.5;

    int delayMsForAttempt(int attempt, double jitterUnit) const;
    int sampleDelayMs(int attempt) const;
};

}
