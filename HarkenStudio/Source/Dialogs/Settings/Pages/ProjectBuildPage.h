#pragma once

#include "Dialogs/Settings/SettingsPage.h"
#include "Core/ProjectPlatform.h"
#include <memory>

#include <QList>
#include <QStringList>

class QTreeWidget;
class QFrame;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace VNX {

class TableWidget;

inline constexpr const char* kSettingsPageBuild = "build";

class ProjectBuildPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit ProjectBuildPage(QWidget* parent = nullptr);

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;
    bool apply() override;
    bool isDirty() const override;

private:
    void rememberBaseline();
    void refreshPlatforms();
    void editPlatform(bool create);
    void removePlatform();
    bool applyTargetLanguages();
    bool applyTargetPlatforms();
    bool applyPackBuildOutputDir();
    void onAddTargetLanguageClicked();
    void onRemoveTargetLanguageClicked();
    void onPackBuildOutputDirBrowseClicked();
    void onPackBuildOutputDirEditingFinished();
    void setTargetLanguages(const QStringList& value);
    void clearTargetLanguageRows();
    void appendTargetLanguageRow(const QString& value);
    void selectTargetLanguageRow(int index);
    void updateTargetLanguageRowState();
    QJsonArray platformDraft() const;
    QStringList targetLanguages() const;

    QTreeWidget* platformList_ = nullptr;
    QToolButton* platformRemoveButton_ = nullptr;
    QVector<ProjectPlatform> draftPlatforms_;
    std::weak_ptr<ProjectData> projectLease_;
    TableWidget* targetLanguagesBox_ = nullptr;
    QVBoxLayout* targetLanguagesBoxLayout_ = nullptr;
    QToolButton* targetLanguageAddBtn_ = nullptr;
    QToolButton* targetLanguageRemoveBtn_ = nullptr;
    QList<QWidget*> targetLanguageRows_;
    QList<QLineEdit*> targetLanguageEdits_;
    int selectedTargetLanguageRow_ = -1;
    bool suppressTargetLanguageDirty_ = false;
    QLineEdit* packBuildOutputDirEdit_ = nullptr;
    QPushButton* packBuildOutputDirBrowseBtn_ = nullptr;

    QJsonArray baselinePlatforms_;
    QStringList baselineProjectLanguages_;
    QString baselinePackBuildOutputDir_;
};

}
