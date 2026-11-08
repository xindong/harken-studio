
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
    bool pingSource(uint32_t sourceId);

    QTreeWidget* sourcesTree() const { return sourcesTree_; }
    PaneSection* assetsSection() const { return assetsSection_; }
    PaneSection* sourcesSection() const { return sourcesSection_; }

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
    PaneSection* assetsSection_ = nullptr;
    PaneSection* sourcesSection_ = nullptr;
    QTreeWidget* sourcesTree_ = nullptr;
    QHash<uint32_t, QTreeWidgetItem*> sourceRows_;
    QHash<QString, QTreeWidgetItem*> sourceFolders_;
};

}
