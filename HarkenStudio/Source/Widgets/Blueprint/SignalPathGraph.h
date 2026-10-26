#pragma once

#include "Core/ProjectTypes.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace VNX {

struct FieldGroupEffectiveSnapshot;

struct SignalPathIdentity
{
    QUuid sourceGuid;
    std::vector<PlayablePropertyLocatorV1> occurrences;

    bool operator==(const SignalPathIdentity&) const = default;
};

struct SignalPathSend
{
    uint32_t targetBusID = 0;
    float sendLevel = 0.0f;

    bool operator==(const SignalPathSend&) const = default;
};

struct SignalPathRouting
{
    uint32_t outputBusID = 0;
    std::vector<SignalPathSend> sends;

    bool operator==(const SignalPathRouting&) const = default;
};

struct SignalPathNodeKey
{
    QUuid objectGuid;

    bool operator==(const SignalPathNodeKey&) const = default;
};

struct SignalPathPortPair
{
    std::optional<SignalPathRouting> routing;
    std::vector<size_t> pathIndices;
};

struct SignalPathNode
{
    SignalPathNodeKey key;
    uint32_t objectID = 0;
    ProjectItemType type = ProjectItemType::None;
    QString name;
    std::vector<SignalPathPortPair> portPairs;
};

struct SignalPathEndpoint
{
    size_t occurrenceIndex = 0;
    size_t portPairIndex = 0;

    bool operator==(const SignalPathEndpoint&) const = default;
};

enum class SignalPathEdgeKind
{
    Through,
    Output,
    Send
};

struct SignalPathRoutingSource
{
    QUuid objectGuid;
    FieldGroupScope scope;

    bool operator==(const SignalPathRoutingSource&) const = default;
};

struct SignalPathEdge
{
    SignalPathEndpoint from;
    SignalPathEndpoint to;
    SignalPathEdgeKind kind = SignalPathEdgeKind::Through;
    float sendLevel = 0.0f;
    std::vector<size_t> pathIndices;
    std::optional<SignalPathRoutingSource> routingSource;
};

struct SignalPath
{
    SignalPathIdentity identity;
    SignalPathRouting routing;
    std::vector<SignalPathEndpoint> computation;
    std::vector<size_t> edgeIndices;
};

struct SignalPathDiagnostic
{
    std::optional<SignalPathIdentity> path;
    QString message;
};

struct SignalPathGraph
{
    FieldGroupScope platform;
    std::vector<SignalPathNode> nodes;
    std::vector<SignalPathEdge> edges;
    std::vector<SignalPath> paths;
    std::vector<SignalPathDiagnostic> diagnostics;

    bool ok() const { return diagnostics.empty(); }
};

SignalPathGraph buildSignalPathGraph(
    const ProjectData& project, const std::vector<SignalPathIdentity>& sources,
    FieldGroupScope platform = {}, const std::vector<uint32_t>& busRoots = {},
    std::shared_ptr<const FieldGroupEffectiveSnapshot> groups = {});

}
