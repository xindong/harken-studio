#pragma once

#include <QString>
#include <vector>

namespace VNH {

struct LocalBuildRecord
{
    QString executablePath;
    QString reportedBuildId;
    QString semanticVersion;
    QString channel;
    QString buildType;
    QString lastProbeError;
    QString executableFingerprint;
    qint64 executableSize = -1;
    qint64 executableModifiedMs = 0;
    int capabilitiesVersion = 0;
    qint64 releaseSequence = 0;
};

class LocalBuildRegistry
{
public:
    static QString filePath();
    static QString stableIdForPath(const QString& executablePath);
    static QString fileFingerprint(const QString& executablePath);
    static bool metadataMatchesFile(const LocalBuildRecord& record);
    static bool contains(const QString& executablePath,
                         QString* error = nullptr);
    static std::vector<LocalBuildRecord> load(QString* error = nullptr);
    static bool probe(const QString& executablePath, LocalBuildRecord* record,
                      QString* error = nullptr);
    static bool upsert(const LocalBuildRecord& record, QString* error = nullptr);
    static bool updateExisting(const LocalBuildRecord& record,
                               QString* error = nullptr);
    static bool requestRecheck(const QString& executablePath,
                               const QString& expectedFingerprint,
                               QString* error = nullptr);
    static bool remove(const QString& executablePath, QString* error = nullptr);
};

}
