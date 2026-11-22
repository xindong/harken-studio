#include "Settings/HubSettings.h"

#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QLockFile>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QThread>

namespace harken::hub {

namespace {

bool lockSettings(QLockFile& lock, bool reading = false, int timeoutMs = -1)
{
    const auto* app = QCoreApplication::instance();
    const bool guiThread = app && app->inherits("QApplication") && QThread::currentThread() == app->thread();
    QDeadlineTimer deadline(timeoutMs >= 0 ? timeoutMs : guiThread ? (reading ? 50 : 0) : 5000);
    do {
        if (lock.tryLock(0)) return true;
        if (deadline.hasExpired()) return false;
        QThread::msleep(5);
    } while (true);
}

QJsonObject readRoot()
{
    QFile file(HubSettings::filePath());
    if (!file.open(QIODevice::ReadOnly))
        return QJsonObject();
    const auto bytes = file.readAll();
    file.close();
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    return document.isObject() ? document.object() : QJsonObject();
}

QJsonObject loadRoot()
{
    const auto path = HubSettings::filePath();
    if (!QFileInfo::exists(path)) return {};
    QLockFile lock(path + QStringLiteral(".lock"));
    lockSettings(lock, true);
    return readRoot();
}

template<class Mutation> bool updateRoot(Mutation mutate, int timeoutMs = -1)
{
    const QString path = HubSettings::filePath();
    if (!QDir().mkpath(QFileInfo(path).dir().path()))
        return false;
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lockSettings(lock, false, timeoutMs))
        return false;
    QJsonObject root = readRoot();
    if (!mutate(root))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return file.write(json) == json.size() && file.commit();
}

bool updateValue(const QString& key, const QJsonValue& value)
{
    return updateRoot([&](QJsonObject& root) { root[key] = value; return true; });
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
    return updateValue(QStringLiteral("autoSilentlyUpdateHub"), enabled);
}

bool HubSettings::autoDownloadStudio()
{
    return loadRoot()
        .value(QStringLiteral("autoDownloadStudio"))
        .toBool(true);
}

bool HubSettings::setAutoDownloadStudio(bool enabled)
{
    return updateValue(QStringLiteral("autoDownloadStudio"), enabled);
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
    return updateValue(QStringLiteral("projectLocation"), value);
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
    return updateValue(QStringLiteral("language"), preference);
}

QString HubSettings::themePreference()
{
    const QString value = loadRoot().value(QStringLiteral("theme")).toString();
    return value == QStringLiteral("light") || value == QStringLiteral("dark")
        ? value : QStringLiteral("system");
}

bool HubSettings::setThemePreference(const QString& preference)
{
    if (preference != QStringLiteral("system") && preference != QStringLiteral("light")
        && preference != QStringLiteral("dark")) return false;
    return updateValue(QStringLiteral("theme"), preference);
}

bool HubSettings::setWindowGeometry(const QRect& geometry, int timeoutMs)
{
    if (!geometry.isValid()) return false;
    return updateRoot([&](QJsonObject& root) {
        root[QStringLiteral("windowSize")] = QJsonObject{{"width", geometry.width()}, {"height", geometry.height()}};
        root[QStringLiteral("windowPosition")] = QJsonObject{{"x", geometry.x()}, {"y", geometry.y()}};
        return true;
    }, timeoutMs);
}

QSize HubSettings::windowSize()
{
    const QJsonObject object =
        loadRoot().value(QStringLiteral("windowSize")).toObject();
    const QSize size(object.value(QStringLiteral("width")).toInt(),
                     object.value(QStringLiteral("height")).toInt());
    return size.width() > 0 && size.height() > 0 ? size : QSize(-1, -1);
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

TableSortPreference HubSettings::tableSort(const QString& table)
{
    const QJsonObject sort = loadRoot().value(QStringLiteral("tableSort"))
        .toObject().value(table).toObject();
    return {sort.value(QStringLiteral("column")).toInt(),
            sort.value(QStringLiteral("descending")).toBool()
                ? Qt::DescendingOrder : Qt::AscendingOrder};
}

bool HubSettings::setTableSort(const QString& table, const TableSortPreference& sort, int timeoutMs)
{
    return updateRoot([&](QJsonObject& root) {
        QJsonObject tables = root.value(QStringLiteral("tableSort")).toObject();
        tables[table] = QJsonObject{{QStringLiteral("column"), sort.column},
                                  {QStringLiteral("descending"), sort.order == Qt::DescendingOrder}};
        root[QStringLiteral("tableSort")] = tables;
        return true;
    }, timeoutMs);
}

QString HubSettings::pendingUpdateNotesVersion()
{
    return loadRoot()
        .value(QStringLiteral("pendingUpdateNotesVersion"))
        .toString();
}

bool HubSettings::setPendingUpdateNotesVersion(const QString& version)
{
    return updateRoot([&](QJsonObject& root) {
        const QString value = version.trimmed();
        if (value.isEmpty())
            root.remove(QStringLiteral("pendingUpdateNotesVersion"));
        else
            root[QStringLiteral("pendingUpdateNotesVersion")] = value;
        return true;
    }, 5000);
}

bool HubSettings::clearPendingUpdateNotesVersion(const QString& version)
{
    const QString value = version.trimmed();
    if (value.isEmpty())
        return false;
    return updateRoot([&](QJsonObject& root) {
        if (root.value(QStringLiteral("pendingUpdateNotesVersion")).toString() != value)
            return false;
        root.remove(QStringLiteral("pendingUpdateNotesVersion"));
        return true;
    });
}

}
