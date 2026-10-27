#pragma once

#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>

namespace VNH {

struct ReleaseNotesChange
{
    QString category;
    QString text;
};

struct ReleaseNotesEntry
{
    QString version;
    QDate date;
    QString introduction;
    QList<int> pullRequests;
    QStringList contributors;
    QList<ReleaseNotesChange> changes;
};

class ReleaseNotesCatalog
{
public:
    static QList<ReleaseNotesEntry> load(QString* error = nullptr);
    static QList<ReleaseNotesEntry> fromJson(const QByteArray& data,
                                             QString* error = nullptr);
    static QString localizedChange(const QString& sourceText);
};

}
