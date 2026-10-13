#pragma once

#include <QMetaType>
#include <QString>

namespace VNX {

enum class AIStepPhase : quint8
{
    ModelWaiting,
    ResumedModelRound,
    ToolExecution,
    ConfirmationWaiting,
    Completed,
    Failed,
    Cancelled
};

struct AIStepEvent
{
    static constexpr int kMaximumRequestIdCharacters = 64;
    static constexpr int kMaximumCommandCharacters = 128;
    static constexpr int kMaximumToolCount = 64;
    // Upper bound for the reported per-turn work budget; kept in step with the
    // service's maxToolExecutions_/maxModelRounds_ so the progress event conveys
    // the real budget rather than a clamped-down number.
    static constexpr int kMaximumToolLimit = 100;
    static constexpr int kMaximumModelRounds = 100;

    AIStepPhase phase = AIStepPhase::ModelWaiting;
    QString requestId;
    QString command;
    int modelRound = 0;
    int toolCount = 0;
    int toolLimit = 0;
    int toolExecutionCount = 0;

    AIStepEvent() = default;
    AIStepEvent(AIStepPhase phaseValue, const QString& requestIdValue,
                const QString& commandValue, int modelRoundValue,
                int toolCountValue, int toolLimitValue,
                int toolExecutionCountValue)
        : phase(phaseValue),
          requestId(requestIdValue.left(kMaximumRequestIdCharacters)),
          command(commandValue.left(kMaximumCommandCharacters)),
          modelRound(qBound(0, modelRoundValue, kMaximumModelRounds)),
          toolCount(qBound(0, toolCountValue, kMaximumToolCount)),
          toolLimit(qBound(0, toolLimitValue, kMaximumToolLimit)),
          toolExecutionCount(qBound(0, toolExecutionCountValue, kMaximumToolLimit))
    {
    }
};

struct AIServiceDiagnostic
{
    static constexpr int kMaximumCodeCharacters = 64;
    static constexpr int kMaximumMessageCharacters = 256;

    QString code;
    QString message;

    AIServiceDiagnostic() = default;
    AIServiceDiagnostic(const QString& codeValue, const QString& messageValue)
        : code(codeValue.left(kMaximumCodeCharacters)),
          message(messageValue.left(kMaximumMessageCharacters))
    {
    }
};

}

Q_DECLARE_METATYPE(VNX::AIStepEvent)
Q_DECLARE_METATYPE(VNX::AIServiceDiagnostic)
