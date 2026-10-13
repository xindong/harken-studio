#pragma once

#include "ProjectPlatform.h"

#include <QSet>
#include <QStringList>
#include <QVector>

#include <optional>

namespace VNX {

struct PlatformContext {
    QVector<ProjectPlatformId> selected;
    ProjectPlatformId          primary = ProjectPlatformId{};
    bool              all = false;
};

inline QVector<ProjectPlatformId> occupiedTargetPlatforms(const QStringList& targetPlatforms)
{
    QVector<ProjectPlatformId> targets;
    QSet<ProjectPlatformId> seen;
    for (const QString& name : targetPlatforms) {
        const ProjectPlatformId platform = QUuid::fromString(name);
        if (platform == ProjectPlatformId{})
            continue;
        if (seen.contains(platform))
            continue;
        seen.insert(platform);
        targets.append(platform);
    }
    return targets;
}

inline bool selectedEqualsTargetPlatforms(const QVector<ProjectPlatformId>& selected,
                                          const QStringList& targetPlatforms)
{
    QSet<ProjectPlatformId> selectedSet;
    for (ProjectPlatformId platform : selected)
        selectedSet.insert(platform);

    QSet<ProjectPlatformId> targetSet;
    for (ProjectPlatformId platform : occupiedTargetPlatforms(targetPlatforms))
        targetSet.insert(platform);

    return selectedSet == targetSet;
}

inline bool selectedIsSubsetOfTargetPlatforms(const QVector<ProjectPlatformId>& selected,
                                              const QStringList& targetPlatforms)
{
    QSet<ProjectPlatformId> targetSet;
    for (ProjectPlatformId platform : occupiedTargetPlatforms(targetPlatforms))
        targetSet.insert(platform);

    for (ProjectPlatformId platform : selected) {
        if (!targetSet.contains(platform))
            return false;
    }
    return true;
}

inline bool operator==(const PlatformContext& lhs, const PlatformContext& rhs)
{
    return lhs.selected == rhs.selected
        && lhs.primary == rhs.primary
        && lhs.all == rhs.all;
}

inline bool operator!=(const PlatformContext& lhs, const PlatformContext& rhs)
{
    return !(lhs == rhs);
}

inline PlatformScope currentWorldPlatformScope(const PlatformContext& context)
{
    if (context.selected.isEmpty() || context.primary == ProjectPlatformId{})
        return std::nullopt;
    return context.primary;
}

inline PlatformContext resolvePlatformContext(const QStringList& persistedSelected,
                                              const QString&     persistedPrimary,
                                              const QStringList& targetPlatforms,
                                              std::optional<bool> persistedAll = std::nullopt)
{
    const QVector<ProjectPlatformId> targets = occupiedTargetPlatforms(targetPlatforms);
    if (targets.isEmpty())
        return {};

    QSet<ProjectPlatformId> persistedSet;
    for (const QString& name : persistedSelected) {
        const ProjectPlatformId platform = QUuid::fromString(name);
        if (platform == ProjectPlatformId{})
            continue;
        persistedSet.insert(platform);
    }

    bool hasIntersection = false;
    bool persistedCoversTargets = !targets.isEmpty();
    for (ProjectPlatformId platform : targets) {
        if (persistedSet.contains(platform))
            hasIntersection = true;
        else
            persistedCoversTargets = false;
    }
    bool stickyAll = persistedAll.value_or(
        persistedSet.isEmpty() || persistedCoversTargets);
    if (!hasIntersection)
        stickyAll = true;

    QVector<ProjectPlatformId> selected;
    if (stickyAll) {
        selected = targets;
    } else {
        for (ProjectPlatformId platform : targets) {
            if (persistedSet.contains(platform))
                selected.append(platform);
        }
        const ProjectPlatformId persisted = QUuid::fromString(persistedPrimary);
        if (selected.size() != 1) {
            if (persisted != ProjectPlatformId{} && selected.contains(persisted))
                selected = {persisted};
            else if (!selected.isEmpty())
                selected = {selected.front()};
            else
                selected = {targets.front()};
        }
    }

    const ProjectPlatformId persisted = QUuid::fromString(persistedPrimary);
    ProjectPlatformId primary = selected.front();
    if (persisted != ProjectPlatformId{} && selected.contains(persisted))
        primary = persisted;

    return {selected, primary, stickyAll};
}

}
