#pragma once

#include <QString>
#include <QUuid>

namespace VNX {

struct ProjectItem;
struct ProjectData;

enum class PlaybackReferenceError { MissingTarget, InvalidTargetType, InvalidOwnership, InvalidStructure };

const ProjectItem* findDirectPlaybackChild(const ProjectItem& owner, const QUuid& guid);
bool validateDirectPlaybackTarget(const ProjectItem& owner, const QUuid& guid,
                                 const ProjectItem* target, QString* error = nullptr,
                                 PlaybackReferenceError* code = nullptr);
bool validatePlaybackReferences(const ProjectData& project, QString* error = nullptr,
                                PlaybackReferenceError* code = nullptr);
bool validatePlaybackReferences(const ProjectItem& root, QString* error = nullptr);

}
