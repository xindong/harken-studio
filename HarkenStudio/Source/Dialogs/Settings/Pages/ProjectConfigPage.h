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
    void addTargetLanguage();
    void selectTargetLanguage(int index);
    void onRemoveTargetLanguageClicked();
    void setTargetLanguages(const QStringList& value, int selectedIndex = -1);
    QJsonArray platformDraft() const;
    QStringList targetLanguages() const;

    QListWidget* platformList_ = nullptr;
    ProjectPlatformEditor* platformEditor_ = nullptr;
    QToolButton* platformRemoveButton_ = nullptr;
    QVector<ProjectPlatform> draftPlatforms_;
    std::weak_ptr<ProjectData> projectLease_;
    QListWidget* targetLanguagesList_ = nullptr;
    QWidget* targetLanguageFields_ = nullptr;
    QLineEdit* targetLanguageEdit_ = nullptr;
    QToolButton* targetLanguageRemoveBtn_ = nullptr;

    QJsonArray baselinePlatforms_;
    QStringList baselineProjectLanguages_;
};

}
