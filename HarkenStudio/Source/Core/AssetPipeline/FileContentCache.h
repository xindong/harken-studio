#pragma once

#include <QString>
#include <functional>
#include <memory>

namespace VNX {

class FileContentCache
{
public:
    explicit FileContentCache(QString cachePath = {});
    ~FileContentCache();

    QString digest(const QString& path, const std::function<bool()>& canceled = {});
    bool save();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
