
#pragma once

#include <QFrame>

class QButtonGroup;
class QHBoxLayout;
class QStackedWidget;

namespace harken::studio {

class InspectorTabContainer : public QFrame
{
    Q_OBJECT
public:
    enum class FramePlacement { Page, Group };

    explicit InspectorTabContainer(QWidget* parent = nullptr, FramePlacement frame = FramePlacement::Page);
    ~InspectorTabContainer() override = default;

    void addTab(QWidget* contentWidget, const QString& tabTitle, const QString& objectName);

    void clearTabs();

    void setCurrentTab(int index);
    void setTabVisible(int index, bool visible);
    int currentTabIndex() const;
    int tabCount() const;
    QWidget* tabWidget(int index) const;
    QSize sizeHint() const override;

signals:
    void currentChanged(int index);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    FramePlacement frame_;
    QWidget*        bar_ = nullptr;
    QHBoxLayout*    barLayout_ = nullptr;
    QButtonGroup*   buttons_ = nullptr;
    QWidget*        content_ = nullptr;
    QStackedWidget* stack_ = nullptr;
};

}
