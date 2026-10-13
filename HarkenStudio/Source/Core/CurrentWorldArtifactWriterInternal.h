#pragma once

#include "CurrentWorldArtifactWriter.h"

#include <QtGlobal>

#include <memory>

namespace VNX {

class CurrentWorldArtifactFile
{
public:
    virtual ~CurrentWorldArtifactFile() = default;
    virtual void setDirectWriteFallback(bool enabled) = 0;
    virtual bool open() = 0;
    virtual qint64 write(const char* data, qint64 byteCount) = 0;
    virtual bool commit() = 0;
};

using CurrentWorldArtifactFileFactory =
    std::unique_ptr<CurrentWorldArtifactFile> (*)(const QString& targetPath);

void setCurrentWorldArtifactFileFactoryForTesting(
    CurrentWorldArtifactFileFactory factory) noexcept;

} // namespace VNX
