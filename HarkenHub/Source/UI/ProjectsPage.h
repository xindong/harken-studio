#pragma once

#include <QWidget>
#include <QHash>

#include <functional>
#include <vector>

#include "Versions/VersionScanner.h"
#include "Projects/ProjectProbe.h"

class QLabel;
class QColor;
class QComboBox;
class QJsonObject;
class QLineEdit;
class QPushButton;
class QSpacerItem;
class QStackedWidget;
class QTableWidget;
class QTimer;
class QTreeWidget;
class QVariantAnimation;

namespace VNH {

class ProjectStore;
class TableSorting;

class ProjectsPage : public QWidget
{
    Q_OBJECT

public:
    using VersionsProvider = std::function<std::vector<InstalledVersion>()>;

    explicit ProjectsPage(ProjectStore* store, QWidget* parent = nullptr);

    void setVersionsProvider(VersionsProvider provider) { versionsProvider_ = std::move(provider); }
    void refresh();
    void refreshVersions();
    void addProjectFromPath(const QString& path);
    void revealProject(const QString& canonicalPath);
    void showNewProjectPage();
    void showProjectsList();
    void showProjectLaunchGuidance(const QString& buildId);

signals:
    void projectsChanged();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct ProjectMetadata
    {
        ProjectProbeResult probe;
        qint64 size = -1;
        qint64 modifiedMs = 0;
    };

    void renderProjects();
    void refreshProjectMetadata();
    void onAddProjectClicked();
    void onNewProjectClicked();
    void onCreateProjectClicked();
    void onAddTemplateClicked();
    QWidget* createNewProjectPage();
    void loadProjectTemplates(const QString& preferredPath = QString());
    void loadProjectVersions();
    void validateSelectedTemplate();
    void showTemplatePreview(const QJsonObject& content,
                             const QString& description);
    void showEmptyTemplate();
    void persistProjectLocation(const QString& path);
    void updateNewProjectCreateEnabled();
    void updateLaunchTarget();
    void applyProjectLaunchGuidanceColor(const QColor& color);
    void clearProjectLaunchGuidance();
    void showRowMenu(int row, const QPoint& globalPos);
    void openProject(const QString& canonicalPath);

    ProjectStore* store_ = nullptr;
    QStackedWidget* viewStack_ = nullptr;
    QWidget* projectsView_ = nullptr;
    QWidget* newProjectView_ = nullptr;
    QTableWidget* table_ = nullptr;
    TableSorting* sorting_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QLineEdit* projectNameEdit_ = nullptr;
    QLineEdit* projectLocationEdit_ = nullptr;
    QComboBox* projectVersionCombo_ = nullptr;
    QComboBox* projectTemplateCombo_ = nullptr;
    QLabel* templateDescription_ = nullptr;
    QWidget* templatePreview_ = nullptr;
    QTreeWidget* templateBusTree_ = nullptr;
    QTreeWidget* templateAttenuationTree_ = nullptr;
    QSpacerItem* newProjectSpacer_ = nullptr;
    QPushButton* newProjectButton_ = nullptr;
    QPushButton* createProjectButton_ = nullptr;
    QTimer* projectLocationSaveTimer_ = nullptr;
    QTimer* launchTargetPollTimer_ = nullptr;
    QVariantAnimation* projectLaunchGuidanceAnimation_ = nullptr;
    QString projectLaunchGuidanceBuildId_;
    InstalledVersion createTarget_;
    bool creatingProject_ = false;
    std::vector<InstalledVersion> createVersions_;
    VersionsProvider versionsProvider_;
    QHash<QString, ProjectMetadata> projectMetadata_;
    bool metadataRefreshRunning_ = false;
    bool metadataRefreshPending_ = false;
};

}
