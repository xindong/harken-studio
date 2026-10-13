#pragma once

#include "ThemeSnapshot.h"

#include <QString>

class QWidget;

namespace ads {
class CDockManager;
}

namespace VNX {

class ThemeAssetResolver;

class QtAdsThemeAdapter
{
public:
    explicit QtAdsThemeAdapter(ThemeSnapshotPtr snapshot);

    static QString styleSheetMarker();
    QString styleSheet() const;
    QString composeApplicationStyleSheet(QString applicationStyleSheet) const;
    static void applyIcons(ThemeAssetResolver& resolver,
                           const ThemeSnapshotPtr& snapshot);
    ads::CDockManager* createDockManager(QWidget* parent) const;

private:
    ThemeSnapshotPtr snapshot_;
};

}
