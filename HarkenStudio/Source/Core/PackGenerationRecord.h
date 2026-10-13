#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <cstdint>
#include "ProjectPlatform.h"

namespace VNX {

struct PackGenerationRecord
{
    QString packName;
    uint32_t objectID = 0;
    QString source;
    QString platform;
    QString language;
    bool success = false;
    QString status;
    QString outputPath;
    QString warnings;
    QString errors;
    qint64 bytes = 0;
    qint64 elapsedMs = -1;
    QString platformName;
    BasePlatform basePlatform = BasePlatform::Windows;
    QJsonObject platformDefinition;

    QJsonObject toJson() const;
};

class PackGenerationRecordStore
{
public:
    static PackGenerationRecordStore& instance();

    void add(const PackGenerationRecord& record);
    void replace(const QVector<PackGenerationRecord>& records);
    void clear();
    void invalidatePlatform(const QString& platformId);
    void reconcilePlatforms(const QVector<ProjectPlatform>& platforms);
    QVector<PackGenerationRecord> records() const;

private:
    QVector<PackGenerationRecord> records_;
};

}
