#pragma once

#include <harken_studio/ObjectId.h>

#include <QDebug>
#include <QHashFunctions>
#include <QJsonValue>
#include <QUuid>

namespace VNX {

inline harkenstudio::ObjectId runtimeObjectId(const QUuid& uuid) noexcept
{
    harkenstudio::ObjectId id{};
    id.high = (uint64_t{uuid.data1} << 32) | (uint64_t{uuid.data2} << 16) | uuid.data3;
    for (const auto byte : uuid.data4) id.low = (id.low << 8) | byte;
    return id;
}

inline QUuid authoringUuid(harkenstudio::ObjectId id) noexcept
{
    return QUuid(static_cast<uint32_t>(id.high >> 32),
        static_cast<uint16_t>(id.high >> 16), static_cast<uint16_t>(id.high),
        static_cast<uint8_t>(id.low >> 56), static_cast<uint8_t>(id.low >> 48),
        static_cast<uint8_t>(id.low >> 40), static_cast<uint8_t>(id.low >> 32),
        static_cast<uint8_t>(id.low >> 24), static_cast<uint8_t>(id.low >> 16),
        static_cast<uint8_t>(id.low >> 8), static_cast<uint8_t>(id.low));
}

inline QString objectIdString(harkenstudio::ObjectId id)
{
    const auto text = harkenstudio::FormatObjectId(id);
    return QString::fromLatin1(text.data(), 36);
}

inline bool parseObjectIdJson(const QJsonValue& value, harkenstudio::ObjectId* id)
{
    if (!value.isString()) return false;
    return harkenstudio::ParseObjectId(value.toString().toStdString(), id);
}

inline harkenstudio::ObjectId createObjectId()
{
    return runtimeObjectId(QUuid::createUuid());
}

}

namespace harkenstudio {

inline size_t qHash(ObjectId id, size_t seed = 0) noexcept
{
    return qHashMulti(seed, id.high, id.low);
}

inline QDebug operator<<(QDebug debug, ObjectId id)
{
    return debug << VNX::objectIdString(id);
}

}

Q_DECLARE_METATYPE(harkenstudio::ObjectId)
