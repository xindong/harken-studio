#pragma once

#include <DockManager.h>
#include <DockWidget.h>

#include <QString>

class QWidget;

namespace VNX {

struct WorkspaceDockCreateSpec
{
    QString title;
    QString objectName;
    QWidget* content = nullptr;
    bool deleteOnClose = true;
    bool initiallyVisible = true;
    bool minimumSizeFromContent = false;
    ads::DockWidgetArea area = ads::NoDockWidgetArea;
    QWidget* anchor = nullptr;
    double initialShare = 0.0;
    QWidget* lastAnchor = nullptr;
};

class WorkspaceDockFactory
{
public:
    static void setContent(ads::CDockWidget* dock, QWidget* content);
    static ads::CDockWidget* createDetachedDock(const WorkspaceDockCreateSpec& spec);
    static ads::CDockWidget* createDock(ads::CDockManager* dockManager,
                                        const WorkspaceDockCreateSpec& spec);
};

}
