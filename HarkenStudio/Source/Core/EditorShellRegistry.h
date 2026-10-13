
#pragma once

#include <QList>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

namespace VNX {

struct EditorShellSpec
{
    QString editorType;
    int instanceId = 0;
    QString objectKey;
    bool pinned = false;
    bool visible = false;
};

struct EditorShellRouteTarget
{
    int instanceId = 0;
    QPointer<QWidget> widget;
    bool exactObjectMatch = false;

    bool isValid() const { return instanceId > 0 && widget; }
};

class EditorShellRegistry
{
public:
    struct EditorTypeInfo
    {
        QString label;
        QString dockObjectPrefix;
        std::function<void()> create;
        std::function<void(const EditorShellSpec&)> recreate;
    };

    static constexpr int kNoFocus = -1;

    void registerEditorType(const QString& editorType, EditorTypeInfo info);
    bool hasEditorType(const QString& editorType) const;
    QStringList registeredEditorTypes() const;
    const EditorTypeInfo* editorTypeInfo(const QString& editorType) const;
    QString editorTypeForDock(const QString& dockObjectName) const;
    bool isEditorDock(const QString& dockObjectName) const;
    bool recreate(const EditorShellSpec& spec) const;

    int allocateShellId(const QString& editorType) const;
    void registerShell(const QString& editorType, int instanceId, QWidget* widget);
    void unregisterShell(const QString& editorType, int instanceId);
    QWidget* findShell(const QString& editorType, int instanceId) const;

    void setObjectKey(const QString& editorType, int instanceId, const QString& objectKey);
    QString objectKey(const QString& editorType, int instanceId) const;

    void setPinned(const QString& editorType, int instanceId, bool pinned);
    bool isPinned(const QString& editorType, int instanceId) const;

    void setVisible(const QString& editorType, int instanceId, bool visible);
    bool isVisible(const QString& editorType, int instanceId) const;

    int lastFocusedShellId(const QString& editorType) const;
    void setLastFocusedShellId(const QString& editorType, int instanceId);

    EditorShellRouteTarget routeTarget(const QString& editorType,
                                       const QString& objectKey = QString()) const;
    QList<EditorShellSpec> snapshot(const QString& editorType = QString()) const;

private:
    struct Entry
    {
        QString editorType;
        int instanceId = 0;
        QString objectKey;
        bool pinned = false;
        bool visible = false;
        QPointer<QWidget> widget;
    };

    Entry* findEntry(const QString& editorType, int instanceId);
    const Entry* findEntry(const QString& editorType, int instanceId) const;

    QStringList typeOrder_;
    QMap<QString, EditorTypeInfo> types_;
    QList<Entry> entries_;
    QMap<QString, int> lastFocused_;
};

}
