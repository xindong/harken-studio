
#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QMenu>
#include <QAction>
#include <memory>
#include <vector>
#include <QMap>
#include <QSet>
#include <QUuid>

#include "Core/ProjectTypes.h"
#include "Core/ProjectManager.h"
#include "Core/AppSettings.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/ProjectTree/ProjectTreeWidget.h"
#include "Widgets/Shared/ProjectItemMenuController.h"

namespace VNX {

class ProjectObjectLabels;

class ProjectTreePanel : public QWidget
{
    Q_OBJECT
    friend class ProjectTreeWidget;

public:
    struct FolderRowAddress {
        bool    valid = false;
        bool    isBucketRoot = false;
        QString bucketFolderName;
        QString relPath;
    };

    explicit ProjectTreePanel(PanelId settingsPanelId, QWidget* parent = nullptr);
    ~ProjectTreePanel() override;

    void newProject(const QString& name = "Untitled");
    bool loadProject(const QString& path);
    bool saveProject(const QString& path);
    void clear();

    ProjectItem* selectedItem() const;
    QList<ProjectItem*> selectedItems() const;
    PhysicalFolder* selectedFolder() const;
    QList<PhysicalFolder*> selectedFolders() const;
    bool selectFolder(PhysicalFolder* folder);
    void setSelectedEntries(const QList<ProjectItem*>& items, const QList<PhysicalFolder*>& folders);
    void selectAll();
    void selectNone();

    void batchDelete();
    void batchMove(const QString& targetFolder);

    void rebuildBusTree();

    void selectBusInTree(uint32_t busID);
    bool selectProjectItem(ProjectItem* item);
    void setSelectedProjectItems(const QList<ProjectItem*>& items);
    bool pingProjectItem(ProjectItem* item);

    ProjectTreeWidget* treeWidget() const { return treeWidget_; }

    static bool wouldCreateCycle(const ProjectItem* item, const ProjectItem* target);

    FolderRowAddress folderAddressForRow(QTreeWidgetItem* row) const;
    static bool canAddressItemAt(const ProjectItem* item, const FolderRowAddress& address);

    void importFilesToTarget(const QStringList& files, ProjectItem* contextItem, const QString& bucketKey);

public slots:
    void onProjectOpened(ProjectData* project);
    void onProjectClosed();
    void onItemCreated(ProjectItem* item);
    void onItemRenamed(ProjectItem* item, const QString& oldName);
    void onItemDeleted(ProjectItem* item);
    void onItemMoved(ProjectItem* item, ProjectItem* oldParent, ProjectItem* newParent);
    void onItemPropertyChanged(ProjectItem* item, const QString& propertyName);
    void onFolderCreated(PhysicalFolder* folder);
    void onFolderDeleted(PhysicalFolder* folder);
    void onProjectSaved();

signals:
    void itemSelected(ProjectItem* item);
    void objectSelectionChanged(const QList<ProjectItem*>& items);
    void itemDoubleClicked(ProjectItem* item);
    void actionSelected(const QString& actionName);
    void audioFileSelected(const QString& filePath);
    void openInNewTabRequested(ProjectItem* item);
    void openInNewWindowRequested(ProjectItem* item);
    void openContainingFolderRequested(ProjectItem* item);

protected:
    void initPanel();

    virtual void setupPanelChrome() = 0;
    virtual QIcon decorateItemIcon(const QIcon& icon, const ProjectItem* item) const;
    virtual void applyVisibility();
    virtual void prepareItemReveal(QTreeWidgetItem* item) = 0;
    void updateSearchLabels();

    void rebuildTree();
    void pasteInto(ProjectItem* target);
    void setItemColorViaDialog(const QList<ProjectItem*>& items);
    void selectResultItems(const QList<ProjectItem*>& items);
    void prepareItemContextMenu(QTreeWidgetItem* item, ProjectItem* projItem);
    ProjectItem* menuAnchorItem() const;
    QList<ProjectItem*> menuAnchorItems() const;
    QList<ProjectItem*> uniqueMenuItems() const;
    QList<ProjectItem*> subtreeMenuItems() const;
    ProjectItemMenuController::MenuContext captureCommandContext() const;
    QString deriveFolderPathForBucket(const QString& bucketFolderName) const;
    QTreeWidgetItem* findTreeItem(ProjectItem* item) const;
    QTreeWidgetItem* createTreeItem(ProjectItem* item);
    void updateTreeItem(QTreeWidgetItem* treeItem, ProjectItem* projectItem);
    void setSubtreeExpanded(QTreeWidgetItem* item, bool expanded);
    QString typeToString(ProjectItem::Type type) const;

