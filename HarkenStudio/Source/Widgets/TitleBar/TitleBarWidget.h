#pragma once
#include "TitleBarButton.h"

#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QMenuBar;
class QHBoxLayout;
class QEvent;
class QResizeEvent;
class QShowEvent;
class QScreen;

namespace VNX {

class TitleBarWidget : public QWidget {
    Q_OBJECT
public:
    explicit TitleBarWidget(QWidget* parent = nullptr);

    QPushButton* minButton()   const { return btnMin_; }
    QPushButton* maxButton()   const { return btnMax_; }
    QPushButton* closeButton() const { return btnClose_; }
    QMenuBar*    menuBar()     const { return menuBar_; }

    void embedMenuBar(QMenuBar* menuBar);

    void insertControlWidget(QWidget* widget);

    void updateMaxButton(bool maximized);

    void setProjectTitle(const QString& name, bool modified);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void trackWindow(QWidget* window);
    void trackScreen(QScreen* screen);
    void syncMaxButton();
    void layoutVersionLabel();
    void requestChromeMetricsSync();
    void syncChromeMetrics();

    QHBoxLayout* layout_{nullptr};
    QHBoxLayout* menuLayout_{nullptr};
    QLabel*      logoLabel_{nullptr};
    QLabel*      versionLabel_{nullptr};
    QMenuBar*    menuBar_{nullptr};
    TitleBarButton* btnMin_{nullptr};
    TitleBarButton* btnMax_{nullptr};
    TitleBarButton* btnClose_{nullptr};
    QPointer<QWidget> trackedWindow_;
    QPointer<QScreen> trackedScreen_;
    QString menuBarBaseStyleSheet_;
    QString appliedMenuBarStyleSheet_;
    QString projectName_;
    bool projectModified_{false};
    bool metricsSyncQueued_{false};
};

}
