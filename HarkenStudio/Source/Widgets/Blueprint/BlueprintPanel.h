
#pragma once

#include <QWidget>
#include <QVariantMap>

class QToolButton;
class QMenu;
class QAction;

namespace VNX {

class BlueprintCanvasWidget;
class BlueprintVisibilityMenu;
class MeterPanel;

class BlueprintPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BlueprintPanel(QWidget* parent = nullptr);

    BlueprintCanvasWidget* canvas() const { return canvas_; }

signals:
    void meterDetachRequested(const QVariantMap& state);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateZoomText_(double factor);
    void refreshIcons_();
    void syncVisibility_();
    void syncFilter_();
    void openMenu_(QMenu* menu, QToolButton* button);
    void updateHeaderHeight_();
    void positionOverlays_();

    BlueprintCanvasWidget* canvas_      = nullptr;
    QWidget* toolbar_ = nullptr;
    MeterPanel* meter_ = nullptr;
    QWidget* meterResizeHandle_ = nullptr;
    bool resizingMeter_ = false;
    int meterPressX_ = 0;
    int meterPressWidth_ = 0;
    QToolButton* selectButton_ = nullptr;
    QToolButton* panButton_ = nullptr;
    QToolButton* snapButton_ = nullptr;
    QToolButton* filterButton_ = nullptr;
    QToolButton* visibilityButton_ = nullptr;
    QMenu* filterMenu_ = nullptr;
    QAction* filterSoundsAction_ = nullptr;
    BlueprintVisibilityMenu* visibilityMenu_ = nullptr;
    QToolButton*           resetButton_ = nullptr;
    QToolButton*           zoomButton_  = nullptr;
};

}
