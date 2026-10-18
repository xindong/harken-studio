#include "PackGenerationRecord.h"
#include <algorithm>

namespace VNX {

QJsonObject PackGenerationRecord::toJson() const
{
    return {{QStringLiteral("packName"), packName},
            {QStringLiteral("objectID"), static_cast<qint64>(objectID)},
            {QStringLiteral("source"), source},
            {QStringLiteral("unitKey"), resolvedUnitKey()},
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

QString PackGenerationRecord::resolvedUnitKey() const
{
    if (!unitKey.isEmpty()) return unitKey;
    if (source == QLatin1String("Action") && objectID != 0)
        return QStringLiteral("a:") + QString::number(objectID);
    return source + QLatin1Char(':') + packName;
}

PackGenerationRecordStore& PackGenerationRecordStore::instance()
{
    static PackGenerationRecordStore store;
    return store;
}

void PackGenerationRecordStore::add(const PackGenerationRecord& record)
{
    for (auto& current : records_) {
        if (current.platform == record.platform && current.language == record.language
            && current.resolvedUnitKey() == record.resolvedUnitKey()) {
            current = record;
            emit changed();
            return;
        }
    }
    records_.push_back(record);
    emit changed();
}

void PackGenerationRecordStore::replace(const QVector<PackGenerationRecord>& records)
{
    records_.clear();
    for (const auto& record : records) {
        auto existing = std::find_if(records_.begin(), records_.end(), [&](const auto& current) {
            return current.platform == record.platform && current.language == record.language
                && current.resolvedUnitKey() == record.resolvedUnitKey();
        });
        if (existing == records_.end()) records_.push_back(record);
        else *existing = record;
    }
    emit changed();
}

void PackGenerationRecordStore::clear()
{
    records_.clear();
    emit changed();
}

const PackGenerationRecord* PackGenerationRecordStore::find(
    const QString& platform, const QString& language, const QString& unitKey) const
{
    const auto found = std::find_if(records_.cbegin(), records_.cend(), [&](const auto& record) {
        return record.platform == platform && record.language == language
            && record.resolvedUnitKey() == unitKey;
    });
    return found == records_.cend() ? nullptr : &*found;
}

QVector<PackGenerationRecord> PackGenerationRecordStore::records() const
{
    return records_;
}

}
