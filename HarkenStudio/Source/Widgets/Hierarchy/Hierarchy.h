
#pragma once

#include <QComboBox>

#include "Core/ItemTypeCatalog.h"
#include "Widgets/ProjectTree/ProjectTreePanel.h"

namespace VNX {

class Hierarchy : public ProjectTreePanel
{
    Q_OBJECT

public:
    explicit Hierarchy(QWidget* parent = nullptr);
    ~Hierarchy() override;

    void setFilter(ItemTypeCatalog::Bucket bucket);
    ItemTypeCatalog::Bucket currentFilter() const;

    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }

signals:
    void panelFocused(int panelId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

    void setupPanelChrome() override;
    void applyVisibility() override;
    void prepareItemReveal(QTreeWidgetItem* item) override;

private slots:
    void onFilterChanged(int index);

private:
    void installFocusFilterRecursively(QObject* root);

    QWidget* filterBar_;
    QComboBox* filterCombo_;

    int panelId_ = 0;
};

}
