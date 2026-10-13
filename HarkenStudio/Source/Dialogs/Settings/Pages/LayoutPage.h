
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace VNX {

inline constexpr const char* kSettingsPageLayout = "layout";

class LayoutPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit LayoutPage(QWidget* parent = nullptr);

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;
    bool apply() override;
    bool isDirty() const override;

signals:
    void layoutRenamed(const QString& oldName, const QString& newName);
    void layoutDeleted(const QString& name);

private:
    void populateLayoutTree();
    void updateLayoutButtons();
    void onLayoutRenameClicked();
    void onLayoutDeleteClicked();

    QTreeWidget* layoutTree_ = nullptr;
    QLabel* layoutImmediateStatusLabel_ = nullptr;
    QPushButton* layoutRenameButton_ = nullptr;
    QPushButton* layoutDeleteButton_ = nullptr;
};

}
