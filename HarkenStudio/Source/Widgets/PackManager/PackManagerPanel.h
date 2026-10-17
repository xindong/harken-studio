
#pragma once

#include "Utils/ObjectId.h"

#include <QWidget>
#include <QDialog>
#include <QLabel>
#include <QTreeWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QStringList>
#include <QList>
#include <QPair>
#include <QHash>

#include <cstdint>

#include "../../Core/PackGenerationRecord.h"

class QAction;

namespace VNX {

struct ProjectData;
class SegmentedControl;
class SplitButton;
class ProportionalTreeWidget;

class PackManagerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PackManagerPanel(QWidget* parent = nullptr);
    ~PackManagerPanel() override;

    void refreshPackList();

    static QStringList effectivePlatforms(const ProjectData* project);

    static QStringList effectiveSelectedPlatforms(const ProjectData* project);

    QStringList generationPlatforms() const;

    static QList<uint32_t> objectIDsFromMime(const QMimeData* mime);

    using PackGenerationResult = PackGenerationRecord;

    void addGenerationResult(const PackGenerationResult& result);
    void clearGenerationResults();

    static QString humanBytes(qint64 bytes);

signals:
    void exportRequested(const QStringList& platforms, const QStringList& unitNames,
                         bool includeUnchecked);
    void revealActionRequested(uint32_t objectID);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onExportClicked();
    void onPackTreeContextMenu(const QPoint& pos);
    void onPackItemChanged(QTreeWidgetItem* item, int column);
    void onOpenSettingsDialog();

private:
    void setupUI();
    QWidget* createToolbar();
    void createSettingsDialog();
    void populateProjectPackTable();
    void populateSettings();
    void populatePlatformControls();
    void applyNameFilter();
    void updateExportButton();
    void applyDeveloperColumns();

    void commitSelectedPlatforms(const QStringList& platforms);

    QStringList selectedUnitNames() const;
    void requestExport(const QStringList& unitNames, bool includeUnchecked = false);
    void toggleUnitPlatform(QTreeWidgetItem* item, bool include);

    static QString platformName(const QString& id);
    QString platformOutputDirFor(const QString& platform) const;
    void showFileInExplorer(const QString& path);

    QString nextAutoGroupName() const;
    void groupActionObjects(const QList<uint32_t>& actionObjectIDs);
    void removeMembersFromGroups(const QList<QPair<QString, harkenstudio::ObjectId>>& members);
    void applyActionObjectsToGroup(const QList<uint32_t>& actionObjectIDs,
                                   const QString& groupName);
    void detachActionObjects(const QList<uint32_t>& actionObjectIDs);

    enum class DropKind { None, Group, Action, Root };
    struct DropTarget {
        DropKind kind = DropKind::None;
        QString groupName;
        uint32_t actionObjectId = 0;
    };
    DropTarget dropTargetAtPanelPos(const QPoint& panelPos) const;

    void setDropHoverItem(QTreeWidgetItem* item);

    QDialog*    settingsDialog_ = nullptr;

    ProportionalTreeWidget* packTree_ = nullptr;
    QLabel*       objectSendWarningLabel_ = nullptr;
    QTreeWidgetItem* dropHoverItem_ = nullptr;
    bool          packTreeUpdating_ = false;
    QList<uint32_t> dragActionIds_;

    QHash<QString, PackGenerationResult> lastResults_;

    SplitButton*  exportBtn_ = nullptr;
    QAction*      exportAllAction_ = nullptr;
    SegmentedControl* platformChipsRow_ = nullptr;
    bool          platformUpdating_ = false;
    QLineEdit*    searchEdit_ = nullptr;

    QLineEdit* buildOutputDirEdit_ = nullptr;
    QComboBox* conversionFailurePolicyCombo_ = nullptr;
    bool       settingsUpdating_ = false;
};

}
