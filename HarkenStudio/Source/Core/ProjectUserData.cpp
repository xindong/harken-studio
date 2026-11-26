#include "ProjectUserData.h"

#include "AssetPipeline/PathUtils.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>

namespace harken::studio::ProjectUserData {
namespace {

constexpr int kVersion = 1;

QDir directory(const QString& projectRoot)
{
    return QDir(QDir(projectRoot).filePath(QLatin1String(kDirectory)));
}

QJsonObject versionedData(const QByteArray& bytes)
{
    auto data = QJsonDocument::fromJson(bytes).object();
    return data.take(QStringLiteral("version")).toInt() == kVersion ? data : QJsonObject{};
}

}

void load(const QString& projectRoot, const QString& fileName,
          const std::function<bool(const QJsonObject&)>& restore, const QString& legacySettingsKey)
{
    QFile file(directory(projectRoot).filePath(fileName));
    if (file.exists()) {
        if (file.open(QIODevice::ReadOnly)) restore(versionedData(file.readAll()));
        return;
    }

    QJsonObject data;
    if (!legacySettingsKey.isEmpty()) {
        const QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                                 QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio"));
        data = versionedData(settings.value(legacySettingsKey).toByteArray());
        if (!data.isEmpty() && !restore(data)) data = {};
    }
    QFile cache(QDir(PathUtils::getDotCacheDir(projectRoot)).filePath(fileName));
    if (data.isEmpty()) {
        if (!cache.open(QIODevice::ReadOnly)) return;
        const auto bytes = cache.readAll();
        cache.close();
        data = fileName == QLatin1String(kEditorLayout)
            ? QJsonDocument::fromJson(bytes).object() : versionedData(bytes);
        if (data.isEmpty() || !restore(data)) return;
    }
    if (write(projectRoot, fileName, data)) {
        if (cache.exists() && !cache.remove())
            qWarning() << "Could not remove migrated project user data:" << cache.fileName();
    } else {
        qWarning() << "Could not migrate project user data:" << file.fileName();
    }
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

bool prepareCacheReset(const QString& projectRoot)
{
    const QDir cache(PathUtils::getDotCacheDir(projectRoot));
    const QDir target = directory(projectRoot);
    const QFileInfo targetInfo(target.absolutePath());
    if (PathUtils::assetRegistryFilePath(projectRoot).isEmpty()
        || targetInfo.isSymLink() || targetInfo.isJunction()) return false;
    load(projectRoot, QLatin1String(kEditorLayout), [](const QJsonObject& data) {
        return !data.isEmpty();
    });
    for (const auto* name : {kEditorLayout, kPackExportSelection})
        if (cache.exists(QLatin1String(name)) && !target.exists(QLatin1String(name))) return false;
    return true;
}

}
