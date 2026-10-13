#pragma once

#include <QJsonValue>

#include <cstdint>

namespace VNX {

bool parseRuntimeU64DecimalStringV1(const QJsonValue& value,
                                    uint64_t* result) noexcept;
QJsonValue formatRuntimeU64DecimalStringV1(uint64_t value);

}
