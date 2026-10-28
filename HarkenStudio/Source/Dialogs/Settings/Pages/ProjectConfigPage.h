#pragma once

#include "Dialogs/Settings/SettingsPage.h"
#include "Core/ProjectPlatform.h"
#include <memory>

#include <QList>
#include <QStringList>

class QListWidget;
class QFrame;
class QLineEdit;
class QToolButton;
class QLabel;

namespace VNX {

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
    void finishEditing();

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void refreshPlatforms(int selectedIndex = -1);
    void syncPlatforms(bool finishEditing = false, int selectedIndex = -1);
    void syncLanguages(bool finishEditing = false, int selectedIndex = -1);
    void updatePlatformAvailability();
    void commitPlatform(const ProjectPlatform& before);
    void addPlatform();
    void selectPlatform(int index);
    void removePlatform();
    bool commitLanguages(const QStringList& languages, int selectedIndex = -1);
    void commitLanguageName();
    void addTargetLanguage();
    void selectTargetLanguage(int index);
    void onRemoveTargetLanguageClicked();
    void setTargetLanguages(const QStringList& value, int selectedIndex = -1);
    bool hasCurrentProject() const;

    QListWidget* platformList_ = nullptr;
    ProjectPlatformEditor* platformEditor_ = nullptr;
    QToolButton* platformRemoveButton_ = nullptr;
    QToolButton* platformAddButton_ = nullptr;
    QLabel* platformBusyLabel_ = nullptr;
    QVector<ProjectPlatform> platforms_;
    std::weak_ptr<ProjectData> projectLease_;
    QListWidget* targetLanguagesList_ = nullptr;
    QWidget* targetLanguageFields_ = nullptr;
    QLineEdit* targetLanguageEdit_ = nullptr;
    QToolButton* targetLanguageRemoveBtn_ = nullptr;

    QString selectedLanguage_;
    bool committing_ = false;
};

}
