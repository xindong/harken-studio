#pragma once

#include "TitleBarButton.h"

#include <QPointer>
#include <QWidget>

class QHBoxLayout;
class QScreen;

namespace VNX {

struct ThemeMetrics;

class TitleBarBase : public QWidget {
    Q_OBJECT
public:
    QAbstractButton* minButton() const { return btnMin_; }
    QAbstractButton* maxButton() const { return btnMax_; }
    QAbstractButton* closeButton() const { return btnClose_; }

protected:
    explicit TitleBarBase(Qt::WindowFlags controls, QWidget* parent);

    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

    void requestChromeMetricsSync();
    void syncChromeMetrics();
    virtual int syncContentMetrics(const ThemeMetrics& metrics);
    virtual void updateContentGeometry();

    QHBoxLayout* layout_;

private:
    void trackWindow();
    void trackScreen(QScreen* screen);
    void syncMaxButton();

    TitleBarButton* btnMin_{nullptr};
    TitleBarButton* btnMax_{nullptr};
    TitleBarButton* btnClose_{nullptr};
    QPointer<QWidget> trackedWindow_;
    QPointer<QScreen> trackedScreen_;
    bool metricsSyncQueued_{false};
};

}
