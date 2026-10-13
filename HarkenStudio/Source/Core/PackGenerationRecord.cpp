#include "PackGenerationRecord.h"

namespace VNX {

QJsonObject PackGenerationRecord::toJson() const
{
    return {{QStringLiteral("packName"), packName},
            {QStringLiteral("objectID"), static_cast<qint64>(objectID)},
            {QStringLiteral("source"), source},
            {QStringLiteral("platform_id"), platform},
            {QStringLiteral("platform_name"), platformName},
            {QStringLiteral("base_platform"), ProjectPlatformSemantics::basePlatformName(basePlatform)},
            {QStringLiteral("language"), language},
            {QStringLiteral("success"), success},
            {QStringLiteral("status"), status},
            {QStringLiteral("outputPath"), outputPath},
            {QStringLiteral("warnings"), warnings},
            {QStringLiteral("errors"), errors},
            {QStringLiteral("bytes"), bytes},
            {QStringLiteral("elapsedMs"), elapsedMs}};
}

PackGenerationRecordStore& PackGenerationRecordStore::instance()
{
    static PackGenerationRecordStore store;
    return store;
}

void PackGenerationRecordStore::add(const PackGenerationRecord& record)
{
    records_.push_back(record);
}

void PackGenerationRecordStore::replace(const QVector<PackGenerationRecord>& records)
{
    records_ = records;
}

void PackGenerationRecordStore::clear()
{
    records_.clear();
}

void PackGenerationRecordStore::invalidatePlatform(const QString& platformId)
{
    for (auto& record : records_) {
        if (record.platform == platformId) record.status = QStringLiteral("Needs rebuild");
    }
}

QVector<PackGenerationRecord> PackGenerationRecordStore::records() const
{
    return records_;
}

void PackGenerationRecordStore::reconcilePlatforms(const QVector<ProjectPlatform>& platforms)
{
    for (auto& record : records_) {
        const auto* definition = ProjectPlatformSemantics::find(platforms, QUuid::fromString(record.platform));
        if (!definition || (!record.platformDefinition.isEmpty()
            && definition->toJson() != record.platformDefinition))
            record.status = QStringLiteral("Needs rebuild");
    }
}

}
