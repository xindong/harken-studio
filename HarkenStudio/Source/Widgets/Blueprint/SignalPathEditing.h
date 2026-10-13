#pragma once

#include "SignalPathGraph.h"
#include "Core/FieldGroupEditing.h"
#include "Core/Components/VSelection.h"

namespace VNX {

enum class SignalPathConnectGesture { Output, Input, Node };
enum class SignalPathConnectTarget { Body, Input, Output };

struct SignalPathConnectRequest
{
    SignalPathEndpoint source;
    SignalPathEndpoint target;
    SignalPathConnectGesture gesture = SignalPathConnectGesture::Output;
    SignalPathConnectTarget targetPart = SignalPathConnectTarget::Input;
    bool addSend = false;
};

struct SignalPathReparent
{
    QUuid object;
    QUuid beforeParent;
    QUuid afterParent;
    std::optional<DefinitionFieldGroups> rootGroups;
};

struct SignalPathDetach
{
    QUuid object;
    QUuid parent;
    DefinitionFieldGroups rootGroups;
};

struct SignalPathBusSendRemoval
{
    QUuid from;
    QUuid to;
    float level = 0.0f;
};

struct SignalPathClipSnapshot
{
    QUuid owner;
    ClipLedger clips;
};

struct SignalPathSelectionSnapshot
{
    QUuid owner;
    SelectionData selections;
    SelectionMode mode = SelectionMode::Random;
    SelectionBehaviorDef behavior;
};

struct SignalPathConnectPlan
{
    QUuid from;
    QUuid to;
    bool busSend = false;
    bool disconnect = false;
    std::vector<SignalPathReparent> moves;
    std::vector<SignalPathDetach> detachments;
    std::vector<FieldGroupParentSnapshot> busOutputResets;
    std::vector<SignalPathBusSendRemoval> busSendRemovals;
    std::vector<SignalPathClipSnapshot> observedClips;
    std::vector<SignalPathSelectionSnapshot> observedSelections;
    std::vector<FieldGroupDefinitionSnapshot> observedGroups;
    std::vector<FieldGroupParentSnapshot> observedParents;
    PlatformScope occurrenceScope;
    std::vector<PlayablePropertyLocatorV1> observedOccurrences;
    std::vector<FieldGroupDefinitionSnapshot> before;
    std::vector<FieldGroupDefinitionSnapshot> after;
    QString description;
    QString error;

    bool ok() const { return error.isEmpty() && !from.isNull() && !to.isNull(); }
};

SignalPathConnectPlan prepareSignalPathConnect(const SignalPathGraph& graph,
    SignalPathEndpoint from, SignalPathEndpoint to, bool addSend);
SignalPathConnectPlan prepareSignalPathConnect(const SignalPathGraph& graph,
    const SignalPathConnectRequest& request);
SignalPathConnectPlan prepareSignalPathNodeDrop(const SignalPathGraph& graph,
    const std::vector<SignalPathEndpoint>& sources, SignalPathEndpoint target);
SignalPathConnectPlan prepareSignalPathDisconnect(const SignalPathGraph& graph,
    const std::vector<size_t>& edgeIndices);
bool applySignalPathConnect(SignalPathConnectPlan plan);

}
