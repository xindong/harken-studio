#pragma once

#include <QList>
#include <QWidget>

class QAction;
class QActionEvent;
class QComboBox;
class QFrame;
class QPushButton;
class QToolButton;

namespace VNX {

class SplitButton final : public QWidget
{
    Q_OBJECT

public:
    explicit SplitButton(QWidget* parent = nullptr);

    QPushButton* primaryButton() const { return primaryButton_; }
    QToolButton* dropdownButton() const { return dropdownButton_; }

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void actionEvent(QActionEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void updateDropdownAvailability();
    void updateMetrics();
    void showPopup();

    QPushButton* primaryButton_;
    QToolButton* dropdownButton_;
    QFrame* divider_;
    QComboBox* popupCombo_;
    QList<QAction*> popupActions_;
};

}
