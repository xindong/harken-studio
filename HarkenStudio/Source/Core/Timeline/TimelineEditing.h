#pragma once

#include "TimelineTypes.h"
#include "../ActionManager.h"

#include <memory>
#include <vector>

namespace harken::studio {

class ProjectManager;
class TransactionalUndoCommand;

namespace TimelineEditing {

TimelineValidation validateClipPlacement(ProjectManager* projectManager,
                                         ProjectItem* track,
                                         const ClipLedger& clips);

ActionResult replaceClips(ProjectManager* projectManager,
                          ProjectItem* track,
                          const ClipLedger& clips,
                          const QObject* originator = nullptr,
                          const QString& undoText = {});
ActionResult addClip(ProjectManager* projectManager,
                     ProjectItem* track,
                     Clip clip,
                     const QObject* originator = nullptr);
ActionResult removeClip(ProjectManager* projectManager,
                        ProjectItem* track,
                        const QUuid& clipGuid,
                        const QObject* originator = nullptr);
ActionResult moveClip(ProjectManager* projectManager,
                      ProjectItem* track,
                      const QUuid& clipGuid,
                      int64_t start,
                      const QObject* originator = nullptr);
ActionResult resizeClip(ProjectManager* projectManager,
                        ProjectItem* track,
                        const QUuid& clipGuid,
                        int64_t start,
                        int64_t duration,
                        const QObject* originator = nullptr);
struct ClipMove {
    QUuid sourceTrackGuid;
    QUuid destinationTrackGuid;
    QUuid clipGuid;
    int64_t start = 0;
};

std::unique_ptr<TransactionalUndoCommand> makeClipMoveCommand(
    ProjectManager* projectManager,
    const std::vector<ClipMove>& moves,
    QString* error = nullptr);

}
}
