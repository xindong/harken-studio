
#pragma once

#include "GridSemantics.h"
#include "Timeline/TimelineTypes.h"
#include "TransitionSemantics.h"

#include <QHash>
#include <QObject>
#include <QUuid>

#include <optional>

namespace VNX {

class VariableRehearsal;

class TransitionRehearsal : public QObject
{
    Q_OBJECT

public:
    explicit TransitionRehearsal(QObject* parent = nullptr);

    std::optional<QUuid> previousWinner(const QUuid& containerId) const;
    std::optional<QUuid> currentWinner(const QUuid& containerId) const;

    void refreshWinner(const QUuid& containerId, std::optional<QUuid> winner);

    TransitionSemantics::HandoverResult handoverFor(
        const QUuid& containerId, const TransitionTableDef& table) const;

    void clear(const QUuid& containerId);
    void clearAll();

    static std::optional<QUuid> winnerForContainer(
        const ProjectItem& container,
        const VariableRehearsal& variableRehearsal,
        const TimelineItemResolver& resolveVariable, PlatformScope scope = std::nullopt);

    static std::optional<QUuid> queueIllustrativeWinner(const ProjectItem& container, PlatformScope scope = std::nullopt);

    static std::optional<QUuid> variableControlWinner(
        const DiscreteGridDef& grid,
        const VariableRehearsal& variableRehearsal,
        const TimelineItemResolver& resolveVariable);

signals:
    void rehearsalChanged(const QUuid& containerId);

private:
    struct WinnerState
    {
        std::optional<QUuid> previousWinner;
        std::optional<QUuid> currentWinner;
    };

    QHash<QUuid, WinnerState> states_;
};

}
