
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

#include <QList>
#include <QListWidget>
#include <QStackedWidget>
#include <QString>

class QLabel;

namespace VNX {

class SearchField;

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
    void applySearchFilter(const QString& text);

    SearchField* searchField_ = nullptr;
    QListWidget* categoryList_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QList<SettingsPage*> pages_;
};

}
