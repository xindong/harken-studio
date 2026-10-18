
#pragma once

#include "Utils/ObjectId.h"

#include <QWidget>
#include <QDialog>
#include <QLabel>
#include <QTreeWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
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

    using PackGenerationResult = PackGenerationRecord;

    void addGenerationResult(const PackGenerationResult& result);
    void clearGenerationResults();

    static QString humanBytes(qint64 bytes);

signals:
    void exportRequested(const QStringList& platforms, const QStringList& packPaths,
                         bool includeUnchecked);
    void revealActionRequested(uint32_t objectID);

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

    QStringList selectedPackPaths() const;
    void requestExport(const QStringList& packPaths, bool includeUnchecked = false);
    void toggleUnitPlatforms(const QList<QTreeWidgetItem*>& items, bool include);

    static QString platformName(const QString& id);
    QString platformOutputDirFor(const QString& platform) const;
    void showFileInExplorer(const QString& path);

    QDialog*    settingsDialog_ = nullptr;

    ProportionalTreeWidget* packTree_ = nullptr;
    QLabel*       objectSendWarningLabel_ = nullptr;
    bool          packTreeUpdating_ = false;

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
