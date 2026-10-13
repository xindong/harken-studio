
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUuid>
#include <cstdint>
#include <vector>

namespace VNX {

struct ProjectData;
struct AssetInfo;

class IntegrityScanner
{
public:
    static constexpr int kIntegrityFormatVersion = 3;

    enum class RefKind : uint8_t
    {
        OutputBus = 0,
        ActionStep,
        SwitchAssignment,
        SwitchDefaultChild,
        RandomChildWeight,
        ItemSourceID,
        ItemLanguageSourceID,
        BusSend,
        BusParent,
        BusRouteCycle = 10
    };

    struct Issue
    {
        RefKind  kind = RefKind::OutputBus;
        QUuid    sourceGuid;
        uint32_t sourceBusId = 0;
        QString  sourceName;
        QString  detail;
        QString  missingTarget;
    };

    struct Report
    {
        int                version = kIntegrityFormatVersion;
        std::vector<Issue> issues;
        qint64             scannedAtMs = 0;
        QByteArray         indexHash;
    };

    IntegrityScanner() = default;
    ~IntegrityScanner() = default;

    IntegrityScanner(const IntegrityScanner&)            = delete;
    IntegrityScanner& operator=(const IntegrityScanner&) = delete;

    Report scan(const ProjectData& project, const QList<AssetInfo>& assets) const;

    bool loadFromCache(const QString& projectRoot, Report& outReport) const;

    bool saveToCache(const QString& projectRoot, const Report& report) const;

    static QString summarize(const Report& report);
};

}
