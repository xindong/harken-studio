#pragma once

#include <harken_studio/ObjectId.h>

#include <QDebug>
#include <QHashFunctions>
#include <QJsonValue>
#include <QUuid>

namespace harken::studio {

inline harken::studio::types::ObjectId runtimeObjectId(const QUuid& uuid) noexcept
{
    harken::studio::types::ObjectId id{};
    id.high = (uint64_t{uuid.data1} << 32) | (uint64_t{uuid.data2} << 16) | uuid.data3;
    for (const auto byte : uuid.data4) id.low = (id.low << 8) | byte;
    return id;
}

inline QUuid authoringUuid(harken::studio::types::ObjectId id) noexcept
{
    return QUuid(static_cast<uint32_t>(id.high >> 32),
        static_cast<uint16_t>(id.high >> 16), static_cast<uint16_t>(id.high),
        static_cast<uint8_t>(id.low >> 56), static_cast<uint8_t>(id.low >> 48),
        static_cast<uint8_t>(id.low >> 40), static_cast<uint8_t>(id.low >> 32),
        static_cast<uint8_t>(id.low >> 24), static_cast<uint8_t>(id.low >> 16),
        static_cast<uint8_t>(id.low >> 8), static_cast<uint8_t>(id.low));
}

inline QString objectIdString(harken::studio::types::ObjectId id)
{
    const auto text = harken::studio::types::FormatObjectId(id);
    return QString::fromLatin1(text.data(), 36);
}

inline bool parseObjectIdJson(const QJsonValue& value, harken::studio::types::ObjectId* id)
{
    if (!value.isString()) return false;
    return harken::studio::types::ParseObjectId(value.toString().toStdString(), id);
}

inline harken::studio::types::ObjectId createObjectId()
{
    return runtimeObjectId(QUuid::createUuid());
}

}

namespace harken::studio::types {

inline size_t qHash(ObjectId id, size_t seed = 0) noexcept
{
    return qHashMulti(seed, id.high, id.low);
}

inline QDebug operator<<(QDebug debug, ObjectId id)
{
    return debug << harken::studio::objectIdString(id);
}

}

Q_DECLARE_METATYPE(harken::studio::types::ObjectId)
