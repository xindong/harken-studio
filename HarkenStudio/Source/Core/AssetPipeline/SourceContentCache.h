#pragma once

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <functional>
#include <memory>

namespace VNX {

QByteArray packFileState(const QString& path);

class SourceContentCache
{
public:
    explicit SourceContentCache(QString cachePath = {});
    ~SourceContentCache();

    QString digest(const QString& path, const std::function<bool()>& canceled = {},
                   QByteArray* state = nullptr);
    void retainSources(const QStringList& paths);
    bool save();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
