#pragma once

#include <QPoint>
#include <QSize>
#include <QString>

#include <optional>

namespace VNH {

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
    static QSize windowSize();
    static bool setWindowSize(const QSize& size);
    static std::optional<QPoint> windowPosition();
    static bool setWindowPosition(const QPoint& position);
    static TableSortPreference tableSort(const QString& table);
    static bool setTableSort(const QString& table, const TableSortPreference& sort);
    static QString pendingUpdateNotesVersion();
    static bool setPendingUpdateNotesVersion(const QString& version);
    static bool clearPendingUpdateNotesVersion(const QString& version);
};

}
