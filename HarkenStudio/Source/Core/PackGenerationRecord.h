#pragma once

#include <QJsonObject>
#include <QObject>
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
    QString unitKey;

    QJsonObject toJson() const;
    QString resolvedUnitKey() const;
};

class PackGenerationRecordStore : public QObject
{
    Q_OBJECT

public:
    static PackGenerationRecordStore& instance();

    void add(const PackGenerationRecord& record);
    void replace(const QVector<PackGenerationRecord>& records);
    void clear();
    QVector<PackGenerationRecord> records() const;
    const PackGenerationRecord* find(const QString& platform, const QString& language,
                                     const QString& unitKey) const;

signals:
    void changed();

private:
    PackGenerationRecordStore() = default;
    QVector<PackGenerationRecord> records_;
};

}
