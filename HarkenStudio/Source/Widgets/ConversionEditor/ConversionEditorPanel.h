#pragma once

#include "Core/Components/VConversion.h"
#include "Core/ConversionResults.h"

#include <QTimer>
#include <QVariantMap>
#include <QWidget>

class QAction;
class QComboBox;
class QSpinBox;
class QTreeWidgetItem;

namespace VNX {

struct ProjectItem;
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
    void refreshConfig();
    void refreshTiers();
    void refreshResults();
    void commitConfig(const ConversionConfig& config);
    void addTier();
    void removeTier();
    void startConversion();
    void cancelConversion();

    int panelId_ = 0;
    bool pinned_ = false;
    bool refreshing_ = false;
    QUuid itemGuid_;
    std::optional<ConversionConfig> displayed_;
    QWidget* fields_ = nullptr;
    QComboBox* format_ = nullptr;
    QSpinBox* quality_ = nullptr;
    TableWidget* tiersTable_ = nullptr;
    ProportionalTreeWidget* tiers_ = nullptr;
    ProportionalTreeWidget* results_ = nullptr;
    QAction* convert_ = nullptr;
    QTimer resultsRefresh_;
    std::shared_ptr<ConversionResults::Batch> batch_;
};

}
