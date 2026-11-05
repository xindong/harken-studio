#pragma once

#include <QString>

namespace VNX {

struct ProjectItem;

enum class AudioImportKind { Rejected, Root, Parent, Sources, RootOutput };

struct AudioImportTarget
{
    AudioImportKind kind = AudioImportKind::Rejected;
    ProjectItem* item = nullptr;
    QString language;
    QString error;

    bool ok() const { return kind != AudioImportKind::Rejected; }
};

AudioImportTarget resolveAudioImportTarget(ProjectItem* target);

}
