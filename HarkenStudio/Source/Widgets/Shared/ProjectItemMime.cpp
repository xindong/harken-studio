
#include "ProjectItemMime.h"

#include "RefIdMime.h"

namespace VNX {
namespace ProjectItemMime {

void writeObjectIDs(QMimeData* mime, const QList<uint32_t>& objectIDs)
{
    RefIdMime::write(mime, RefDomain::ProjectObject, objectIDs);
}

bool hasObjectIDs(const QMimeData* mime)
{
    return RefIdMime::has(mime, RefDomain::ProjectObject);
}

QList<uint32_t> readObjectIDs(const QMimeData* mime)
{
    return RefIdMime::read(mime, RefDomain::ProjectObject);
}

uint32_t readSingleObjectID(const QMimeData* mime)
{
    return RefIdMime::readSingle(mime, RefDomain::ProjectObject);
}

}
}
