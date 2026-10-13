
#pragma once

#include "ProjectTypes.h"

#include <QString>
#include <QStringList>
#include <cstdint>
#include <set>
#include <vector>

namespace VNX {

struct PackGroupEditResult
{
    bool    ok = false;
    QString errorCode;
    QString errorMessage;

    static PackGroupEditResult success() { return {true, {}, {}}; }
    static PackGroupEditResult failure(const QString& code, const QString& message) {
        return {false, code, message};
    }
};

// Pure edits over a PackGroup collection. Each mutates `groups` in place on
// success and leaves it untouched on failure. Group names are compared
// case-sensitively and must be unique across the collection. Action ids are
// runtime stable ids (uint64); a single id belongs to at most one group.
// A group that loses members through removeActions or an addActions move is
// dissolved once fewer than two members remain; its leftover action becomes
// unassigned. The addActions target group itself is never dissolved.
namespace PackGroupEditing {

PackGroupEditResult createGroup(std::vector<PackGroup>& groups, const QString& name);
PackGroupEditResult deleteGroup(std::vector<PackGroup>& groups, const QString& name);
PackGroupEditResult renameGroup(std::vector<PackGroup>& groups,
                                const QString& oldName, const QString& newName);
PackGroupEditResult addActions(std::vector<PackGroup>& groups,
                               const QString& groupName,
                               const std::vector<harkenstudio::ObjectId>& actionIds,
                               const std::set<harkenstudio::ObjectId>& validActionIds);
PackGroupEditResult removeActions(std::vector<PackGroup>& groups,
                                  const QString& groupName,
                                  const std::vector<harkenstudio::ObjectId>& actionIds);
PackGroupEditResult setPlatforms(std::vector<PackGroup>& groups,
                                 const QString& groupName,
                                 bool allPlatforms, const QStringList& platforms);

}

}
