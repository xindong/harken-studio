#pragma once

#include <QString>
#include <cstdint>

namespace harken::studio::bridge {

QString mediaRegistryFindPath(uint32_t sourceId);
void mediaRegistryRegister(uint32_t sourceId, const QString& absolutePath,
                           uint64_t fileSize = 0);
void mediaRegistryUnregister(uint32_t sourceId);
uint32_t mediaRegistryCount();
void mediaRegistryClear();
uint32_t mediaRegistryFindSourceIdByPath(const QString& absolutePath);

}
