
#pragma once

#include <QHash>
#include <cstdint>

#include "Widgets/ProjectTree/ProjectTreePanel.h"

class QTreeWidget;
class QTreeWidgetItem;

namespace VNX {

struct AssetInfo;
class PaneSection;

class ProjectExplorer : public ProjectTreePanel
{
    Q_OBJECT

public:
    explicit ProjectExplorer(QWidget* parent = nullptr);
    ~ProjectExplorer() override;

    void addAction(const QString& packName, const QString& actionName);
    bool pingResource(uint32_t sourceId);

    QTreeWidget* resourcesTree() const { return resourcesTree_; }
    PaneSection* assetsSection() const { return assetsSection_; }
    PaneSection* resourcesSection() const { return resourcesSection_; }

signals:
    void fileViewerRequested(uint32_t sourceId);

protected:
    void setupPanelChrome() override;
    void prepareItemReveal(QTreeWidgetItem* item) override;
    QIcon decorateItemIcon(const QIcon& icon, const ProjectItem* item) const override;

private:
    QIcon iconWithColorChip(const QIcon& base, uint32_t color) const;
    void rebuildResourcesTree();
    void applyResourcesVisibility();
    void updateResourceRow(QTreeWidgetItem* row, const AssetInfo& asset);
    QTreeWidgetItem* ensureResourceFolder(const QString& path);
    PaneSection* assetsSection_ = nullptr;
    PaneSection* resourcesSection_ = nullptr;
    QTreeWidget* resourcesTree_ = nullptr;
    QHash<uint32_t, QTreeWidgetItem*> resourceRows_;
    QHash<QString, QTreeWidgetItem*> resourceFolders_;
};

}
