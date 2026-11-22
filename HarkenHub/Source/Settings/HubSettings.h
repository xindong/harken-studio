#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <optional>

namespace harken::hub {

struct TableSortPreference
{
    int column = 0;
    Qt::SortOrder order = Qt::AscendingOrder;
};

class HubSettings
{
public:
    static QString filePath();
    static bool autoSilentlyUpdateHub();
    static bool setAutoSilentlyUpdateHub(bool enabled);
    static bool autoDownloadStudio();
    static bool setAutoDownloadStudio(bool enabled);
    static QString projectLocation();
    static bool setProjectLocation(const QString& path);
    static QString languagePreference();
    static bool setLanguagePreference(const QString& preference);
    static QString themePreference();
    static bool setThemePreference(const QString& preference);
    static bool setWindowGeometry(const QRect& geometry, int timeoutMs = -1);
    static QSize windowSize();
    static std::optional<QPoint> windowPosition();
    static TableSortPreference tableSort(const QString& table);
    static bool setTableSort(const QString& table, const TableSortPreference& sort, int timeoutMs = -1);
    static QString pendingUpdateNotesVersion();
    static bool setPendingUpdateNotesVersion(const QString& version);
    static bool clearPendingUpdateNotesVersion(const QString& version);
};

}
