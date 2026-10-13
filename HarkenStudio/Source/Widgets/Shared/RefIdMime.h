
#pragma once

#include "RefDomain.h"

#include <QList>
#include <cstdint>

class QMimeData;

namespace VNX {
namespace RefIdMime {

void write(QMimeData* mime, RefDomain domain, const QList<uint32_t>& ids);
bool has(const QMimeData* mime, RefDomain domain);
QList<uint32_t> read(const QMimeData* mime, RefDomain domain);
uint32_t readSingle(const QMimeData* mime, RefDomain domain);

}
}
