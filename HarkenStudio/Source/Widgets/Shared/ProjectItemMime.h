
#pragma once

#include <QList>
#include <cstdint>

class QMimeData;

namespace VNX {
namespace ProjectItemMime {

inline constexpr char kSingleFormat[] = "application/x-harken-project-item";
inline constexpr char kListFormat[]   = "application/x-harken-project-items";

void writeObjectIDs(QMimeData* mime, const QList<uint32_t>& objectIDs);
bool hasObjectIDs(const QMimeData* mime);
QList<uint32_t> readObjectIDs(const QMimeData* mime);
uint32_t readSingleObjectID(const QMimeData* mime);

}
}
