
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

#include <functional>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace VNX {

inline constexpr const char* kSettingsPageLayout = "layout";

class LayoutPage : public SettingsPage
{
    Q_OBJECT

public:
    using SaveCurrentLayout = std::function<QString(QWidget*)>;

    explicit LayoutPage(QWidget* parent = nullptr, SaveCurrentLayout saveCurrentLayout = {});

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;

signals:
    void layoutRenamed(const QString& oldName, const QString& newName);
    void layoutDeleted(const QString& name);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void populateLayoutTree();
    void updateLayoutButtons();
    void onLayoutSaveClicked();
    void onLayoutRenameClicked();
    void onLayoutDeleteClicked();

    QTreeWidget* layoutTree_ = nullptr;
    QLabel* layoutImmediateStatusLabel_ = nullptr;
    QPushButton* layoutRenameButton_ = nullptr;
    QPushButton* layoutDeleteButton_ = nullptr;
    SaveCurrentLayout saveCurrentLayout_;
};

}
