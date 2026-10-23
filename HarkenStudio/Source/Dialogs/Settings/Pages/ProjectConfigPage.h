#pragma once

#include "Dialogs/Settings/SettingsPage.h"
#include "Core/ProjectPlatform.h"
#include <memory>

#include <QList>
#include <QStringList>

class QListWidget;
class QFrame;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace VNX {

class TableWidget;
class ProjectPlatformEditor;

inline constexpr const char* kSettingsPageConfig = "config";

class ProjectConfigPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit ProjectConfigPage(QWidget* parent = nullptr);

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;
    bool apply() override;
    bool isDirty() const override;
    bool hasExternalChanges() const;
    bool validate();

private:
    void rememberBaseline();
    void refreshPlatforms(int selectedIndex = -1);
    void addPlatform();
    void selectPlatform(int index);
    void removePlatform();
    bool applyTargetLanguages();
    bool applyTargetPlatforms();
    void editTargetLanguage(QTreeWidgetItem* item = nullptr);
    void onRemoveTargetLanguageClicked();
    void setTargetLanguages(const QStringList& value);
    QJsonArray platformDraft() const;
    QStringList targetLanguages() const;

    QListWidget* platformList_ = nullptr;
    ProjectPlatformEditor* platformEditor_ = nullptr;
    QToolButton* platformRemoveButton_ = nullptr;
    QVector<ProjectPlatform> draftPlatforms_;
    std::weak_ptr<ProjectData> projectLease_;
    TableWidget* targetLanguagesBox_ = nullptr;
    QTreeWidget* targetLanguagesList_ = nullptr;
    QToolButton* targetLanguageRemoveBtn_ = nullptr;

    QJsonArray baselinePlatforms_;
    QStringList baselineProjectLanguages_;
};

}
