
#pragma once

#include "Core/BaseDialog.h"
#include "Dialogs/Settings/SettingsPage.h"

#include <QList>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>

namespace VNX {

class SettingsShellDialog : public BaseDialog
{
    Q_OBJECT

public:
    explicit SettingsShellDialog(QWidget* parent = nullptr);

    bool showPage(const QString& pageId);

protected:
    void addPage(SettingsPage* page);
    void loadPages();
    virtual bool validateBeforeApply();

private:
    void onApplyClicked();
    void updateApplyEnabled();
    void onCategoryChanged(int index);
    bool anyDirty() const;

    QListWidget* categoryList_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QList<SettingsPage*> pages_;
};

}
