#pragma once

#include "TimelineTypes.h"
#include "../ActionManager.h"

#include <optional>

namespace VNX {

class ProjectManager;

namespace TimelineEditing {

SegmentTimeSpec timeSpecForTrackParent(const ProjectItem* parent);
std::optional<ClipLedger> convertClipLedger(const ClipLedger& source,
                                            const SegmentTimeSpec& sourceTime,
                                            const SegmentTimeSpec& destinationTime,
                                            QString* error = nullptr);
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
ActionResult moveClipAcrossTracks(ProjectManager* projectManager,
                                  ProjectItem* sourceTrack,
                                  ProjectItem* destinationTrack,
                                  const QUuid& clipGuid,
                                  int64_t start,
                                  const QObject* originator = nullptr);

ActionResult replaceMarkers(ProjectManager* projectManager,
                         ProjectItem* segment,
                         const MarkerLedger& markers,
                         const QObject* originator = nullptr,
                         const QString& undoText = {});
ActionResult addMarker(ProjectManager* projectManager,
                    ProjectItem* segment,
                    Marker marker,
                    const QObject* originator = nullptr);
ActionResult updateMarker(ProjectManager* projectManager,
                       ProjectItem* segment,
                       const Marker& marker,
                       const QObject* originator = nullptr);
ActionResult removeMarker(ProjectManager* projectManager,
                       ProjectItem* segment,
                       const QUuid& markerGuid,
                       const QObject* originator = nullptr);
ActionResult replaceSegmentTime(ProjectManager* projectManager,
                              ProjectItem* segment,
                              const SegmentTimeSpec& segmentTime,
                              const QObject* originator = nullptr,
                              const QString& undoText = {});
ActionResult setSpliceByMarker(ProjectManager* projectManager,
                            ProjectItem* container,
                            bool spliceByMarker,
                            const QObject* originator = nullptr,
                            const QString& undoText = {});

}
}
