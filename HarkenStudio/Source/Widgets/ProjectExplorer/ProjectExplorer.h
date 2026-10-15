
#pragma once

#include <QHash>
#include <cstdint>

#include "Widgets/ProjectTree/ProjectTreePanel.h"

class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;

namespace VNX {

struct AssetInfo;
class ExplorerSection;

class ProjectExplorer : public ProjectTreePanel
{
    Q_OBJECT

public:
    explicit ProjectExplorer(QWidget* parent = nullptr);
    ~ProjectExplorer() override;

    void addAction(const QString& packName, const QString& actionName);
    bool pingSource(uint32_t sourceId);

    QTreeWidget* sourcesTree() const { return sourcesTree_; }
    ExplorerSection* assetsSection() const { return assetsSection_; }
    ExplorerSection* sourcesSection() const { return sourcesSection_; }

signals:
    void sourceEditorRequested(uint32_t sourceId);

protected:
    void setupPanelChrome() override;
    void prepareItemReveal(QTreeWidgetItem* item) override;
    QIcon decorateItemIcon(const QIcon& icon, const ProjectItem* item) const override;

private:
    QIcon iconWithColorChip(const QIcon& base, uint32_t color) const;
    void rebuildSourcesTree();
    void applySourcesVisibility();
    void updateSourceRow(QTreeWidgetItem* row, const AssetInfo& asset);
    QTreeWidgetItem* ensureSourcesFolder(const QString& path);
    void updateSectionLayout();
    void saveSectionLayout();

    QSplitter* sectionsSplitter_ = nullptr;
    ExplorerSection* assetsSection_ = nullptr;
    ExplorerSection* sourcesSection_ = nullptr;
    QWidget* collapsedSpace_ = nullptr;
    QList<int> expandedSizes_ = {300, 200};
    QTreeWidget* sourcesTree_ = nullptr;
    QHash<uint32_t, QTreeWidgetItem*> sourceRows_;
    QHash<QString, QTreeWidgetItem*> sourceFolders_;
};

}
