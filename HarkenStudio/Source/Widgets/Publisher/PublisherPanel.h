
#pragma once

#include "Utils/ObjectId.h"

#include <QWidget>
#include <QLabel>
#include <QTreeWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QStringList>
#include <QList>
#include <QPair>
#include <QHash>

#include <cstdint>

#include "../../Core/PackGenerationRecord.h"

class QAction;

namespace VNX {

struct ProjectData;
class MultiSelectComboBox;
class SplitButton;
class ProportionalTreeWidget;

class PublisherPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PublisherPanel(QWidget* parent = nullptr);
    ~PublisherPanel() override;

    void refreshPackList();

    static QStringList effectivePlatforms(const ProjectData* project);

    static QStringList effectiveSelectedPlatforms(const ProjectData* project,
                                                  const QStringList& selectedPlatforms);

    QStringList generationPlatforms() const;

    using PackGenerationResult = PackGenerationRecord;

    void addGenerationResult(const PackGenerationResult& result);
    void clearGenerationResults();

    static QString humanBytes(qint64 bytes);

signals:
    void exportRequested(const QStringList& platforms, const QStringList& packPaths,
                         bool includeUnchecked, bool forceReexport, bool selfCleaning);
    void revealActionRequested(uint32_t objectID);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void onExportClicked();
    void onPackTreeContextMenu(const QPoint& pos);
    void onPackItemChanged(QTreeWidgetItem* item, int column);

private:
    void setupUI();
    QWidget* createToolbar();
    void populateProjectPackTable();
    void requestPackTableRefresh();
    void updatePackProgress(const QString& platform, const QString& language, const QString& unitKey);
    void applyGenerationState(QTreeWidgetItem* item, const QString& platform,
                              const QString& language, const QString& unitKey);
    void populatePlatformControls();
    void applyNameFilter();
    void updateExportButton();
    void applyDeveloperColumns();

    void commitSelectedPlatforms(const QStringList& platforms);

    QStringList selectedPackPaths() const;
    void requestExport(const QStringList& packPaths, bool includeUnchecked = false,
                       bool forceReexport = false, bool selfCleaning = false);
    void toggleUnitPlatforms(const QList<QTreeWidgetItem*>& items, bool include);

    static QString platformName(const QString& id);
    QString platformOutputDirFor(const QString& platform) const;
    void showFileInExplorer(const QString& path);

    ProjectData* openedProject_ = nullptr;

    ProportionalTreeWidget* packTree_ = nullptr;
    QLabel*       objectSendWarningLabel_ = nullptr;
    bool          packTreeUpdating_ = false;

    bool packTableDirty_ = true;
    bool packTableRefreshQueued_ = false;
    QHash<QPair<QString, QString>, QTreeWidgetItem*> packRows_;
    bool cancelRequested_ = false;

    SplitButton*  exportBtn_ = nullptr;
    QAction*      exportAllAction_ = nullptr;
    QCheckBox*    forceReexportBox_ = nullptr;
    QCheckBox*    selfCleaningBox_ = nullptr;
    MultiSelectComboBox* platformCombo_ = nullptr;
    QLineEdit*    searchEdit_ = nullptr;
};

}
