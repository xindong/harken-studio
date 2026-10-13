#pragma once

#include <QString>
#include <QStringList>

namespace VNX {

struct ProjectData;

namespace ProjectLanguageSemantics {

struct Normalization
{
    QStringList value;
    QString     error;

    bool ok() const { return error.isEmpty(); }
};

QStringList   buildLanguages(const ProjectData& project);
Normalization normalize(const QStringList& requested);
bool          isSfxName(const QString& name);
QStringList   dropSfxNames(const QStringList& names);

}
}
