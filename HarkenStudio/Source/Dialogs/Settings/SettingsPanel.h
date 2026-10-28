
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

#include <QList>
#include <QListWidget>
#include <QStackedWidget>
#include <QString>

namespace VNX {

class SettingsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPanel(QWidget* parent = nullptr);

    bool showPage(const QString& pageId);
    QString currentPageId() const;
    QSize sizeHint() const override;

protected:
    void addPage(SettingsPage* page);
    void loadPages();

private:
    void onCategoryChanged(int index);

    QListWidget* categoryList_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
    QList<SettingsPage*> pages_;
};

}
