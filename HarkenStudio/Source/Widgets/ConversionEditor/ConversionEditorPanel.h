#pragma once

#include "Core/Components/VConversion.h"
#include "Core/ConversionResults.h"

#include <QHash>
#include <QTimer>
#include <QVariantMap>
#include <QWidget>

class QAction;
class QTreeWidgetItem;

namespace VNX {

struct ProjectItem;
class ListFooter;
class ProportionalTreeWidget;
class TableWidget;

class ConversionEditorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit ConversionEditorPanel(QWidget* parent = nullptr);
    ~ConversionEditorPanel() override;
    static bool acceptsItem(const ProjectItem* item);
    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }
    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);
    ProjectItem* boundItem() const;
    void bindItem(ProjectItem* item);
    void clearEditor();
    QVariantMap snapshotState() const;
    void restoreState(const QVariantMap& state);

signals:
    void panelFocused(int panelId);
    void pinToggled(bool pinned);
    void navigateRequested(const QUuid& itemGuid);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    void installFocusFilter(QObject* object);
    void refreshPreset();
    void rebuildPlatforms();
    void refreshSelection();
    QWidget* buildTierRow(const ConversionPlatformConfig& config, size_t index);
    void refreshResults();
    void commitConfig(const ConversionPlatformConfig& config);
    void addTier(const ProjectPlatformId& platform);
    void removeTier(const ProjectPlatformId& platform);
    void startConversion();
    void cancelConversion();
    const ConversionPlatformConfig* displayedConfig(const ProjectPlatformId& platform) const;

    int panelId_ = 0;
    bool pinned_ = false;
    bool refreshing_ = false;
    QUuid itemGuid_;
    std::vector<ConversionPlatformConfig> displayed_;
    ProjectPlatformId selectedPlatform_;
    int selectedTier_ = -1;
    TableWidget* platforms_ = nullptr;
    QList<QWidget*> tierRows_;
    QHash<ProjectPlatformId, ListFooter*> groupFooters_;
    ProportionalTreeWidget* results_ = nullptr;
    QAction* convert_ = nullptr;
    QTimer resultsRefresh_;
    std::shared_ptr<ConversionResults::Batch> batch_;
};

}
