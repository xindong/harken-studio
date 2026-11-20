#pragma once
#include "TitleBarBase.h"

#include <QString>

class QLabel;
class QMenuBar;
class QHBoxLayout;
class QEvent;
class QResizeEvent;

namespace VNX {

class TitleBarWidget : public TitleBarBase {
    Q_OBJECT
public:
    explicit TitleBarWidget(QWidget* parent = nullptr);

    QMenuBar*    menuBar()     const { return menuBar_; }

    void embedMenuBar(QMenuBar* menuBar);

    void insertControlWidget(QWidget* widget);

    void setProjectTitle(const QString& name, bool modified);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    int syncContentMetrics(const ThemeMetrics& metrics) override;
    void updateContentGeometry() override;

private:
    void layoutVersionLabel();

    QHBoxLayout* menuLayout_{nullptr};
    QLabel*      logoLabel_{nullptr};
    QLabel*      versionLabel_{nullptr};
    QMenuBar*    menuBar_{nullptr};
    QString menuBarBaseStyleSheet_;
    QString appliedMenuBarStyleSheet_;
    QString projectName_;
    bool projectModified_{false};
};

}