    ProjectItem* projectItemForTreeItem(QTreeWidgetItem* treeItem) const;
    QTreeWidgetItem* treeItemForProjectItem(ProjectItem* item) const;
    QString bucketKeyForTreeItem(QTreeWidgetItem* treeItem) const;

    void addToItemMap(QTreeWidgetItem* treeItem, ProjectItem* projectItem);
    void removeFromItemMap(QTreeWidgetItem* treeItem);

    SearchField* searchBox_ = nullptr;
    ProjectTreeWidget* treeWidget_ = nullptr;
    QMenu* contextMenu_ = nullptr;

    std::unique_ptr<ProjectItemMenuController> menuController_;

    QMap<QTreeWidgetItem*, ProjectItem*> itemMap_;
    QMap<ProjectItem*, QTreeWidgetItem*> reverseItemMap_;
    QMap<QTreeWidgetItem*, PhysicalFolder*> folderMap_;
    QMap<PhysicalFolder*, QTreeWidgetItem*> reverseFolderMap_;
    QMap<QString, QTreeWidgetItem*> categoryFolders_;

    ProjectItem* contextItem_ = nullptr;
    QString contextBucketFolder_;

protected slots:
    void onRename();
    void onDelete();
    void onDuplicate();
    void onCut();
    void onCopy();
    void onPaste();
    void onPasteProperties();
    void onNewAction(VActionStep::Type actionType);

private slots:
    void onItemClicked(QTreeWidgetItem* item, int column);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onItemExpanded(QTreeWidgetItem* item);
    void onItemCollapsed(QTreeWidgetItem* item);
    void onSearchTextChanged(const QString& text);
    void onContextMenu(const QPoint& pos);
    void onSelectionChanged();
    void onFontSettingsChanged(PanelId panelId, int size);

    void onSelectAll();
    void onSelectNone();
    void onInternalMoveRequested(ProjectItem* item,
                                 ProjectItem* newParent,
                                 const QString& bucketKey,
                                 const QString& folderPath,
                                 bool folderAddressValid);

    void onExternalFilesDropped(const QStringList& filePaths, QTreeWidgetItem* targetItem, const QString& bucketKey);

    void onNewParent(ProjectItem::Type containerType);

private:
    void setupUI();
    void setupTree();
    void setupContextMenu();
    void buildItemMenu();
    void applyMenuContext(const ProjectItemMenuController::MenuContext& context);
    void populateCreatedSubtree(ProjectItem* item, QTreeWidgetItem* row);

    QList<ProjectItem*> resolveMenuItems(const QList<QUuid>& guids) const;
    QList<PhysicalFolder*> menuFolders() const;
    bool hasSingleFolderTarget(const ProjectItemMenuController::MenuContext& ctx) const;
    QList<ProjectItem*> itemsInTreeOrder(const QList<ProjectItem*>& items) const;
    QList<QTreeWidgetItem*> expandTargetRows() const;
    bool hasSingleObjectTarget(const ProjectItemMenuController::MenuContext& ctx) const;
    bool canPasteAtContext(const ProjectItemMenuController::MenuContext& ctx) const;
    std::vector<ProjectItem::Type> newParentCandidateTypes(
        const QList<ProjectItem*>& selection) const;

    QTreeWidgetItem* ensureCategoryFolder(const QString& folderName);
    QTreeWidgetItem* ensureFolderRow(const PhysicalFolder* folder);
    void updateFolderRow(QTreeWidgetItem* row, PhysicalFolder* folder);
    void updateBucketRows();
    void setFolderExpanded(PhysicalFolder* folder, bool expanded);
    void updateFolderExpansionPaths();
    void saveFolderExpansion() const;
    PhysicalFolder* folderForTreeItem(QTreeWidgetItem* row) const;
    QTreeWidgetItem* treeItemForFolder(PhysicalFolder* folder) const;

    QTreeWidgetItem* bussesFolder_ = nullptr;
    QTreeWidgetItem* busTreeRootItem_ = nullptr;

    std::unique_ptr<ProjectObjectLabels> objectLabels_;
    bool visibilityRefreshPending_ = false;
    QString folderExpansionSettingsKey_;
    QSet<QString> collapsedFolderPaths_;
    QMap<const PhysicalFolder*, QString> folderRowPaths_;

    PanelId settingsPanelId_;
};

}
