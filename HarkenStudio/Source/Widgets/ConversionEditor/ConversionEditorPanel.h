#pragma once

#include "Core/Components/VConversion.h"
#include "Core/ConversionResults.h"

#include <QTimer>
#include <QVariantMap>
#include <QWidget>

class QModelIndex;
class QPushButton;
class QShortcut;

namespace harken::studio {

struct ProjectItem;
class ConversionSizeSplits;
class PaneSectionStack;
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
    void updateConversionState();
    void refreshRules();
    void refreshResults();
    QModelIndex selectedRule() const;
    void selectRange(int index);
    void updateRuleSelection();
    void previewBoundary(int index, quint64 bytes);
    void editSplit(int index, quint64 bytes, bool insert);
    bool commitConfig(const ConversionConfig& config);
    void removeRule();
    void startConversion();
    void cancelConversion();

    int panelId_ = 0;
    bool pinned_ = false;
    bool refreshing_ = false;
    QUuid itemGuid_;
    std::optional<ConversionConfig> displayed_;
    PaneSectionStack* sections_ = nullptr;
    ConversionSizeSplits* splits_ = nullptr;
    TableWidget* rulesTable_ = nullptr;
    ProportionalTreeWidget* rules_ = nullptr;
    ProportionalTreeWidget* results_ = nullptr;
    QPushButton* convert_ = nullptr;
    QShortcut* cancelShortcut_ = nullptr;
    QTimer resultsRefresh_;
    std::shared_ptr<ConversionResults::Batch> batch_;
};

}
