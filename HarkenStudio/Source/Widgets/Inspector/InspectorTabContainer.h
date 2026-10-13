
#pragma once

#include <QWidget>

class QBoxLayout;
class QButtonGroup;
class QStackedWidget;

namespace VNX {

class InspectorTabContainer : public QWidget
{
    Q_OBJECT
public:
    explicit InspectorTabContainer(QWidget* parent = nullptr);
    ~InspectorTabContainer() override = default;

    void addTab(QWidget* contentWidget, const QString& tabTitle, const QString& objectName);

    void clearTabs();

    void setCurrentTab(int index);
    int currentTabIndex() const;
    int tabCount() const;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void applyOrientation(bool sideBar);

    QBoxLayout*     rootLayout_ = nullptr;
    QWidget*        bar_ = nullptr;
    QBoxLayout*     barLayout_ = nullptr;
    QButtonGroup*   buttons_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    bool            sideBar_ = false;
};

}
