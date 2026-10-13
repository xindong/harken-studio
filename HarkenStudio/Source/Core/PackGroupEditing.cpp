
#include "PackGroupEditing.h"

#include <algorithm>

namespace VNX {
namespace PackGroupEditing {

namespace {

PackGroup* findGroup(std::vector<PackGroup>& groups, const QString& name)
{
    for (auto& group : groups)
        if (group.name == name) return &group;
    return nullptr;
}

bool nameTaken(const std::vector<PackGroup>& groups, const QString& name,
               const PackGroup* except)
{
    for (const auto& group : groups) {
        if (&group == except) continue;
        if (group.name == name) return true;
    }
    return false;
}

} // namespace

PackGroupEditResult createGroup(std::vector<PackGroup>& groups, const QString& name)
{
    if (name.isEmpty())
        return PackGroupEditResult::failure(QStringLiteral("INVALID_ARGS"),
            QStringLiteral("Group name must not be empty"));
    if (nameTaken(groups, name, nullptr))
        return PackGroupEditResult::failure(QStringLiteral("DUPLICATE_NAME"),
            QStringLiteral("A pack group named '%1' already exists").arg(name));

    PackGroup group;
    group.name = name;
    groups.push_back(std::move(group));
    return PackGroupEditResult::success();
}

PackGroupEditResult deleteGroup(std::vector<PackGroup>& groups, const QString& name)
{
    auto it = std::find_if(groups.begin(), groups.end(),
                           [&](const PackGroup& g) { return g.name == name; });
    if (it == groups.end())
        return PackGroupEditResult::failure(QStringLiteral("GROUP_NOT_FOUND"),
            QStringLiteral("Pack group '%1' does not exist").arg(name));

    groups.erase(it);
    return PackGroupEditResult::success();
}

PackGroupEditResult renameGroup(std::vector<PackGroup>& groups,
                                const QString& oldName, const QString& newName)
{
    if (newName.isEmpty())
        return PackGroupEditResult::failure(QStringLiteral("INVALID_ARGS"),
            QStringLiteral("Group name must not be empty"));

    PackGroup* group = findGroup(groups, oldName);
    if (!group)
        return PackGroupEditResult::failure(QStringLiteral("GROUP_NOT_FOUND"),
            QStringLiteral("Pack group '%1' does not exist").arg(oldName));

    if (nameTaken(groups, newName, group))
        return PackGroupEditResult::failure(QStringLiteral("DUPLICATE_NAME"),
            QStringLiteral("A pack group named '%1' already exists").arg(newName));

    group->name = newName;
    return PackGroupEditResult::success();
}

PackGroupEditResult addActions(std::vector<PackGroup>& groups,
                               const QString& groupName,
                               const std::vector<harkenstudio::ObjectId>& actionIds,
                               const std::set<harkenstudio::ObjectId>& validActionIds)
{
    PackGroup* target = findGroup(groups, groupName);
    if (!target)
        return PackGroupEditResult::failure(QStringLiteral("GROUP_NOT_FOUND"),
            QStringLiteral("Pack group '%1' does not exist").arg(groupName));

    std::vector<size_t> sizesBefore;
    sizesBefore.reserve(groups.size());
    for (const auto& group : groups)
        sizesBefore.push_back(group.actionIds.size());

    for (harkenstudio::ObjectId id : actionIds) {
        if (!validActionIds.count(id)) continue;

        for (auto& group : groups) {
            if (&group == target) continue;
            auto& ids = group.actionIds;
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        }

        if (std::find(target->actionIds.begin(), target->actionIds.end(), id)
                == target->actionIds.end())
            target->actionIds.push_back(id);
    }

    for (size_t i = groups.size(); i-- > 0;) {
        if (groups[i].name == groupName)
            continue;
        if (groups[i].actionIds.size() < sizesBefore[i]
            && groups[i].actionIds.size() <= 1)
            groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return PackGroupEditResult::success();
}

PackGroupEditResult removeActions(std::vector<PackGroup>& groups,
                                  const QString& groupName,
                                  const std::vector<harkenstudio::ObjectId>& actionIds)
{
    PackGroup* target = findGroup(groups, groupName);
    if (!target)
        return PackGroupEditResult::failure(QStringLiteral("GROUP_NOT_FOUND"),
            QStringLiteral("Pack group '%1' does not exist").arg(groupName));

    const std::set<harkenstudio::ObjectId> toRemove(actionIds.begin(), actionIds.end());
    auto& ids = target->actionIds;
    const size_t sizeBefore = ids.size();
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](harkenstudio::ObjectId id) { return toRemove.count(id) != 0; }),
              ids.end());
    if (ids.size() < sizeBefore && ids.size() <= 1)
        deleteGroup(groups, groupName);
    return PackGroupEditResult::success();
}

PackGroupEditResult setPlatforms(std::vector<PackGroup>& groups,
                                 const QString& groupName,
                                 bool allPlatforms, const QStringList& platforms)
{
    PackGroup* target = findGroup(groups, groupName);
    if (!target)
        return PackGroupEditResult::failure(QStringLiteral("GROUP_NOT_FOUND"),
            QStringLiteral("Pack group '%1' does not exist").arg(groupName));

    target->allPlatforms = allPlatforms;
    target->platformIds = allPlatforms ? QStringList() : platforms;
    return PackGroupEditResult::success();
}

}
}
