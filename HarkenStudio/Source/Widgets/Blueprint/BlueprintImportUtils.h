
#pragma once

#include <QPointF>
#include <QString>
#include <QStringList>

namespace VNX {

struct ProjectItem;

namespace Blueprint {

enum class AudioDropKind { Rejected, Root, Parent, Sources, RootOutput };

struct AudioDropTarget
{
    AudioDropKind kind = AudioDropKind::Rejected;
    ProjectItem* item = nullptr;
    QString language;
    QString error;

    bool ok() const { return kind != AudioDropKind::Rejected; }
};

AudioDropTarget resolveAudioDropTarget(ProjectItem* target);

QString commonPrefix(const QStringList& filePaths);

QPointF snapToGrid(QPointF scenePoint, double cellPx = 20.0);

}
}
