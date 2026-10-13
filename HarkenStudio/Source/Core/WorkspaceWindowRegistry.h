#pragma once

#include "WorkspaceWindowTypes.h"

#include <DockWidget.h>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

namespace VNX {

struct WorkspaceWindowInstanceEntry
{
    QString typeId;
    int instanceId = 0;
    QPointer<ads::CDockWidget> dock;
    QPointer<QWidget> content;
    WorkspaceWindowInstanceSpec restoreSpec;
};

class WorkspaceWindowRegistry
{
public:
    bool registerSingleton(const QString& typeId, ads::CDockWidget* dock);
    void unregisterSingleton(const QString& typeId);

    bool containsSingleton(const QString& typeId) const;
    ads::CDockWidget* singletonDock(const QString& typeId) const;
    QString singletonTypeForDockObjectName(const QString& dockObjectName) const;
    QList<QString> singletonTypeIds() const;
    QList<ads::CDockWidget*> singletonDocks() const;

    bool registerInstance(const QString& typeId, int instanceId,
                          ads::CDockWidget* dock, QWidget* content);
    void unregisterInstance(const QString& typeId, int instanceId);

    bool containsInstance(const QString& typeId, int instanceId) const;
    ads::CDockWidget* instanceDock(const QString& typeId, int instanceId) const;
    QWidget* instanceContent(const QString& typeId, int instanceId) const;
    QString instanceTypeForDockObjectName(const QString& dockObjectName) const;
    QList<WorkspaceWindowInstanceEntry> instanceEntries(const QString& typeId = QString()) const;

    void setInstanceRestoreSpec(const WorkspaceWindowInstanceSpec& spec);
    WorkspaceWindowInstanceSpec instanceRestoreSpec(const QString& typeId, int instanceId) const;

private:
    static QString instanceKey(const QString& typeId, int instanceId);

    QHash<QString, QPointer<ads::CDockWidget>> singletonDocks_;
    QHash<QString, QString> dockObjectNameToTypeId_;
    QHash<QString, WorkspaceWindowInstanceEntry> instanceEntries_;
    QHash<QString, QString> instanceDockObjectNameToKey_;
};

}
