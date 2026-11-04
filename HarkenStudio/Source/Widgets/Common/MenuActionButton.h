#pragma once

#include <QWidget>

class QFrame;
class QMenu;
class QPushButton;
class QToolButton;

namespace VNX {

class MenuActionButton final : public QWidget
{
    Q_OBJECT

public:
    explicit MenuActionButton(QWidget* parent = nullptr);

    QPushButton* primaryButton() const { return primaryButton_; }
    QToolButton* dropdownButton() const { return dropdownButton_; }
    QMenu* menu() const { return menu_; }

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateMetrics();
    void showMenu();

    QPushButton* primaryButton_;
    QToolButton* dropdownButton_;
    QFrame* divider_;
    QMenu* menu_;
};

}
