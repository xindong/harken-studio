#include "ProjectUserData.h"

#include "AssetPipeline/PathUtils.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace harken::studio::ProjectUserData {
namespace {

constexpr int kVersion = 1;

QDir directory(const QString& projectRoot)
{
    return QDir(QDir(projectRoot).filePath(QLatin1String(kDirectory)));
}

}

QJsonObject read(const QString& projectRoot, const QString& fileName)
{
    QFile file(directory(projectRoot).filePath(fileName));
    if (!file.open(QIODevice::ReadOnly)) return {};
    auto data = QJsonDocument::fromJson(file.readAll()).object();
    return data.take(QStringLiteral("version")).toInt() == kVersion ? data : QJsonObject{};
}

bool write(const QString& projectRoot, const QString& fileName, QJsonObject data)
{
    const QDir target = directory(projectRoot);
    if (!target.mkpath(QStringLiteral("."))) return false;
    data.insert(QStringLiteral("version"), kVersion);
    const QByteArray bytes = QJsonDocument(data).toJson(QJsonDocument::Indented);
    QSaveFile file(target.filePath(fileName));
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

bool importFromCache(const QString& projectRoot)
{
    const QDir cache(PathUtils::getDotCacheDir(projectRoot));
    for (const auto* fileName : {kEditorLayout, kPackExportSelection}) {
        const QString name = QLatin1String(fileName);
        if (directory(projectRoot).exists(name) || !cache.exists(name)) continue;
        QFile file(cache.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) return false;
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject() && !write(projectRoot, name, document.object())) return false;
    }
    return true;
}

}
