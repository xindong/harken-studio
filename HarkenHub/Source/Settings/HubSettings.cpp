#include "Settings/HubSettings.h"

#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace VNH {

namespace {

QJsonObject loadRoot()
{
    QFile file(HubSettings::filePath());
    if (!file.open(QIODevice::ReadOnly))
        return QJsonObject();
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

bool saveRoot(const QJsonObject& root)
{
    const QString path = HubSettings::filePath();
    if (!QDir().mkpath(QFileInfo(path).dir().path()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return file.write(json) == json.size() && file.commit();
}

}

QString HubSettings::filePath()
{
    return userDataDir() + QStringLiteral("/settings.json");
}

bool HubSettings::autoSilentlyUpdateHub()
{
    return loadRoot()
        .value(QStringLiteral("autoSilentlyUpdateHub"))
        .toBool(true);
}

bool HubSettings::setAutoSilentlyUpdateHub(bool enabled)
{
    QJsonObject root = loadRoot();
    root[QStringLiteral("autoSilentlyUpdateHub")] = enabled;
    return saveRoot(root);
}

bool HubSettings::autoDownloadStudio()
{
    return loadRoot()
        .value(QStringLiteral("autoDownloadStudio"))
        .toBool(true);
}

bool HubSettings::setAutoDownloadStudio(bool enabled)
{
    QJsonObject root = loadRoot();
    root[QStringLiteral("autoDownloadStudio")] = enabled;
    return saveRoot(root);
}

QString HubSettings::projectLocation()
{
    return loadRoot()
        .value(QStringLiteral("projectLocation"))
        .toString();
}

bool HubSettings::setProjectLocation(const QString& path)
{
    const QString value = path.trimmed();
    if (value.isEmpty())
        return false;
    QJsonObject root = loadRoot();
    root[QStringLiteral("projectLocation")] = value;
    return saveRoot(root);
}

QString HubSettings::languagePreference()
{
    return loadRoot()
        .value(QStringLiteral("language"))
        .toString(QStringLiteral("system"));
}

bool HubSettings::setLanguagePreference(const QString& preference)
{
    if (preference != QStringLiteral("system") &&
        preference != QStringLiteral("en") &&
        preference != QStringLiteral("zh_CN"))
        return false;
    QJsonObject root = loadRoot();
    root[QStringLiteral("language")] = preference;
    return saveRoot(root);
}

QSize HubSettings::windowSize()
{
    const QJsonObject object =
        loadRoot().value(QStringLiteral("windowSize")).toObject();
    const QSize size(object.value(QStringLiteral("width")).toInt(),
                     object.value(QStringLiteral("height")).toInt());
    return size.width() > 0 && size.height() > 0 ? size : QSize(-1, -1);
}

bool HubSettings::setWindowSize(const QSize& size)
{
    if (size.width() <= 0 || size.height() <= 0)
        return false;
    QJsonObject object;
    object[QStringLiteral("width")] = size.width();
    object[QStringLiteral("height")] = size.height();
    QJsonObject root = loadRoot();
    root[QStringLiteral("windowSize")] = object;
    return saveRoot(root);
}

std::optional<QPoint> HubSettings::windowPosition()
{
    const QJsonValue value = loadRoot().value(QStringLiteral("windowPosition"));
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject object = value.toObject();
    const QJsonValue x = object.value(QStringLiteral("x"));
    const QJsonValue y = object.value(QStringLiteral("y"));
    if (!x.isDouble() || !y.isDouble())
        return std::nullopt;
    return QPoint(x.toInt(), y.toInt());
}

bool HubSettings::setWindowPosition(const QPoint& position)
{
    QJsonObject object;
    object[QStringLiteral("x")] = position.x();
    object[QStringLiteral("y")] = position.y();
    QJsonObject root = loadRoot();
    root[QStringLiteral("windowPosition")] = object;
    return saveRoot(root);
}

TableSortPreference HubSettings::tableSort(const QString& table)
{
    const QJsonObject sort = loadRoot().value(QStringLiteral("tableSort"))
        .toObject().value(table).toObject();
    return {sort.value(QStringLiteral("column")).toInt(),
            sort.value(QStringLiteral("descending")).toBool()
                ? Qt::DescendingOrder : Qt::AscendingOrder};
}

bool HubSettings::setTableSort(const QString& table, const TableSortPreference& sort)
{
    QJsonObject root = loadRoot();
    QJsonObject tables = root.value(QStringLiteral("tableSort")).toObject();
    tables[table] = QJsonObject{{QStringLiteral("column"), sort.column},
                              {QStringLiteral("descending"), sort.order == Qt::DescendingOrder}};
    root[QStringLiteral("tableSort")] = tables;
    return saveRoot(root);
}

QString HubSettings::pendingUpdateNotesVersion()
{
    return loadRoot()
        .value(QStringLiteral("pendingUpdateNotesVersion"))
        .toString();
}

bool HubSettings::setPendingUpdateNotesVersion(const QString& version)
{
    QJsonObject root = loadRoot();
    const QString value = version.trimmed();
    if (value.isEmpty())
        root.remove(QStringLiteral("pendingUpdateNotesVersion"));
    else
        root[QStringLiteral("pendingUpdateNotesVersion")] = value;
    return saveRoot(root);
}

bool HubSettings::clearPendingUpdateNotesVersion(const QString& version)
{
    const QString value = version.trimmed();
    if (value.isEmpty())
        return false;
    QJsonObject root = loadRoot();
    if (root.value(QStringLiteral("pendingUpdateNotesVersion")).toString() !=
        value)
        return false;
    root.remove(QStringLiteral("pendingUpdateNotesVersion"));
    return saveRoot(root);
}

}
