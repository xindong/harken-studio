
#include "RefIdMime.h"

#include "ProjectItemMime.h"

#include <QMimeData>
#include <QtGlobal>

#include <cstring>

namespace VNX {
namespace RefIdMime {
namespace {

struct Formats {
    const char* single;
    const char* list;
};

Formats formats(RefDomain domain)
{
    switch (domain) {
    case RefDomain::ProjectObject:
        return {ProjectItemMime::kSingleFormat, ProjectItemMime::kListFormat};
    case RefDomain::AudioAsset:
        return {"application/x-harken-audio-asset", "application/x-harken-audio-assets"};
    }
    Q_UNREACHABLE_RETURN((Formats{}));
}

}

void write(QMimeData* mime, RefDomain domain, const QList<uint32_t>& ids)
{
    if (!mime)
        return;
    QByteArray payload;
    for (uint32_t id : ids) {
        if (id == 0)
            continue;
        payload.append(reinterpret_cast<const char*>(&id), sizeof(uint32_t));
    }
    if (payload.isEmpty())
        return;
    const Formats fmt = formats(domain);
    mime->setData(QLatin1String(fmt.single),
                  payload.left(static_cast<qsizetype>(sizeof(uint32_t))));
    mime->setData(QLatin1String(fmt.list), payload);
}

bool has(const QMimeData* mime, RefDomain domain)
{
    if (!mime)
        return false;
    const Formats fmt = formats(domain);
    return mime->hasFormat(QLatin1String(fmt.list))
        || mime->hasFormat(QLatin1String(fmt.single));
}

QList<uint32_t> read(const QMimeData* mime, RefDomain domain)
{
    QList<uint32_t> ids;
    if (!mime)
        return ids;
    const Formats fmt = formats(domain);
    QByteArray payload = mime->data(QLatin1String(fmt.list));
    if (payload.isEmpty())
        payload = mime->data(QLatin1String(fmt.single));
    const qsizetype count = payload.size() / static_cast<qsizetype>(sizeof(uint32_t));
    ids.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        uint32_t id = 0;
        std::memcpy(&id, payload.constData() + i * sizeof(uint32_t), sizeof(uint32_t));
        if (id != 0)
            ids.append(id);
    }
    return ids;
}

uint32_t readSingle(const QMimeData* mime, RefDomain domain)
{
    const QList<uint32_t> ids = read(mime, domain);
    return ids.size() == 1 ? ids.first() : 0;
}

}
}
