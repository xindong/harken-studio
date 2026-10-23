#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <cstdint>
#include <memory>
#include "ProjectPlatform.h"

namespace VNX {

enum class PackGenerationStage {
    Queued,
    Preparing,
    Compiling,
    Writing,
    Publishing,
    Complete,
    Failed,
    Canceled
};

struct PackGenerationProgress
{
    QString platform;
    QString language;
    QString unitKey;
    int percent = 0;
    PackGenerationStage stage = PackGenerationStage::Queued;
};

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
    quint64 beginProgress(const std::shared_ptr<std::atomic_bool>& cancel);
    void updateProgress(quint64 runId, const PackGenerationProgress& progress);
    void finishProgress(quint64 runId, bool canceled);
    const PackGenerationProgress* findProgress(const QString& platform, const QString& language,
                                               const QString& unitKey) const;
    bool isExporting() const;
    bool isCurrentRun(quint64 runId) const;
    void cancelExport();

signals:
    void changed();
    void progressChanged(const QString& platform, const QString& language, const QString& unitKey);
    void exportActivityChanged(bool exporting);

private:
    struct ProgressEntry {
        PackGenerationProgress progress;
        quint64 runId = 0;
    };

    PackGenerationRecordStore() = default;
    QVector<PackGenerationRecord> records_;
    QVector<ProgressEntry> progress_;
    quint64 nextRunId_ = 0;
    quint64 activeRunId_ = 0;
    std::shared_ptr<std::atomic_bool> cancel_;
};

}
