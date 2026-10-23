#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace VNX {

struct ProjectData;

struct PackArtifactMetadata
{
    QString unitKey;
    QString platform;
    QString language;
    QString packName;
    QString outputPath;
    qint64 bytes = 0;
    QDateTime modified;
    quint32 version = 0;
};

class PackArtifactCache : public QObject
{
    Q_OBJECT

public:
    static PackArtifactCache& instance();

    void openProject(const ProjectData& project);
    void closeProject();
    void publish(const QString& platform, const QVector<PackArtifactMetadata>& artifacts,
                 bool updateSelectedOnly = false);
    void removeUnselectedPlatforms(const QString& outputRoot, const QStringList& platforms);
    const PackArtifactMetadata* find(const QString& platform, const QString& language,
                                    const QString& unitKey, const QString& outputPath) const;

signals:
    void changed();

private:
    void save() const;

    QString cachePath_;
    QVector<PackArtifactMetadata> artifacts_;
};

}
