#pragma once

#include "SignalPathGraph.h"

#include <QObject>
#include <memory>

namespace VNX {

struct FieldGroupEffectiveSnapshot;

std::optional<float> signalPathGainValue(
    const ProjectData& project, const SignalPathGraph& graph, size_t edgeIndex,
    const ProjectItem* previewItem = nullptr,
    const DefinitionFieldGroups* previewGroups = nullptr);

std::optional<float> signalPathGainValue(
    const FieldGroupEffectiveSnapshot& snapshot, const SignalPathGraph& graph,
    size_t edgeIndex, bool previewing = false);

class SignalPathGainEditSession final : public QObject
{
public:
    SignalPathGainEditSession(const SignalPathGraph& graph, size_t edgeIndex,
                             const QObject* originator = nullptr);
    ~SignalPathGainEditSession() override;

    bool isActive() const;
    void preview(double value);
    void commit();
    void cancel();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}
