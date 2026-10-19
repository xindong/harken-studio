#pragma once
#include "TitleBarButton.h"

#include <QPointer>
#include <QWidget>

class QHBoxLayout;
class QEvent;
class QShowEvent;

namespace VNX {

class FloatingTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit FloatingTitleBar(QWidget* parent = nullptr);

    QPushButton* minButton()   const { return btnMin_; }
    QPushButton* maxButton()   const { return btnMax_; }
    QPushButton* closeButton() const { return btnClose_; }

    void updateMaxButton(bool maximized);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void trackWindow(QWidget* window);
    void syncMaxButton();

    QHBoxLayout* layout_{nullptr};
    TitleBarButton* btnMin_{nullptr};
    TitleBarButton* btnMax_{nullptr};
    TitleBarButton* btnClose_{nullptr};
    QPointer<QWidget> trackedWindow_;
};

}
